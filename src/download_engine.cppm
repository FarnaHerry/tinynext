// download_engine.cppm — engine-agnostic downloader interface.
//
// State/TaskView and the abstract DownloadEngine live here so both the
// in-process tinyhttps engine (dl::TinyHttpsEngine) and a future aria2-next
// process engine can share one surface. The UI consumes DownloadEngine only;
// the concrete engine is chosen by a factory (settings "下载引擎").
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
// config"; engines that don't support multi-connection ignore it.
export struct StartOptions {
    int connections = 0;
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

    // Copy of all tasks, newest first.
    virtual std::vector<TaskView> snapshot() const = 0;

    // True while any task is queued or running.
    virtual bool busy() const = 0;

    // Cancel everything and release engine resources.
    virtual void shutdown() = 0;
};

} // namespace dl
