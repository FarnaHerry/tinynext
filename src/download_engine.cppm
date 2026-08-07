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
};

// Per-task start options. connections == 0 means "use the engine default from
// config"; engines that don't support a field just ignore it.
// 注意：aria2-next 没有下载级 priority 选项（实测+--help=#all 确认），优先级功能已移除。
export struct StartOptions {
    int connections = 0;                  // 0 = 引擎按配置默认
    std::string outputName;               // 重命名；空 = 取 URL 文件名
    std::filesystem::path dirOverride;    // 覆盖下载目录；空 = 配置目录（相对按配置目录解析）
    std::int64_t limitBps = 0;            // 每任务限速 bytes/s；0 = 全局配置
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

    // True while any task is queued or running.
    virtual bool busy() const = 0;

    // True when the engine has spawned its runtime (e.g. the aria2 daemon), so
    // daemon-level settings saved now only take effect after a restart.
    virtual bool engineActive() const { return false; }

    // Cancel everything and release engine resources.
    virtual void shutdown() = 0;
};

} // namespace dl
