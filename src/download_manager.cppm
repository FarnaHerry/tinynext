// download_manager.cppm — module interface for the download engine.
//
// The only place the tinyhttps module leaks in is the implementation unit
// (download_manager.cpp); consumers of this module see plain C++ types only.
export module tinynext.download_manager;

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
};

export class DownloadManager {
public:
    // Initializes the platform networking layer (on Windows this is Winsock:
    // tinyhttps never calls WSAStartup itself, so without this every connect
    // fails with "Connection failed" — verified on the consuming side).
    DownloadManager();
    ~DownloadManager();

    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    // Enqueue a download. Each task gets its own worker thread and its own
    // HttpClient (the library is not thread-safe). Returns the task id.
    std::uint64_t start(const std::string& url, const std::filesystem::path& destPath);

    // Request cancellation of a task. The worker stops at the next block
    // boundary; the task ends up in the Cancelled state.
    void cancel(std::uint64_t id);

    // Pause a queued/running task. The worker parks at the next block boundary
    // (the connection stays open); the task enters the Paused state. No-op
    // unless the task is Queued or Downloading.
    void pause(std::uint64_t id);

    // Resume a paused task. The worker continues from where it parked and the
    // task returns to the Downloading state. No-op unless the task is Paused.
    void resume(std::uint64_t id);

    // Copy of all tasks, newest first.
    std::vector<TaskView> snapshot() const;

    // True while any task is queued or running.
    bool busy() const;

    // Cancel everything and join all workers. Called by the destructor.
    void shutdown();

private:
    struct Task;
    void runWorker(std::shared_ptr<Task> task);

    // Pick dest if free, otherwise append " (1)", " (2)", ... to avoid
    // overwriting an existing file. Considers both files already on disk and
    // destination paths reserved by in-flight tasks. Caller must hold mutex_.
    std::filesystem::path makeUniqueDest(const std::filesystem::path& dest) const;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Task>> tasks_;
    std::uint64_t nextId_ = 1;
};

} // namespace dl
