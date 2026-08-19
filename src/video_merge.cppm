// video_merge.cppm — 视频 DASH 下载编排（音视频分离流 → ffmpeg 合并成 mp4）。
//
// b 站高画质是音视频分离的 DASH：一个视频要下两个流（video.m4s + audio.m4s）再
// 用 ffmpeg 合并。本模块的 MergeTracker 把这种「视频下载」编成**一个**任务呈现给
// UI：内部起两个 aria2 子任务（带各自 Referer/UA 头），对外聚合进单个合成
// TaskView；两个子任务都下完后后台线程跑 ffmpeg 合并、清理 .m4s。
//
// 领域层模块：不 import 任何 ui.*/eui。进程 spawn 复用 video_resolver 的
// findEngineBinary/runProcessLogged。线程模型：UI 线程调 snapshot/命令，housekeep
// 后台线程调 pollMerges，ffmpeg 在独立 detached 线程跑——jobs_ 一律经 mutex_ 访问。
//
// 已知局限：下载中重启 app 时，子任务被 aria2 会话恢复成普通 .m4s 任务、内存里的
// 任务表丢失 → 不自动合并（孤立分片按普通任务显示）。v1 接受。
module;

// eui 的 UI 唤醒（ffmpeg 完成线程改状态后让 UI 重绘一帧，跨线程安全）。
namespace core::platform { void requestUiUpdate(); }

export module tinynext.video_merge;

import std;
import tinynext.config;
import tinynext.download_engine;   // dl::DownloadEngine/TaskView/State/StartOptions
import tinynext.video_resolver;    // VideoInfo/VideoFormat + findEngineBinary/runProcessLogged
import tinynext.i18n;              // tr / trf（合并错误文案）
import tinynext.utils;             // pathFromUtf8 / utf8FromPath（中文标题的 path 转换）

namespace video {

export class MergeTracker {
public:
    // 一个 DASH 视频下载任务（两个 aria2 子任务 + 一次 ffmpeg 合并）。
    // 所有视频下载统一走 aria2（yt-dlp 只解析、不下载）；架构见 docs/roadmap.md。
    struct Job {
        std::uint64_t visibleId = 0;         // 合成任务 id（UI 所见）
        std::string title;
        std::string webpageUrl;
        std::filesystem::path outputPath;    // 最终 .mp4
        std::filesystem::path videoPath;     // <base>.video.m4s
        std::filesystem::path audioPath;     // <base>.audio.m4s
        std::uint64_t videoTaskId = 0;
        std::uint64_t audioTaskId = 0;
        enum class Phase { Downloading, Merging, Done, Failed } phase = Phase::Downloading;
        std::string error;
        bool keepParts = false;
        std::string audioExt;    // 配对音频流容器（mp4/m4a/webm…）：决定合并是否转码
        std::string audioCodec;  // 配对音频流编码（aac/opus/vorbis…）
    };

    // 文件名净化：替换 Windows/Unix 非法字符与控制字符为 _。public：TaskStore 的
    // 合流单文件命名也复用它。
    static std::string sanitizeFileName(const std::string& name) {
        std::string out;
        out.reserve(name.size());
        for (char c : name) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x20 || c == '/' || c == '\\' || c == '?' || c == '%' ||
                c == '*' || c == ':' || c == '|' || c == '"' || c == '<' || c == '>') {
                out += '_';
            } else {
                out += c;
            }
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
        if (out.empty()) out = "video";
        if (out.size() > 80) out = out.substr(0, 80);
        return out;
    }

    // 启动一个 DASH 视频下载：起 video+audio 两个 aria2 子任务。返回合成任务 id；
    // 失败返回 0。baseOpts 提供连接数等，头的部分由 format 覆盖。
    std::uint64_t startJob(dl::DownloadEngine& engine,
                           const VideoInfo& info,
                           const VideoFormat& format,
                           const std::filesystem::path& dir,
                           const dl::StartOptions& baseOpts,
                           bool keepParts) {
        if (format.videoUrl.empty() || format.audioUrl.empty()) return 0;  // 只处理 DASH

        const std::string base = sanitizeFileName(info.title.empty() ? "video" : info.title);
        Job job;
        job.title = info.title;
        job.webpageUrl = info.webpageUrl.empty() ? format.videoUrl : info.webpageUrl;
        // 文件名全程按 UTF-8 字符串持有（outputName 原样进 aria2 JSON 的 out），
        // 只在落盘 path 时经 pathFromUtf8 转宽——Windows 窄串构造会按 ANSI 代码页
        // 转码，中文标题直接抛异常/乱码。
        const std::string outName = uniqueName(dir, base, ".mp4");
        const std::string videoName = uniqueName(dir, base, ".video.m4s");
        const std::string audioName = uniqueName(dir, base, ".audio.m4s");
        job.outputPath = dir / pathFromUtf8(outName);
        job.videoPath = dir / pathFromUtf8(videoName);
        job.audioPath = dir / pathFromUtf8(audioName);
        job.keepParts = keepParts;
        job.audioExt = format.audioExt;
        job.audioCodec = format.audioCodec;

        dl::StartOptions vopts = baseOpts;
        vopts.outputName = videoName;
        vopts.headers = format.headers.extra;
        vopts.userAgent = format.headers.userAgent;
        vopts.referer = format.headers.referer;
        // YouTube 等 googlevideo CDN 对开放式 Range 首请求回 403（实测）：注入有限的
        // YouTube googlevideo CDN：必须单连接、但 **不加分段 Range**。加了 → aria2
        // 后续分段 Range 请求 403（YouTube URL 有时效），只下 ~2MB 就停。全量单
        // 连接一次 GET 通常能顺利完成。
        if (format.rangeBootstrap) {
            vopts.connections = 1;
        }
        job.videoTaskId = engine.start(format.videoUrl, job.videoPath, vopts);
        if (job.videoTaskId == 0) return 0;

        dl::StartOptions aopts = vopts;
        aopts.outputName = audioName;
        job.audioTaskId = engine.start(format.audioUrl, job.audioPath, aopts);
        if (job.audioTaskId == 0) {
            engine.remove(job.videoTaskId);  // 音频起不来则回滚视频子任务
            return 0;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        job.visibleId = nextVisibleId();
        subtaskToJob_[job.videoTaskId] = job.visibleId;
        subtaskToJob_[job.audioTaskId] = job.visibleId;
        jobs_.emplace(job.visibleId, std::move(job));
        return job.visibleId;
    }

    // 把引擎快照里的子任务滤掉、注入合成任务。UI 线程每帧调用，须立即返回。
    std::vector<dl::TaskView> mergeSnapshot(const std::vector<dl::TaskView>& engineTasks) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<dl::TaskView> out;
        out.reserve(engineTasks.size() + jobs_.size());
        for (const auto& t : engineTasks) {
            if (subtaskToJob_.find(t.id) == subtaskToJob_.end()) out.push_back(t);
        }
        for (const auto& [vid, job] : jobs_) {
            out.push_back(buildView(job, engineTasks));
        }
        return out;
    }

    // 是否为合成任务 id（UI 命令路由用）。
    bool isVideoTask(std::uint64_t id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return jobs_.find(id) != jobs_.end();
    }

    
    bool cancel(dl::DownloadEngine& engine, std::uint64_t id) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return false;
        // 读出子任务 id，释放锁再调引擎（引擎内部会取自己的 tasksMutex_）。
        const std::uint64_t v = it->second.videoTaskId;
        const std::uint64_t a = it->second.audioTaskId;
        lock.unlock();
        engine.cancel(v);
        engine.cancel(a);
        return true;
    }
    bool pause(dl::DownloadEngine& engine, std::uint64_t id) {
        return forSubtasks(id, [&](std::uint64_t sub) { engine.pause(sub); });
    }
    bool resume(dl::DownloadEngine& engine, std::uint64_t id) {
        return forSubtasks(id, [&](std::uint64_t sub) { engine.resume(sub); });
    }
    bool retry(dl::DownloadEngine& engine, std::uint64_t id) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(id);
            if (it == jobs_.end()) return false;
            it->second.phase = Job::Phase::Downloading;
            it->second.error.clear();
        }
        return forSubtasks(id, [&](std::uint64_t sub) { engine.retry(sub); });
    }
    // 删除记录：移除子任务 + 删半成品与成品。
    bool remove(dl::DownloadEngine& engine, std::uint64_t id) {
        Job job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(id);
            if (it == jobs_.end()) return false;
            job = it->second;
            subtaskToJob_.erase(job.videoTaskId);
            subtaskToJob_.erase(job.audioTaskId);
            jobs_.erase(it);
        }
        std::error_code ec;
        engine.remove(job.videoTaskId);
        engine.remove(job.audioTaskId);
        std::filesystem::remove(job.videoPath, ec);
        std::filesystem::remove(job.audioPath, ec);
        std::filesystem::remove(job.outputPath, ec);
        // aria2 控制文件也一并清掉（+= 拼扩展名，避免 .string() 走 ANSI 代码页）。
        std::error_code ec2;
        std::filesystem::remove(job.videoPath.wstring() + L".aria2", ec2);
        std::filesystem::remove(job.audioPath.wstring() + L".aria2", ec2);
        return true;
    }

    // housekeep 500ms 循环调用：发现两个子任务都 Done 的 Downloading 任务 → 触发合并；
    // 原生任务则检查 yt-dlp 线程是否跑完 → 直接转 Done/Failed。返回是否新变化（调用
    // 方据此唤醒 UI）。
    bool pollMerges(dl::DownloadEngine& engine) {
        const auto tasks = engine.snapshot();
        std::vector<std::uint64_t> toMerge;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [vid, job] : jobs_) {
                if (job.phase != Job::Phase::Downloading) continue;
                const dl::TaskView* v = find(tasks, job.videoTaskId);
                const dl::TaskView* a = find(tasks, job.audioTaskId);
                if (v && a && v->state == dl::State::Done && a->state == dl::State::Done) {
                    // aria2 的 --auto-file-renaming 可能在落盘时把同名文件改成
                    // "xxx (1).m4s"，而任务表里的是预设的原始名——合并前用任务真实
                    // destPath（aria2 报的 files[0].path）覆盖预设路径，否则 ffmpeg
                    // 会去开一个不存在的文件（实测：中文名重名下载，合并失败）。
                    job.videoPath = v->destPath;
                    job.audioPath = a->destPath;
                    job.phase = Job::Phase::Merging;
                    toMerge.push_back(vid);
                }
            }
        }
        for (const std::uint64_t vid : toMerge) spawnMerge(vid);
        return changed || !toMerge.empty();
    }

private:

    static std::uint64_t nextVisibleId() {
        // 合成 id 用高段，避免撞引擎的 nextId_（从 1 递增）。
        static std::atomic<std::uint64_t> counter{1'000'000};
        return counter++;
    }

    static const dl::TaskView* find(const std::vector<dl::TaskView>& tasks, std::uint64_t id) {
        for (const auto& t : tasks) if (t.id == id) return &t;
        return nullptr;
    }

    // 对两个子任务执行同一命令。id 不是合成任务则返回 false。
    template <typename Fn>
    bool forSubtasks(std::uint64_t id, Fn&& fn) {
        std::uint64_t v = 0, a = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(id);
            if (it == jobs_.end()) return false;
            v = it->second.videoTaskId;
            a = it->second.audioTaskId;
        }
        fn(v);
        fn(a);
        return true;
    }

    // 聚合两个子任务成一个合成 TaskView。
    dl::TaskView buildView(const Job& job, const std::vector<dl::TaskView>& tasks) const {
        dl::TaskView view{};
        view.id = job.visibleId;
        view.url = job.webpageUrl;
        view.destPath = job.outputPath;
        view.displayName = job.title;
        view.error = job.error;
        const dl::TaskView* v = find(tasks, job.videoTaskId);
        const dl::TaskView* a = find(tasks, job.audioTaskId);
        view.totalBytes = (v ? v->totalBytes : 0) + (a ? a->totalBytes : 0);
        view.downloadedBytes = (v ? v->downloadedBytes : 0) + (a ? a->downloadedBytes : 0);
        view.speedBps = (v ? v->speedBps : 0.0) + (a ? a->speedBps : 0.0);
        view.connections = (v ? v->connections : 0) + (a ? a->connections : 0);
        view.state = aggregateState(job, v, a);
        // 子任务任一来自会话恢复 → 合成任务也是历史恢复，不触发完成/失败通知。
        view.fromSession = (v && v->fromSession) || (a && a->fromSession);
        if (view.error.empty()) {
            if (v && !v->error.empty()) view.error = v->error;
            else if (a && !a->error.empty()) view.error = a->error;
        }
        return view;
    }

    static dl::State aggregateState(const Job& job, const dl::TaskView* v, const dl::TaskView* a) {
        if (job.phase == Job::Phase::Done) return dl::State::Done;
        if (job.phase == Job::Phase::Failed) return dl::State::Failed;
        if (job.phase == Job::Phase::Merging) return dl::State::Merging;
        // phase == Downloading：由子任务态推导。
        if (!v || !a) return dl::State::Queued;
        if (v->state == dl::State::Failed || a->state == dl::State::Failed) return dl::State::Failed;
        if (v->state == dl::State::Cancelled || a->state == dl::State::Cancelled) return dl::State::Cancelled;
        if (v->state == dl::State::Done && a->state == dl::State::Done) return dl::State::Merging;
        if (v->state == dl::State::Paused || a->state == dl::State::Paused) return dl::State::Paused;
        if (v->state == dl::State::Downloading || a->state == dl::State::Downloading) return dl::State::Downloading;
        return dl::State::Queued;
    }

    // 后台线程跑 ffmpeg 合并。不进锁阻塞；完成改状态 + 唤醒 UI。
    void spawnMerge(std::uint64_t visibleId) {
        Job job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = jobs_.find(visibleId);
            if (it == jobs_.end()) return;
            job = it->second;
        }
        std::thread([this, visibleId, job] {
            const std::string ffmpeg = findEngineBinary("ffmpeg");
            bool ok = false;
            std::string err;
            if (ffmpeg.empty()) {
                err = tr("vres.no_ffmpeg");
            } else {
                const std::filesystem::path log = job.outputPath.parent_path() / "tinynext-ffmpeg.log";
                // 音频是否 mp4 装得下：aac(m4a)/mp4 容器可 -c:a copy（b 站 m4s 即此，
                // 瞬时完成）；YouTube 的 opus/vorbis（webm）装不进 mp4，需转 aac。
                // 视频恒 copy（重编码视频太慢）。
                const bool transcodeAudio =
                    job.audioCodec.rfind("opus", 0) == 0 ||
                    job.audioCodec.rfind("vorbis", 0) == 0 ||
                    job.audioExt == "webm";
                const std::vector<std::string> args = {
                    "-y",
                    "-i", utf8FromPath(job.videoPath),
                    "-i", utf8FromPath(job.audioPath),
                    "-c:v", "copy",
                    "-c:a", transcodeAudio ? "aac" : "copy",
                    "-movflags", "+faststart",
                    utf8FromPath(job.outputPath),
                };
                const int code = runProcessLogged(ffmpeg, args, log, 600);
                std::error_code ec;
                ok = code == 0 && std::filesystem::exists(job.outputPath, ec) &&
                     std::filesystem::file_size(job.outputPath, ec) > 0;
                if (!ok) err = tr("vmerge.ffmpeg_failed");
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = jobs_.find(visibleId);
                if (it != jobs_.end()) {
                    it->second.phase = ok ? Job::Phase::Done : Job::Phase::Failed;
                    if (!ok) it->second.error = err;
                }
            }
            if (ok && !job.keepParts) {
                std::error_code ec;
                std::filesystem::remove(job.videoPath, ec);
                std::filesystem::remove(job.audioPath, ec);
                std::filesystem::remove(job.videoPath.wstring() + L".aria2", ec);
                std::filesystem::remove(job.audioPath.wstring() + L".aria2", ec);
            }
            core::platform::requestUiUpdate();
        }).detach();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Job> jobs_;
    std::unordered_map<std::uint64_t, std::uint64_t> subtaskToJob_;

    // 生成不存在的文件名（UTF-8 字符串；撞名加 " (n)"）。只返回名字不返回 path：
    // path 拼接经 pathFromUtf8，名字本身保持 UTF-8 供 outputName / ffmpeg 参数用。
    static std::string uniqueName(const std::filesystem::path& dir,
                                  const std::string& base,
                                  const std::string& ext) {
        for (int i = 0; i < 1000; ++i) {
            const std::string name = i == 0
                ? base + ext
                : base + " (" + std::to_string(i) + ")" + ext;
            std::error_code ec;
            if (!std::filesystem::exists(dir / pathFromUtf8(name), ec)) return name;
        }
        return base + ext;
    }

    };

} // namespace video
