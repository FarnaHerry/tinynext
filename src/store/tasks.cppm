// store/tasks.cppm — 领域 store：任务数据与下载命令的唯一入口（JS 语境的
// "store" = 状态 + 只允许通过方法变更状态）。UI 无关：不 import 任何 eui / ui.*
// 模块，GUI / CLI / headless / 未来的其他前端共用。
//
// 纪律：
//   - 引擎对象由 TaskStore 持有，外部不再直接碰 dl::DownloadEngine 指针；
//   - startFromUrl 返回 StartResult{ok, message}，不自己弹状态消息——消息展示
//     是 UI 层的事（store.ui 的 showStatus / headless 的 stdout）；
//   - 线程模型继承引擎层：warmup 可从后台线程调用（引擎内部 daemonMutex_
//     互斥），其余命令在 UI 线程调用；snapshot 内部有锁，任意线程可读。
//
// Boot-order note: g_tasks 的动态初始化（构造引擎对象）先于任何 import 本模块
// 的 TU 的静态初始化执行。主实例没问题；第二实例在 CLI boot 退出前会短暂构造
// 一次引擎对象——与旧 ui/state.cppm 的 g_manager 相同，无害（不 spawn daemon）。
export module tinynext.store.tasks;

import std;
import tinynext.config;
import tinynext.aria2_engine;
import tinynext.download_engine;
import tinynext.i18n;   // tr / trf（结果消息按语言）
import tinynext.utils;
import tinynext.video_resolver;  // VideoInfo/VideoFormat（startVideoDownload 入参）
import tinynext.video_merge;     // MergeTracker（DASH 音视频合并编排）

// 任务显示名：BT/磁力优先用种子真实名（displayName，bittorrent.info.name）；
// 否则用真实下载路径的文件名（HTTP 经 Content-Disposition 解析后的最终名，替换
// URL 末尾 uuid 占位）；占位（magnet-N）回退 URL 文件名。
export std::string taskDisplayName(const dl::TaskView& task) {
    if (!task.displayName.empty()) return task.displayName;
    // UTF-8 提取（UI 显示字符串约定 UTF-8；窄串 .string() 在 Windows 走 ANSI 代码页）。
    const std::string fp = utf8FromPath(task.destPath.filename());
    if (!fp.empty() && fp.rfind("magnet-", 0) != 0) return fp;
    return fileNameFromUrl(task.url);
}

// startFromUrl 的结果：ok + 给用户看的消息（成功/失败都有）。UI 层决定怎么
// 展示（状态条 / 终端）。
export struct StartResult {
    bool ok = false;
    std::string message;
    std::uint64_t id = 0;   // 成功时的任务 id
};

// 删除任务后清理 aria2 的 .aria2 控制文件（下载缓存）。best-effort，失败静默。
void removeControlFile(const std::filesystem::path& destPath) {
    std::filesystem::path control = destPath;
    control += ".aria2";
    std::error_code ec;
    std::filesystem::remove(control, ec);
}

export class TaskStore {
public:
    TaskStore() : engine_(std::make_unique<dl::Aria2Engine>()) {}

    TaskStore(const TaskStore&) = delete;
    TaskStore& operator=(const TaskStore&) = delete;

    // ---- 查询 ----
    // 快照经 MergeTracker 过滤（DASH 视频子任务聚合成单个合成任务），并按统一的
    // 「下载创建顺序」降序排列（最新在前）——普通下载与视频下载都经 startFromUrl /
    // startVideoDownload 统一派发时由本 store 打同一条序号，保证两类任务排序一致。
    std::vector<dl::TaskView> snapshot() const {
        auto views = videoMerge_.mergeSnapshot(engine_->snapshot());
        std::stable_sort(views.begin(), views.end(), [&](const dl::TaskView& a, const dl::TaskView& b) {
            const auto sa = seqOf(a.id);
            const auto sb = seqOf(b.id);
            return sb < sa;  // 序号大 = 更晚创建 → 排前面
        });
        return views;
    }
    // 后台线程进度轮询（~1s 节流；UI 线程不要调，见 engine_->pollProgress 注释）。
    void pollProgress() { engine_->pollProgress(); }
    // housekeep 500ms 循环调用：检查 DASH 任务音视频是否都下完 → 触发 ffmpeg 合并。
    // 返回是否新触发了合并（true 时调用方唤醒 UI 显示「合并中」）。
    bool pollVideoMerges() { return videoMerge_.pollMerges(*engine_); }
    bool busy() const { return engine_->busy(); }
    bool engineActive() const { return engine_->engineActive(); }
    std::string lastError() const { return engine_->lastError(); }

    // ---- 引擎监控 ----
    // 健康快照（纯读缓存，UI 线程每帧可调）；refreshHealth/restartEngine 结果经
    // 回调在后台线程返回，UI 层自行 marshal（postStatus + requestUiUpdate）。
    dl::HealthInfo health() const { return engine_->health(); }
    void refreshHealth(std::function<void(const dl::HealthInfo&)> onDone) {
        engine_->refreshHealth(std::move(onDone));
    }
    void restartEngine(std::function<void(bool)> onDone) {
        engine_->restartEngine(std::move(onDone));
    }

    // ---- 生命周期 ----
    // 启动预热：拉起 daemon + 恢复上次会话历史任务（可后台线程调用，引擎内部
    // 有 daemonMutex_ 与 UI 线程互斥）。见 app.cpp 的预热线程。
    void warmup() { engine_->warmup(); }
    void shutdown() { engine_->shutdown(); }

    // ---- 任务命令（UI 线程）----
    // 先问 MergeTracker：命中视频合成任务则映射到两个子任务，否则落到引擎。
    void cancel(std::uint64_t id) { if (videoMerge_.cancel(*engine_, id)) return; engine_->cancel(id); }
    void pause(std::uint64_t id) { if (videoMerge_.pause(*engine_, id)) return; engine_->pause(id); }
    void resume(std::uint64_t id) { if (videoMerge_.resume(*engine_, id)) return; engine_->resume(id); }
    void pauseAll() { engine_->pauseAll(); }
    void resumeAll() { engine_->resumeAll(); }
    void retry(std::uint64_t id) { if (videoMerge_.retry(*engine_, id)) return; engine_->retry(id); }
    void addMirror(std::uint64_t id, const std::string& url,
                   std::function<void(bool)> onDone) {
        engine_->addMirror(id, url, std::move(onDone));
    }
    void removeMirror(std::uint64_t id, const std::string& url,
                      std::function<void(bool)> onDone) {
        engine_->removeMirror(id, url, std::move(onDone));
    }

    // 删除任务记录（daemon 会话 + 本地任务表）并清理下载缓存（.aria2 控制
    // 文件）。源文件是否删除由 UI 层的删除确认弹窗决定，不在本方法职责内。
    void deleteRecord(const dl::TaskView& task) {
        // 视频合成任务：MergeTracker 内部移除两个子任务并清掉 .m4s/成品 mp4。
        if (videoMerge_.remove(*engine_, task.id)) return;
        engine_->remove(task.id);
        removeControlFile(task.destPath);
    }

    // ---- 添加下载 ----
    // 校验并启动一个下载。完整的每任务选项在 opts 里（连接数/重命名/目录/镜像）。
    // 对话框 / CLI / 单实例 socket / inbox 共用。不碰 UI 状态，结果经
    // StartResult 返回。
    StartResult startFromUrl(std::string url, const dl::StartOptions& opts) {
        const std::size_t first = url.find_first_not_of(" \t\r\n");
        const std::size_t last = url.find_last_not_of(" \t\r\n");
        url = first == std::string::npos ? "" : url.substr(first, last - first + 1);
        if (url.empty()) {
            return {false, tr("请输入下载地址", "Please enter a download URL")};
        }

        const bool magnet = url.starts_with("magnet:");
        // aria2 原生支持 http/https/ftp/sftp/magnet；http 不再强制升级为 https。
        // 本地 .torrent 文件路径也放行（走 addTorrent）。
        const bool torrentFile = url.ends_with(".torrent") && std::filesystem::exists(url);
        if (!isDownloadableSource(url) && !torrentFile) {
            return {false, tr("仅支持 http(s) / ftp(s) / sftp 链接、magnet: 磁力链接或本地 .torrent 文件",
                              "Only http(s) / ftp(s) / sftp links, magnet: URIs, or local .torrent files are supported")};
        }
        // 下载目录：opts.dirOverride 覆盖（相对路径按配置目录解析）。
        std::filesystem::path dir = opts.dirOverride.empty()
            ? cfg::downloadDir()
            : opts.dirOverride;
        if (dir.is_relative()) dir = cfg::downloadDir() / dir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        // 文件名：优先重命名；磁力 / .torrent 没有可用的 URL 文件名，用占位名
        // （拿到元数据后引擎会从 files[0].path 更新为种子真实名）。
        std::string name = opts.outputName.empty()
            ? (torrentFile ? "torrent" : (magnet ? "magnet" : fileNameFromUrl(url)))
            : opts.outputName;
        if (name.empty()) name = "torrent";

        // name 是 UTF-8（URL 文件名 / 重命名输入），path 拼接必须经 pathFromUtf8——
        // Windows 窄串构造走 ANSI 代码页，中文名会抛异常/乱码。
        const std::filesystem::path dest = dir / pathFromUtf8(name);
        const std::uint64_t id = engine_->start(url, dest, opts);
        if (id == 0) {
            // 引擎给不出原因（如没实现 lastError）时回退到笼统提示。
            const std::string err = engine_->lastError();
            return {false, err.empty() ? tr("下载启动失败：引擎不可用",
                                            "Failed to start download: engine unavailable")
                                       : trf("下载启动失败：{}",
                                             "Failed to start download: {}", err)};
        }
        stampSeq(id);
        return {true, trf("已开始下载 #{} — {}", "Started download #{} — {}", id, name), id};
    }

    // 兼容重载：仅 URL + 连接数（CLI / inbox 用），其余选项取默认。
    StartResult startFromUrl(std::string url, int connections) {
        dl::StartOptions opts;
        opts.connections = connections;
        return startFromUrl(std::move(url), opts);
    }

    // ---- 视频下载 ----
    // 启动一个已解析好的视频下载（视频页在 resolveVideoUrl 成功后调用）。
    //   - 合流格式（format.audioUrl 空）：单个 aria2 任务，带 format 的请求头；
    //   - DASH（音视频分离）：交给 MergeTracker 起两个子任务，下完 ffmpeg 合并。
    // dir 为空用配置下载目录。baseOpts 提供连接数/目录覆盖等，请求头由 format 覆盖。
    StartResult startVideoDownload(const video::VideoInfo& info,
                                   const video::VideoFormat& format,
                                   const dl::StartOptions& baseOpts) {
        std::filesystem::path dir = baseOpts.dirOverride.empty()
            ? cfg::downloadDir()
            : baseOpts.dirOverride;
        if (dir.is_relative()) dir = cfg::downloadDir() / dir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        const std::string base =
            video::MergeTracker::sanitizeFileName(info.title.empty() ? "video" : info.title);

        // googlevideo / YouTube：aria2 直接拿这类 CDN 的流会被 403（开放 Range 首请求
        // 即拒，有代理拦截时更甚），改由 yt-dlp 原生下载并合并成单个 mp4。
        if (format.rangeBootstrap) {
            const std::uint64_t id = videoMerge_.startNativeJob(
                info, format, dir, cfg::videoConfig().keepM4sParts);
            if (id == 0) {
                return {false, tr("视频下载启动失败：解析器不可用",
                                  "Failed to start video download: resolver unavailable")};
            }
            stampSeq(id);
            return {true, trf("已开始下载视频 #{} — {}", "Started video download #{} — {}", id, base), id};
        }

        if (!format.audioUrl.empty()) {
            // DASH：MergeTracker 内部完成命名 / 起子任务 / 合并编排。
            const std::uint64_t id = videoMerge_.startJob(
                *engine_, info, format, dir, baseOpts, cfg::videoConfig().keepM4sParts);
            if (id == 0) {
                return {false, tr("视频下载启动失败：引擎不可用",
                                  "Failed to start video download: engine unavailable")};
            }
            stampSeq(id);
            return {true, trf("已开始下载视频 #{} — {}", "Started video download #{} — {}", id, base), id};
        }

        // 合流单文件：直接走引擎，携带解析出的请求头（Referer/UA 防 CDN 403）。
        if (format.videoUrl.empty()) {
            return {false, tr("该画质没有可用的视频流地址",
                              "No playable stream URL for this quality")};
        }
        dl::StartOptions opts = baseOpts;
        const std::string ext = format.ext.empty() ? "mp4" : format.ext;
        opts.outputName = base + "." + ext;
        opts.headers = format.headers.extra;
        opts.userAgent = format.headers.userAgent;
        opts.referer = format.headers.referer;
        // 合流流若在 googlevideo CDN：同样需要有限分段 Range + 单连接引导（见 MergeTracker）。
        if (format.rangeBootstrap) {
            opts.connections = 1;
            opts.headers.push_back("Range: bytes=0-1048575");
        }
        const std::filesystem::path dest = dir / pathFromUtf8(opts.outputName);
        const std::uint64_t id = engine_->start(format.videoUrl, dest, opts);
        if (id == 0) {
            const std::string err = engine_->lastError();
            return {false, err.empty() ? tr("视频下载启动失败：引擎不可用",
                                            "Failed to start video download: engine unavailable")
                                       : trf("视频下载启动失败：{}",
                                             "Failed to start video download: {}", err)};
        }
        stampSeq(id);
        return {true, trf("已开始下载视频 #{} — {}", "Started video download #{} — {}", id, opts.outputName), id};
    }

private:
    // 统一下载创建序：每个成功派发的下载（普通 startFromUrl 或视频 startVideoDownload
    // 的合成任务）领一个全局递增序号，存入 seqByTask_（taskId → seq）。作图层的
    // 「最新在前」排序依据，避免视频任务因 MergeTracker 追加在列表尾部而排不上去。
    std::uint64_t stampSeq(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(seqMutex_);
        const std::uint64_t s = ++nextSeq_;
        seqByTask_[id] = s;
        return s;
    }
    std::uint64_t seqOf(std::uint64_t id) const {
        std::lock_guard<std::mutex> lock(seqMutex_);
        const auto it = seqByTask_.find(id);
        return it == seqByTask_.end() ? 0 : it->second;
    }

    std::unique_ptr<dl::DownloadEngine> engine_;
    video::MergeTracker videoMerge_;   // DASH 视频任务的聚合 / 合并编排
    mutable std::mutex seqMutex_;      // 保护 nextSeq_ / seqByTask_（UI 线程派发写，housekeep 后台读）
    mutable std::uint64_t nextSeq_ = 0;
    mutable std::unordered_map<std::uint64_t, std::uint64_t> seqByTask_;
};

// 全局唯一任务 store（模块级导出变量在 importers 间共享同一实体）。
export TaskStore g_tasks;
