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

// 执行 headless 下载：起 daemon → 逐个加入任务 → 轮询到全部结束 → shutdown。
// 返回进程 exit code（0 = 全部成功，1 = 任一失败或引擎不可用）。
export int run() {
    // 收集可下载源（与 CLI 白名单一致：http(s)/ftp(s)/sftp/magnet/.torrent）。
    std::vector<std::string> urls;
    for (const auto& a : commandLineArgs()) {
        if (a == "--headless") continue;
        if (isDownloadableSource(a) || a.ends_with(".torrent")) urls.push_back(a);
    }
    if (urls.empty()) {
        std::cerr << "tinynext: "
                  << tr("--headless 需要一个下载链接（http(s)/ftp(s)/sftp / magnet: / 本地 .torrent）",
                        "--headless needs a download link (http(s)/ftp(s)/sftp / magnet: / local .torrent)")
                  << "\n";
        return 1;
    }

    dl::Aria2Engine engine;
    const std::filesystem::path dir = cfg::downloadDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::cerr << "tinynext: "
              << trf("开始下载 {} 个任务 → {}", "Starting {} download(s) → {}",
                     urls.size(), dir.string())
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
        const std::uint64_t id = engine.start(url, dir / name, opts);
        if (id == 0) {
            const std::string err = engine.lastError();
            std::cerr << "tinynext: "
                      << trf("下载启动失败：{}：{}", "Download start failed: {}: {}",
                             (err.empty() ? tr("引擎不可用", "Engine unavailable") : err), url)
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
    const auto tasks = engine.snapshot();
    for (const auto& t : tasks) {
        if (t.state == dl::State::Done) {
            std::cerr << "tinynext: " << tr("[完成]", "[Done]") << " " << t.destPath.string() << "\n";
        } else if (t.state == dl::State::Failed) {
            std::cerr << "tinynext: " << tr("[失败]", "[Failed]") << " " << t.destPath.string()
                      << " — " << (t.error.empty() ? tr("未知错误", "Unknown error") : t.error) << "\n";
        } else if (t.state == dl::State::Cancelled) {
            std::cerr << "tinynext: " << tr("[已取消]", "[Cancelled]") << " " << t.destPath.string() << "\n";
        }
    }

    // 干净退出：saveSession（失败任务留档续传）+ forceShutdown daemon。
    engine.shutdown();
    return anyFailed ? 1 : 0;
}

} // namespace headless
