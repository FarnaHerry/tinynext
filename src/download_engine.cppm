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
};

// 镜像任务的一个源（aria2 files[0].uris 的去重后结果）。
export struct MirrorSource {
    std::string uri;
    std::string status;   // aria2: used（在用）/ waiting（备用）/ error（失败）
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

    // Copy of all tasks, newest first.
    virtual std::vector<TaskView> snapshot() const = 0;

    // 向已有任务追加镜像源（aria2.changeUri add）。仅活动任务有效；
    // 引擎不支持时是 no-op。返回是否成功。
    virtual bool addMirror(std::uint64_t id, const std::string& url) {
        (void)id; (void)url;
        return false;
    }

    // 从已有任务移除镜像源（aria2.changeUri delete）。仅活动任务有效。
    virtual bool removeMirror(std::uint64_t id, const std::string& url) {
        (void)id; (void)url;
        return false;
    }

    // True while any task is queued or running.
    virtual bool busy() const = 0;

    // True when the engine has spawned its runtime (e.g. the aria2 daemon), so
    // daemon-level settings saved now only take effect after a restart.
    virtual bool engineActive() const { return false; }

    // 最近一次失败操作的原因（如 daemon 为何没能启动/完整性校验失败）。成功或
    // 无错误时为空串。UI 线程调用，供「下载启动失败：<原因>」提示。
    virtual std::string lastError() const { return {}; }

    // Cancel everything and release engine resources.
    virtual void shutdown() = 0;
};

} // namespace dl
