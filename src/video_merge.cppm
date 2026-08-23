// video_merge.cppm — 视频 DASH 下载编排 + yt-dlp 原生下载任务跟踪。
//
// 两种任务类型：
//   - Aria2Dash：bilibili 等站点的音视频分离 DASH → 两个 aria2 子任务 → ffmpeg 合并。
//   - YtDlpNative：YouTube 等 googlevideo CDN 站点 → yt-dlp 命令行下载
//     （--downloader native 单进程下载，yt-dlp 自行 DASH 合并；aria2c 委托
//     已被证明对 googlevideo 不可用——开放式 Range 请求首包即 403）。
//
// 对外统一呈现为合成任务 id，UI 层无感知。领域层模块：不 import 任何 ui.*/eui。
// 线程模型：UI 线程调 snapshot/命令，housekeep 后台线程调 pollMerges，ffmpeg 在
// 独立 detached 线程跑——jobs_ 一律经 mutex_ 访问。
module;

// eui 的 UI 唤醒（ffmpeg 完成线程改状态后让 UI 重绘一帧，跨线程安全）。
namespace core::platform { void requestUiUpdate(); }

export module tinynext.video_merge;

import std;
import tinynext.config;
import tinynext.download_engine;   // dl::DownloadEngine/TaskView/State/StartOptions
import tinynext.video_resolver;    // VideoInfo/VideoFormat/YtDlpProgress/startYtDlpDownload + findEngineBinary/runProcessLogged
import tinynext.i18n;              // tr / trf（合并错误文案）
import tinynext.utils;             // pathFromUtf8 / utf8FromPath（中文标题的 path 转换）

namespace video {

export class MergeTracker {
public:
    // 一个视频下载任务（DASH 双 aria2 子任务 或 yt-dlp 原生单进程）。
    struct Job {
        std::uint64_t visibleId = 0;         // 合成任务 id（UI 所见）
        std::string title;
        std::string webpageUrl;
        std::filesystem::path outputPath;    // 最终 .mp4
        // Aria2Dash 字段
        std::filesystem::path videoPath;     // <base>.video.m4s
        std::filesystem::path audioPath;     // <base>.audio.m4s
        std::uint64_t videoTaskId = 0;
        std::uint64_t audioTaskId = 0;
        // YtDlpNative 字段
        std::shared_ptr<YtDlpProgress> ytDlpProg;
        struct {
            std::string url;
            std::string jsRuntime;
            std::string outName;
            std::filesystem::path dir;
            std::string userAgent;
            std::string referer;
            std::filesystem::path logFile;
        } ytDlpParams;
        // 通用字段
        enum class Phase { Downloading, Merging, Done, Failed, Cancelled } phase = Phase::Downloading;
        enum class Kind { Aria2Dash, YtDlpNative } kind = Kind::Aria2Dash;
        std::string error;
        bool keepParts = false;
        std::string audioExt;
        std::string audioCodec;
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
        if (out.size() > 80) out = truncateUtf8Bytes(out, 80);
        return out;
    }

    // 启动一个 DASH 视频下载（Aria2Dash）：起 video+audio 两个 aria2 子任务。
    // 返回合成任务 id；失败返回 0。
    std::uint64_t startJob(dl::DownloadEngine& engine,
                           const VideoInfo& info,
                           const VideoFormat& format,
                           const std::filesystem::path& dir,
                           const dl::StartOptions& baseOpts,
                           bool keepParts) {
        if (format.videoUrl.empty() || format.audioUrl.empty()) return 0;

        const std::string base = sanitizeFileName(info.title.empty() ? "video" : info.title);
        Job job;
        job.kind = Job::Kind::Aria2Dash;
        job.title = info.title;
        job.webpageUrl = info.webpageUrl.empty() ? format.videoUrl : info.webpageUrl;
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
        if (format.rangeBootstrap) {
            vopts.connections = 1;
        }
        job.videoTaskId = engine.start(format.videoUrl, job.videoPath, vopts);
        if (job.videoTaskId == 0) return 0;

        dl::StartOptions aopts = vopts;
        aopts.outputName = audioName;
        job.audioTaskId = engine.start(format.audioUrl, job.audioPath, aopts);
        if (job.audioTaskId == 0) {
            engine.remove(job.videoTaskId);
            return 0;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        job.visibleId = nextVisibleId();
        subtaskToJob_[job.videoTaskId] = job.visibleId;
        subtaskToJob_[job.audioTaskId] = job.visibleId;
        jobs_.emplace(job.visibleId, std::move(job));
        return job.visibleId;
    }

    // 启动一个 yt-dlp 原生下载（YtDlpNative）：起 yt-dlp --downloader native。
    // 返回合成任务 id。失败返回 0。
    std::uint64_t startYtDlpJob(const std::string& url,
                                const std::string& jsRuntime,
                                const std::string& outName,
                                const std::filesystem::path& dir,
                                const std::string& userAgent,
                                const std::string& referer) {
        // 用 jsRuntimeSpec() 解析配置为完整 "runtime:path" 规格
        const std::string resolvedRuntime = jsRuntimeSpec();
        const std::string base = outName.empty() ? "video" : outName;
        const std::string finalName = uniqueName(dir, base, ".mp4");
        const std::filesystem::path outputPath = dir / pathFromUtf8(finalName);
        const std::filesystem::path logFile = cfg::configDir() / "tinynext-ytdlp-download.log";

        auto prog = std::make_shared<YtDlpProgress>();

        Job job;
        job.kind = Job::Kind::YtDlpNative;
        job.title = base;
        job.webpageUrl = url;
        job.outputPath = outputPath;
        job.ytDlpProg = prog;
        job.ytDlpParams = {url, resolvedRuntime, finalName, dir, userAgent, referer, logFile};

        // 启动 yt-dlp 子进程
        const bool ok = startYtDlpDownload(url, resolvedRuntime, finalName, dir,
                                           userAgent, referer, logFile, prog);
        if (!ok) return 0;

        std::lock_guard<std::mutex> lock(mutex_);
        job.visibleId = nextVisibleId();
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

    bool isVideoTask(std::uint64_t id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return jobs_.find(id) != jobs_.end();
    }

    // 是否存在仍在进行中的 yt-dlp 原生任务（TaskStore::busy 用它判断是否驱动
    // housekeep 进度轮询唤醒）。aria2 子任务的活动性由 engine_->busy() 覆盖。
    bool hasActiveJobs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [vid, job] : jobs_) {
            if (job.kind != Job::Kind::YtDlpNative || !job.ytDlpProg) continue;
            if (!job.ytDlpProg->finished.load() && !job.ytDlpProg->canceled.load()) {
                return true;
            }
        }
        return false;
    }

    bool cancel(dl::DownloadEngine& engine, std::uint64_t id) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return false;
        if (it->second.kind == Job::Kind::YtDlpNative) {
            if (it->second.ytDlpProg) {
                it->second.ytDlpProg->canceled.store(true);
            }
            return true;
        }
        const std::uint64_t v = it->second.videoTaskId;
        const std::uint64_t a = it->second.audioTaskId;
        lock.unlock();
        engine.cancel(v);
        engine.cancel(a);
        return true;
    }

    bool pause(dl::DownloadEngine& engine, std::uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return false;
        if (it->second.kind == Job::Kind::YtDlpNative) {
            // yt-dlp 不支持暂停，忽略
            return true;
        }
        engine.pause(it->second.videoTaskId);
        engine.pause(it->second.audioTaskId);
        return true;
    }

    bool resume(dl::DownloadEngine& engine, std::uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return false;
        if (it->second.kind == Job::Kind::YtDlpNative) {
            return true; // no-op
        }
        engine.resume(it->second.videoTaskId);
        engine.resume(it->second.audioTaskId);
        return true;
    }

    bool retry(dl::DownloadEngine& engine, std::uint64_t id) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end()) return false;
        if (it->second.kind == Job::Kind::YtDlpNative) {
            auto& p = it->second.ytDlpParams;
            auto& prog = it->second.ytDlpProg;
            // 重置进度
            prog = std::make_shared<YtDlpProgress>();
            it->second.phase = Job::Phase::Downloading;
            it->second.error.clear();
            const bool ok = startYtDlpDownload(p.url, p.jsRuntime, p.outName, p.dir,
                                               p.userAgent, p.referer, p.logFile, prog);
            lock.unlock();
            (void)ok;
            return true;
        }
        it->second.phase = Job::Phase::Downloading;
        it->second.error.clear();
        lock.unlock();
        engine.retry(it->second.videoTaskId);
        engine.retry(it->second.audioTaskId);
        return true;
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
        if (job.kind == Job::Kind::YtDlpNative) {
            // 取消进行中的 yt-dlp 进程
            if (job.ytDlpProg && !job.ytDlpProg->finished.load()) {
                job.ytDlpProg->canceled.store(true);
            }
            // 删输出文件
            std::error_code ec;
            std::filesystem::remove(job.outputPath, ec);
            return true;
        }
        std::error_code ec;
        engine.remove(job.videoTaskId);
        engine.remove(job.audioTaskId);
        std::filesystem::remove(job.videoPath, ec);
        std::filesystem::remove(job.audioPath, ec);
        std::filesystem::remove(job.outputPath, ec);
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

                if (job.kind == Job::Kind::YtDlpNative) {
                    auto& prog = job.ytDlpProg;
                    if (!prog) continue;
                    if (!prog->finished.load()) continue;
                    // yt-dlp 进程已退出
                    if (prog->canceled.load()) {
                        job.phase = Job::Phase::Cancelled;
                    } else if (prog->ok.load()) {
                        job.phase = Job::Phase::Done;
                        if (!prog->outputPath.empty()) {
                            job.outputPath = pathFromUtf8(prog->outputPath);
                        }
                    } else {
                        job.phase = Job::Phase::Failed;
                        job.error = prog->error;
                    }
                    changed = true;
                    continue;
                }

                // Aria2Dash：检查两个子任务
                const dl::TaskView* v = find(tasks, job.videoTaskId);
                const dl::TaskView* a = find(tasks, job.audioTaskId);
                if (v && a && v->state == dl::State::Done && a->state == dl::State::Done) {
                    job.videoPath = v->destPath;
                    job.audioPath = a->destPath;
                    job.phase = Job::Phase::Merging;
                    changed = true;
                    toMerge.push_back(vid);
                }
            }
        }
        for (const std::uint64_t vid : toMerge) {
            std::thread([this, vid] { spawnMerge(vid); }).detach();
        }
        return changed || !toMerge.empty();
    }

private:

    static std::uint64_t nextVisibleId() {
        static std::atomic<std::uint64_t> counter{1'000'000};
        return counter++;
    }

    static const dl::TaskView* find(const std::vector<dl::TaskView>& tasks, std::uint64_t id) {
        for (const auto& t : tasks) if (t.id == id) return &t;
        return nullptr;
    }

    // 聚合任务成一个合成 TaskView。
    dl::TaskView buildView(const Job& job, const std::vector<dl::TaskView>& tasks) const {
        dl::TaskView view{};
        view.id = job.visibleId;
        view.url = job.webpageUrl;
        view.destPath = job.outputPath;
        view.displayName = job.title;
        view.error = job.error;

        if (job.kind == Job::Kind::YtDlpNative) {
            const auto& prog = job.ytDlpProg;
            if (prog) {
                // 单调进度：累计已完成的阶段字节 + 当前阶段已下载字节
                const std::int64_t phaseAcc = prog->phaseCompletedBytes.load();
                const std::int64_t curDl = prog->downloadedBytes.load();
                view.downloadedBytes = phaseAcc + curDl;
                // 总大小 = 累计已完成 + 当前阶段总量
                const std::int64_t curTotal = prog->totalBytes.load();
                view.totalBytes = curTotal > 0 ? phaseAcc + curTotal : phaseAcc;
                view.speedBps = static_cast<double>(prog->speedBps.load());
                // yt-dlp 是单进程单流下载，没有 aria2 那样的「连接数」概念——
                // 置 0 让卡片信息行跳过连接数（显示恒为 1 只是噪音）。
                view.connections = 0;
                view.state = ytDlpState(job, *prog);
                view.fromSession = false;
                if (view.error.empty() && !prog->error.empty() && prog->finished.load() && !prog->ok.load()) {
                    view.error = prog->error;
                }
                // 信息行展示区分解析阶段/合并阶段：yt-dlp 原生任务在解析格式时显示
                // "解析中"，在 ffmpeg 合并阶段显示进度（卡片用 Merging 态已覆盖）。
                // 这里额外在 Downloading 时标记 progressState，供卡片信息行显示
                // 更贴切的状态（如 yt-dlp [Merger] 阶段）。
                view.progressState = prog->merging.load() ? dl::State::Merging : view.state;
            } else {
                view.state = dl::State::Queued;
            }
            return view;
        }

        // Aria2Dash
        const dl::TaskView* v = find(tasks, job.videoTaskId);
        const dl::TaskView* a = find(tasks, job.audioTaskId);
        view.totalBytes = (v ? v->totalBytes : 0) + (a ? a->totalBytes : 0);
        view.downloadedBytes = (v ? v->downloadedBytes : 0) + (a ? a->downloadedBytes : 0);
        view.speedBps = (v ? v->speedBps : 0.0) + (a ? a->speedBps : 0.0);
        view.connections = (v ? v->connections : 0) + (a ? a->connections : 0);
        view.state = aggregateState(job, v, a);
        view.progressState = view.state;
        view.fromSession = (v && v->fromSession) || (a && a->fromSession);
        if (view.error.empty()) {
            if (v && !v->error.empty()) view.error = v->error;
            else if (a && !a->error.empty()) view.error = a->error;
        }
        return view;
    }

    static dl::State ytDlpState(const Job& job, const YtDlpProgress& prog) {
        if (job.phase == Job::Phase::Done) return dl::State::Done;
        if (job.phase == Job::Phase::Failed) return dl::State::Failed;
        if (job.phase == Job::Phase::Cancelled) return dl::State::Cancelled;
        if (prog.finished.load()) {
            if (prog.canceled.load()) return dl::State::Cancelled;
            return prog.ok.load() ? dl::State::Done : dl::State::Failed;
        }
        if (prog.canceled.load()) return dl::State::Cancelled;
        if (prog.merging.load()) return dl::State::Merging;
        if (prog.started.load()) return dl::State::Downloading;
        return dl::State::Queued;
    }

    static dl::State aggregateState(const Job& job, const dl::TaskView* v, const dl::TaskView* a) {
        if (job.phase == Job::Phase::Done) return dl::State::Done;
        if (job.phase == Job::Phase::Failed) return dl::State::Failed;
        if (job.phase == Job::Phase::Merging) return dl::State::Merging;
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
        const std::string ffmpeg = findEngineBinary("ffmpeg");
        bool ok = false;
        std::string err;
        if (ffmpeg.empty()) {
            err = tr("vres.no_ffmpeg");
        } else {
            const std::filesystem::path log = job.outputPath.parent_path() / "tinynext-ffmpeg.log";
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
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Job> jobs_;
    std::unordered_map<std::uint64_t, std::uint64_t> subtaskToJob_;

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