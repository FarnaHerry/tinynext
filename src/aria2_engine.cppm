// aria2_engine.cppm — aria2-next process-based download engine.
//
// Spawns the bundled aria2-next.exe (engines/) as a local JSON-RPC daemon and
// drives it with addUri/pause/unpause/remove/tellStatus. Multi-connection
// sharding (-x 16 -s 16), resume (.aria2 control files) and all the aria2
// protocol support come for free. There are no worker threads: every operation
// runs on the UI thread via RPC, so the engine is single-threaded by construction.
export module tinynext.aria2_engine;

import std;

export import tinynext.download_engine;

namespace dl {

export class Aria2Engine : public DownloadEngine {
public:
    Aria2Engine();
    ~Aria2Engine() override;

    Aria2Engine(const Aria2Engine&) = delete;
    Aria2Engine& operator=(const Aria2Engine&) = delete;

    // Enqueue a download. Spawns the daemon on first use. Returns the task id,
    // or 0 if the daemon could not be started / the task failed to enqueue.
    // StartOptions.connections overrides the config split/max-connection for
    // this task when > 0.
    std::uint64_t start(const std::string& url, const std::filesystem::path& destPath,
                        const StartOptions& options = {}) override;
    void cancel(std::uint64_t id) override;
    void remove(std::uint64_t id) override;
    void pause(std::uint64_t id) override;
    void resume(std::uint64_t id) override;
    void pauseAll() override;
    void resumeAll() override;
    void retry(std::uint64_t id) override;
    std::vector<TaskView> snapshot() const override;
    bool busy() const override;
    bool engineActive() const override;
    void shutdown() override;

private:
    struct Task;
    bool ensureDaemon() const;      // spawn + wait until RPC answers
    void recoverSession() const;    // 重启后重建会话任务（tellActive/Waiting/Stopped）
    void refreshStates() const;     // poll tellStatus for live tasks (~5 Hz)
    std::shared_ptr<Task> findTask(std::uint64_t id) const;
    std::filesystem::path makeUniqueDest(const std::filesystem::path& dest) const;

    mutable std::vector<std::shared_ptr<Task>> tasks_;
    mutable std::uint64_t nextId_ = 1;
    mutable bool daemonSpawned_ = false;
    mutable int port_ = 0;
    mutable std::string secret_;
    mutable void* processHandle_ = nullptr;   // HANDLE on Windows
    mutable std::chrono::steady_clock::time_point lastPoll_{};
};

} // namespace dl
