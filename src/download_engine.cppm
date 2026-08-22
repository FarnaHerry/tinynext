// download_engine.cppm — engine-agnostic downloader interface.
//
// State/TaskView and the abstract DownloadEngine live here so the UI and the
// aria2-next process engine (dl::Aria2Engine) share one surface. The UI
// consumes DownloadEngine only.
export module tinynext.download_engine;

import std;

namespace dl {

export enum class State {
    Queued,
    Downloading,
    Paused,
    Done,
    Failed,
    Cancelled,
    Merging,   // 视频音视频流已下完，ffmpeg 合并中（视频下载合成任务的中间态）
};

// 镜像任务的一个源（aria2 files[0].uris 的去重后结果）。
export struct MirrorSource {
    std::string uri;
    std::string status;   // aria2: used（在用）/ waiting（备用）/ error（失败）
};

// 引擎健康信息（监控页展示）。纯读缓存：由 refreshHealth() 在后台线程刷新，
// health() 只读缓存、绝不发 RPC（UI 线程每帧可调用）。
export struct HealthInfo {
    bool checked = false;        // 是否已做过一次健康检查（未检查时页面显示"检测中…"）
    bool binaryFound = false;    // engines/aria2-next(.exe) 存在
    bool daemonSpawned = false;  // daemon 已拉起过（进程可能已退出，看 daemonAlive）
    bool daemonAlive = false;    // 守护进程已拉起且进程未退出
    bool rpcReachable = false;   // 最近一次 getVersion 成功
    bool wsConnected = false;    // WebSocket 事件推送连接
    std::string version;         // aria2 版本串（getVersion.version）
    std::string error;           // 最近检测/启动失败原因（空 = 无）
    int rpcPort = 0;             // 本地 RPC 端口（daemon 未拉起 = 0）
    std::int64_t downloadSpeedBps = 0;   // getGlobalStat
    std::int64_t uploadSpeedBps = 0;
    int activeDownloads = 0;
    int waitingDownloads = 0;
    int stoppedDownloads = 0;
};

// Immutable snapshot of one download task, safe to read from any thread.
export struct TaskView {
    std::uint64_t id;
    std::string url;
    std::filesystem::path destPath;
    State state;
    std::int64_t totalBytes;      // -1 = unknown (no Content-Length)
    std::int64_t downloadedBytes;
    std::string error;            // empty unless Failed
    double speedBps;              // bytes/second, last measured
    int connections = 1;          // active network connections; 1 = single-connection engine
    std::string displayName;      // 真实显示名（BT/磁力用种子的 info.name；HTTP 不用，取 destPath 文件名）
    int mirrorCount = 0;          // 配置的镜像源数（除主 URL 外的源数；0 = 单源）
    std::vector<MirrorSource> mirrors;  // 实时源列表（去重，含主 URL 与运行时 changeUri 增删）
    bool fromSession = false;           // 从上次会话恢复的历史任务：不触发「完成/失败」通知
    std::string destPathUtf8;           // destPath 的预编码 UTF-8 串（在快照时任务数据存活时转好，
                                        // 避免后续读 destPath 本体时遇悬空指针崩溃）
    State progressState = State::Queued; // 信息行展示用的状态（yt-dlp 原生任务在 ffmpeg 合并时
                                         // 覆盖 state 为 Merging，卡片信息行据此展示「合并中」）
};

// Per-task start options. connections == 0 means "use the engine default from
// config"; engines that don't support a field just ignore it.
// 注意：aria2-next 没有下载级 priority 选项（实测+--help=#all 确认），优先级功能已移除。
export struct StartOptions {
    int connections = 0;                  // 0 = 引擎按配置默认
    std::string outputName;               // 重命名；空 = 取 URL 文件名
    std::filesystem::path dirOverride;    // 覆盖下载目录；空 = 配置目录（相对按配置目录解析）
    std::filesystem::path torrentPath;    // 本地 .torrent 文件；空 = 普通 URL 下载
    std::vector<std::string> mirrors;     // 镜像源（同一任务多源）；空 = 单 URL
    // 限速不在这里：每任务单独限速已移除（无意义），统一走配置的 maxDownloadLimit。
    // 每任务 HTTP 头（视频解析等 CDN 受限源用，如 bilibili 强制 Referer 否则 403）。
    // 全部可空；空则不加对应 aria2 选项，行为与之前完全一致（向后兼容）。
    std::vector<std::string> headers;     // 原始 "Key: Value" 行（如 Cookie）
    std::string userAgent;                // 覆盖 daemon 级 UA（仅本任务）
    std::string referer;                  // 覆盖 daemon 级 Referer（仅本任务）
};

// Abstract download engine contract. Implementations are owned by the app
// (std::unique_ptr) and must be created/destroyed on the UI thread.
export class DownloadEngine {
public:
    virtual ~DownloadEngine() = default;

    // Enqueue a download to destPath. Returns a stable task id.
    virtual std::uint64_t start(const std::string& url,
                                const std::filesystem::path& destPath,
                                const StartOptions& options = {}) = 0;

    // Request cancellation; the task ends up in the Cancelled state.
    virtual void cancel(std::uint64_t id) = 0;

    // Permanently remove a task from the list (cancels + joins first).
    virtual void remove(std::uint64_t id) = 0;

    // Pause a queued/running task; no-op unless Queued/Downloading.
    virtual void pause(std::uint64_t id) = 0;

    // Resume a paused task; no-op unless Paused.
    virtual void resume(std::uint64_t id) = 0;

    // Pause / resume every active task (Queued / Downloading / Paused).
    virtual void pauseAll() = 0;
    virtual void resumeAll() = 0;

    // Re-download a Failed/Cancelled task using its original URL and destination
    // path. Engines that support resume (aria2 control files) continue from the
    // partial file; others restart from scratch.
    virtual void retry(std::uint64_t id) = 0;

    // Copy of all tasks, newest first. 纯读缓存（内部持锁），绝不发 RPC——UI 线程
    // 每帧调用它，实现必须保证立即返回。
    virtual std::vector<TaskView> snapshot() const = 0;

    // 后台线程进度轮询：发一次 tellStatus 批量刷新活动任务的进度/速度/连接数
    // （内部 ~1s 节流）。UI 线程不要调用——snapshot 已剥离 RPC，进度刷新由
    // housekeep 等后台线程驱动。默认空实现。
    virtual void pollProgress() {}

    // 引擎健康快照（纯读缓存，不发 RPC）。UI 线程每帧可调用；引擎不支持健康
    // 监控时返回默认（全 false / 空）。
    virtual HealthInfo health() const { return {}; }

    // 异步健康检查：后台线程发一次 getVersion/getGlobalStat 刷新缓存，结果经
    // onDone 回传（后台线程调用，UI 层自行 marshal，如 requestUiUpdate）。引擎
    // 不支持时立即回调默认值。
    virtual void refreshHealth(std::function<void(const HealthInfo&)> onDone) {
        if (onDone) onDone(HealthInfo{});
    }

    // 重启引擎运行时：保存会话 → 优雅退出 → 重新拉起（进行中任务经会话恢复
    // --input-file 重载，不会丢）。结果经 onDone(bool) 回传（后台线程调用）。
    // 默认实现直接回调 true（无运行时可重启）。
    virtual void restartEngine(std::function<void(bool)> onDone) {
        if (onDone) onDone(true);
    }

    // 向已有任务追加镜像源（aria2.changeUri add）。仅活动任务有效。RPC 在后台
    // 线程执行，结果经 onDone(bool) 回传（后台线程调用，UI 层自行 marshal，如
    // postStatus + requestUiUpdate）。引擎不支持时立即回调 false。
    virtual void addMirror(std::uint64_t id, const std::string& url,
                           std::function<void(bool)> onDone) {
        (void)id; (void)url;
        if (onDone) onDone(false);
    }

    // 从已有任务移除镜像源（aria2.changeUri delete）。仅活动任务有效。同上回调式。
    virtual void removeMirror(std::uint64_t id, const std::string& url,
                              std::function<void(bool)> onDone) {
        (void)id; (void)url;
        if (onDone) onDone(false);
    }

    // True while any task is queued or running.
    virtual bool busy() const = 0;

    // True when the engine has spawned its runtime (e.g. the aria2 daemon), so
    // daemon-level settings saved now only take effect after a restart.
    virtual bool engineActive() const { return false; }

    // 最近一次失败操作的原因（如 daemon 为何没能启动/完整性校验失败）。成功或
    // 无错误时为空串。UI 线程调用，供「下载启动失败：<原因>」提示。
    virtual std::string lastError() const { return {}; }

    // 启动预热：拉起引擎运行时并恢复上次会话的历史任务（否则首次 start() 才
    // 懒惰初始化，重启后历史记录要等下一次下载才出现）。可从后台线程调用
    // （实现须自行与 UI 线程的 start() 等调用互斥）。默认空实现。
    virtual void warmup() {}

    // Cancel everything and release engine resources.
    virtual void shutdown() = 0;
};

} // namespace dl
