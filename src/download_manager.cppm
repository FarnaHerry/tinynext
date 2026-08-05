// download_manager.cppm — tinyhttps-backed download engine.
//
// Implements the engine-agnostic dl::DownloadEngine interface (defined in
// tinynext.download_engine). tinyhttps only ever leaks in the implementation
// unit (download_manager.cpp); consumers see plain C++ types.
export module tinynext.download_manager;

import std;

export import tinynext.download_engine;

namespace dl {

export class TinyHttpsEngine : public DownloadEngine {
public:
    // Initializes the platform networking layer (on Windows this is Winsock:
    // tinyhttps never calls WSAStartup itself, so without this every connect
    // fails with "Connection failed" — verified on the consuming side).
    TinyHttpsEngine();
    ~TinyHttpsEngine() override;

    TinyHttpsEngine(const TinyHttpsEngine&) = delete;
    TinyHttpsEngine& operator=(const TinyHttpsEngine&) = delete;

    // Enqueue a download. Each task gets its own worker thread and its own
    // HttpClient (the library is not thread-safe). Returns the task id.
    std::uint64_t start(const std::string& url, const std::filesystem::path& destPath) override;

    // Request cancellation of a task. The worker stops at the next block
    // boundary; the task ends up in the Cancelled state.
    void cancel(std::uint64_t id) override;

    // Permanently remove a task from the list. If its worker is still running
    // it is cancelled first and joined before this returns, so the task is
    // guaranteed gone from snapshot() afterwards. Does not touch the file on
    // disk.
    void remove(std::uint64_t id) override;

    // Pause a queued/running task. The worker parks at the next block boundary
    // (the connection stays open); the task enters the Paused state. No-op
    // unless the task is Queued or Downloading.
    void pause(std::uint64_t id) override;

    // Resume a paused task. The worker continues from where it parked and the
    // task returns to the Downloading state. No-op unless the task is Paused.
    void resume(std::uint64_t id) override;

    // Copy of all tasks, newest first.
    std::vector<TaskView> snapshot() const override;

    // True while any task is queued or running.
    bool busy() const override;

    // Cancel everything and join all workers. Called by the destructor.
    void shutdown() override;

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
