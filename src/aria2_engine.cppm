// aria2_engine.cppm — aria2-next process-based download engine.
//
// Spawns the bundled aria2-next.exe (engines/) as a local JSON-RPC daemon and
// drives it with addUri/pause/unpause/remove/tellStatus. Multi-connection
// sharding (-x 16 -s 16), resume (.aria2 control files) and all the aria2
// protocol support come for free.
//
// Threading: requests still run on the UI thread (single-threaded), but a
// background WebSocket connection (compat.websocket / IXWebSocket) receives
// aria2's push notifications (onDownloadStart/Complete/Error/...). Those arrive
// on IXWebSocket's internal thread, so all access to tasks_ is guarded by
// tasksMutex_. Lock discipline: callers that touch tasks_ hold the lock; the
// WS callback takes it briefly to flip a task's state. See aria2_engine.cpp.
export module tinynext.aria2_engine;

import std;
import nlohmann.json;

export import tinynext.download_engine;

namespace dl {

struct WsNotifier;  // defined in aria2_engine.cpp

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
    bool addMirror(std::uint64_t id, const std::string& url) override;
    bool removeMirror(std::uint64_t id, const std::string& url) override;
    std::vector<TaskView> snapshot() const override;
    bool busy() const override;
    bool engineActive() const override;
    std::string lastError() const override;
    void shutdown() override;

private:
    struct Task;
    bool ensureDaemon() const;      // spawn + wait until RPC answers
    void recoverSession() const;    // 重启后重建会话任务（tellActive/Waiting/Stopped）
    void refreshStates() const;     // poll tellStatus for live tasks (~1 Hz)
    std::shared_ptr<Task> findTask(std::uint64_t id) const;
    std::filesystem::path makeUniqueDest(const std::filesystem::path& dest) const;
    // 用一条 tellStatus 的 JSON 刷新任务字段（含 status→State 映射）。调用方须持锁。
    void applyTellStatus(const std::shared_ptr<Task>& task, const nlohmann::json& st) const;
    // WebSocket 推送事件回调（IXWebSocket 后台线程）。持锁按 gid 更新状态。
    void handleWsEvent(const std::string& method, const std::string& gid) const;

    // 以下成员仅 UI 线程访问（daemon 生命周期）：port_/secret_/daemonSpawned_/ws_。
    // tasks_ 及其 Task 字段由 UI 线程 + WS 后台线程共享，一律经 tasksMutex_ 访问。
    mutable std::vector<std::shared_ptr<Task>> tasks_;
    mutable std::mutex tasksMutex_;
    mutable std::uint64_t nextId_ = 1;
    mutable bool daemonSpawned_ = false;
    mutable int port_ = 0;
    mutable std::string secret_;
    mutable std::string lastError_;   // 最近一次 daemon 启动失败原因（仅 UI 线程）
    mutable void* processHandle_ = nullptr;   // HANDLE on Windows
    mutable std::chrono::steady_clock::time_point lastPoll_{};
    mutable std::unique_ptr<WsNotifier> ws_;  // 事件监听（仅收推送，请求仍走 HTTP）
};

} // namespace dl
