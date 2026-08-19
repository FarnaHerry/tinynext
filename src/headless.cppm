// headless.cppm — `tinynext --headless <url>` 脚本模式。
//
// 不开窗、按 TinyNext 自身配置（下载目录 / 连接数 / aria2 参数）下载完退出，
// exit 0/1。面向「已用 TinyNext 的人」写脚本 / 定时任务。复用 aria2_engine 的
// daemon + JSON-RPC，不依赖 eui / UI 状态（headless 是纯引擎驱动的下载器）。
//
// 接线：cli.cppm 的 CliBoot 在 main 之前检测 --headless 并调 headless::run()，
// 不抢单实例锁、不进 GUI、不转发 URL —— headless 独立起自己的 daemon 下载。
// 失败的任务保留在会话文件（下次 GUI 启动 aria2 控制文件续传），符合断点续传语义。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // GetCommandLineW / LocalFree
#elif defined(__APPLE__)
#include <crt_externs.h>  // _NSGetArgc/_NSGetArgv
#endif

export module tinynext.headless;

import std;
import tinynext.config;
import tinynext.download_engine;
import tinynext.aria2_engine;
import tinynext.i18n;   // tr / trf（终端输出按语言）
import tinynext.utils;  // isDownloadableSource / fileNameFromUrl（纯 std，无 eui 依赖）
import tinynext.video_resolver;  // --resolve 视频解析（yt-dlp）
import tinynext.store.tasks;     // --video-dl 走 TaskStore（含 MergeTracker 合并编排）

namespace headless {

namespace {

// 全部命令行参数（不含 exe 路径）。headless 独立于 cli.cppm 解析，避免依赖 UI
// 状态模块。静态缓存一次。
std::vector<std::string> commandLineArgs() {
    static const std::vector<std::string> cached = [] {
        std::vector<std::string> args;
#ifdef _WIN32
        using CmdToArgvFn = LPWSTR*(WINAPI*)(LPCWSTR, int*);
        static const CmdToArgvFn cmdToArgv = []() -> CmdToArgvFn {
            HMODULE m = LoadLibraryW(L"shell32.dll");
            if (!m) return nullptr;
            return reinterpret_cast<CmdToArgvFn>(
                reinterpret_cast<void*>(GetProcAddress(m, "CommandLineToArgvW")));
        }();
        if (cmdToArgv) {
            int argc = 0;
            LPWSTR* wargv = cmdToArgv(GetCommandLineW(), &argc);
            if (wargv) {
                for (int i = 1; i < argc; ++i) {
                    const std::wstring w(wargv[i]);
                    args.push_back(std::string(w.begin(), w.end()));
                }
                LocalFree(static_cast<HLOCAL>(wargv));
            }
        }
#elif defined(__APPLE__)
        const int argc = *_NSGetArgc();
        char** argv = *_NSGetArgv();
        for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
#else
        // Linux: /proc/self/cmdline (NUL-separated); argv[0] is the exe path.
        std::ifstream in("/proc/self/cmdline", std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
        std::size_t start = 0;
        while (start < s.size()) {
            const std::size_t end = s.find('\0', start);
            const std::string tok = s.substr(start, end - start);
            if (start > 0) args.push_back(tok);  // skip argv[0]
            if (end == std::string::npos) break;
            start = end + 1;
        }
#endif
        return args;
    }();
    return cached;
}

// 任务最终态：Done = 成功；Failed/Cancelled = 失败。映射到 exit code。
int exitCodeFor(dl::State s) {
    return s == dl::State::Done ? 0 : 1;
}

} // namespace

// 命令行是否含 --headless（CliBoot 在 main 之前调用）。
export bool requested() {
    static const bool cached = [] {
        for (const auto& a : commandLineArgs()) {
            if (a == "--headless") return true;
        }
        return false;
    }();
    return cached;
}

// 命令行是否含 --resolve（视频解析脚本模式：只解析打印画质列表，不下载不开窗）。
export bool resolveRequested() {
    static const bool cached = [] {
        for (const auto& a : commandLineArgs()) {
            if (a == "--resolve") return true;
        }
        return false;
    }();
    return cached;
}

// 执行视频解析：`tinynext --resolve <视频页URL>` → 打印标题 + 各画质（含是否
// DASH 需合并）。用设置里的 SESSDATA。返回 0 = 解析成功，1 = 失败。
export int runResolve() {
    std::string url;
    for (const auto& a : commandLineArgs()) {
        if (a == "--resolve") continue;
        if (a.starts_with("http://") || a.starts_with("https://")) { url = a; break; }
    }
    if (url.empty()) {
        std::cerr << "tinynext: "
                  << tr("cli.resolve_need_url")
                  << "\n";
        return 1;
    }
    const std::string proxy = cfg::aria2Config().proxy;
    const video::ResolveResult r = video::resolveVideoUrl(url, cfg::videoConfig().bilibiliCookie, proxy);
    if (!r.ok || !r.info.has_value()) {
        std::cerr << "tinynext: "
                  << trf("cli.resolve_failed", r.error) << "\n";
        return 1;
    }
    const video::VideoInfo& info = *r.info;
    std::cout << tr("cli.title") << info.title << "\n";
    std::cout << trf("cli.qualities_count", info.formats.size()) << "\n";
    for (const auto& f : info.formats) {
        std::cout << "  " << f.label << " · " << (f.ext.empty() ? "mp4" : f.ext);
        if (f.filesizeApprox > 0) std::cout << " · ~" << formatBytes(f.filesizeApprox);
        std::cout << (f.audioUrl.empty() ? tr("cli.combined_no_merge")
                                         : tr("cli.dash_needs_merge"))
                  << "\n";
    }
    return 0;
}

// 命令行是否含 --video-dl（视频下载脚本模式：解析 + 下载 + DASH 自动合并，不开窗）。
export bool videoDlRequested() {
    static const bool cached = [] {
        for (const auto& a : commandLineArgs()) {
            if (a == "--video-dl") return true;
        }
        return false;
    }();
    return cached;
}

// 执行视频下载：`tinynext --video-dl <视频页URL> [画质关键词]`。画质关键词按
// label 子串匹配（如 "1080"），缺省用设置的默认画质、再缺省最高画质。全程走
// g_tasks（TaskStore）：DASH 经 MergeTracker 聚合成一个任务并自动 ffmpeg 合并。
// 返回 0 = 完成，1 = 解析/下载/合并失败。
export int runVideoDownload() {
    std::string url;
    std::string quality;
    for (const auto& a : commandLineArgs()) {
        if (a == "--video-dl") continue;
        if (a.starts_with("http://") || a.starts_with("https://")) { url = a; continue; }
        if (!a.empty() && a.front() != '-' && quality.empty()) quality = a;
    }
    if (url.empty()) {
        std::cerr << "tinynext: "
                  << tr("cli.video_dl_need_url")
                  << "\n";
        return 1;
    }

    const std::string proxy = cfg::aria2Config().proxy;
    const video::ResolveResult r = video::resolveVideoUrl(url, cfg::videoConfig().bilibiliCookie, proxy);
    if (!r.ok || !r.info.has_value()) {
        std::cerr << "tinynext: "
                  << trf("cli.resolve_failed", r.error) << "\n";
        return 1;
    }
    const video::VideoInfo& info = *r.info;

    // 选画质：命令行关键词 > 设置默认画质 > 最高（formats 已按高度降序）。
    std::string want = quality.empty() ? cfg::videoConfig().defaultQuality : quality;
    int pick = 0;
    if (!want.empty()) {
        for (int i = 0; i < static_cast<int>(info.formats.size()); ++i) {
            if (info.formats[i].label.find(want) != std::string::npos) { pick = i; break; }
        }
    }
    const video::VideoFormat& format = info.formats[pick];
    std::cerr << "tinynext: " << tr("cli.resolved") << info.title << "\n"
              << "tinynext: " << trf("cli.quality_label", format.label)
              << (format.audioUrl.empty()
                        ? tr("cli.combined")
                        : tr("cli.dash_auto_merge"))
              << "\n";

    // 走 TaskStore 完整链路（含 MergeTracker 的 DASH 编排与 ffmpeg 合并）。
    g_tasks.warmup();
    if (!g_tasks.engineActive()) {
        std::cerr << "tinynext: "
                  << trf("cli.engine_start_failed", g_tasks.lastError())
                  << "\n";
        return 1;
    }
    const StartResult sr = g_tasks.startVideoDownload(info, format, dl::StartOptions{});
    if (!sr.ok) {
        std::cerr << "tinynext: " << sr.message << "\n";
        g_tasks.shutdown();
        return 1;
    }
    std::cerr << "tinynext: " << sr.message << "\n";

    // 轮询到任务离开活动状态（Merging 也算活动：等 ffmpeg 合并完）。
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        g_tasks.pollProgress();
        g_tasks.pollVideoMerges();
        const auto tasks = g_tasks.snapshot();
        const dl::TaskView* t = nullptr;
        for (const auto& v : tasks) {
            if (v.id == sr.id) { t = &v; break; }
        }
        if (!t) continue;  // 会话恢复竞态下可能暂时缺帧，继续等
        if (t->state == dl::State::Done) {
            std::cerr << "tinynext: " << tr("cli.done_tag") << " "
                      << utf8FromPath(t->destPath) << "\n";
            g_tasks.shutdown();
            return 0;
        }
        if (t->state == dl::State::Failed || t->state == dl::State::Cancelled) {
            std::cerr << "tinynext: " << tr("cli.failed_tag") << " "
                      << (t->error.empty() ? tr("cli.unknown_error") : t->error)
                      << "\n";
            g_tasks.shutdown();
            return 1;
        }
    }
}

// 执行 headless 下载：起 daemon → 逐个加入任务 → 轮询到全部结束 → shutdown。
// 返回进程 exit code（0 = 全部成功，1 = 任一失败或引擎不可用）。
export int run() {
    // 收集可下载源（与 CLI 白名单一致：http(s)/ftp(s)/sftp/magnet/.torrent）。
    std::vector<std::string> urls;
    for (const auto& a : commandLineArgs()) {
        if (a == "--headless") continue;
        if (isLikelyVideoPageUrl(a)) {
                std::cerr << "tinynext: "
                          << tr("cli.video_dl_hint")
                          << "\n";
                continue;
            }
            if (isDownloadableSource(a) || a.ends_with(".torrent")) urls.push_back(a);
    }
    if (urls.empty()) {
        std::cerr << "tinynext: "
                  << tr("cli.headless_need_url")
                  << "\n";
        return 1;
    }

    dl::Aria2Engine engine;
    const std::filesystem::path dir = cfg::downloadDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::cerr << "tinynext: "
              << trf("cli.starting_n",
                     urls.size(), utf8FromPath(dir))
              << "\n";

    for (const auto& url : urls) {
        dl::StartOptions opts;
        std::string name;
        if (url.ends_with(".torrent")) {
            opts.torrentPath = url;
            name = "torrent";
        } else if (url.starts_with("magnet:")) {
            name = "magnet";  // 占位名；拿到元数据后引擎从 files[0].path 更新为真实名
        } else {
            name = fileNameFromUrl(url);
        }
        if (name.empty()) name = "torrent";
        // name 是 UTF-8（URL 文件名），path 拼接经 pathFromUtf8。
        const std::uint64_t id = engine.start(url, dir / pathFromUtf8(name), opts);
        if (id == 0) {
            const std::string err = engine.lastError();
            std::cerr << "tinynext: "
                      << trf("cli.dl_start_failed2",
                             (err.empty() ? tr("err.engine_unavailable") : err), url)
                      << "\n";
            engine.shutdown();
            return 1;
        }
        std::cerr << "tinynext:   + " << name << "\n";
    }

    // 轮询到所有任务离开活动状态（排队/进行/暂停）。aria2 状态迁移走 WS 事件
    // 即时，进度由 pollProgress（内部 ~1s 轮询补进度）显式刷新，snapshot 已是纯
    // 读缓存，这里每 500ms 查一次足够。
    bool anyFailed = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        engine.pollProgress();
        const auto tasks = engine.snapshot();
        bool active = false;
        for (const auto& t : tasks) {
            if (t.state == dl::State::Queued || t.state == dl::State::Downloading ||
                t.state == dl::State::Paused) {
                active = true;
            } else if (t.state == dl::State::Failed) {
                anyFailed = true;
            }
        }
        if (!active) break;
    }

    // 汇总结果（打印到 stderr，脚本可 redirect；stdout 留给需要解析的调用方）。
    // 路径经 utf8FromPath 打印（Windows 窄串会走 ANSI 代码页，中文路径乱码）。
    const auto tasks = engine.snapshot();
    for (const auto& t : tasks) {
        // 取路径字符串：优先用预编码的 destPathUtf8（避免读可能悬空的 destPath）。
        // 回退 utf8FromPath（两者都空则空串）。
        const std::string p = !t.destPathUtf8.empty() ? t.destPathUtf8 : utf8FromPath(t.destPath);
        if (t.state == dl::State::Done) {
            std::cerr << "tinynext: " << tr("cli.done_tag") << " " << p << "\n";
        } else if (t.state == dl::State::Failed) {
            std::cerr << "tinynext: " << tr("cli.failed_tag") << " " << p
                      << " — " << (t.error.empty() ? tr("cli.unknown_error") : t.error) << "\n";
        } else if (t.state == dl::State::Cancelled) {
            std::cerr << "tinynext: " << tr("cli.cancelled_tag") << " " << p << "\n";
        }
    }

    // 干净退出：saveSession（失败任务留档续传）+ forceShutdown daemon。
    engine.shutdown();
    return anyFailed ? 1 : 0;
}

} // namespace headless
