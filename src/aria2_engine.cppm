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

    // Enqueue a download. The daemon is normally already up (warmup() spawns it
    // at app startup); start() falls back to spawning on first use if warmup
    // has not run (e.g. headless). Returns the task id, or 0 if the daemon
    // could not be started / the task failed to enqueue.
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
    void addMirror(std::uint64_t id, const std::string& url,
                   std::function<void(bool)> onDone) override;
    void removeMirror(std::uint64_t id, const std::string& url,
                      std::function<void(bool)> onDone) override;
    std::vector<TaskView> snapshot() const override;
    void pollProgress() override;
    HealthInfo health() const override;
    void refreshHealth(std::function<void(const HealthInfo&)> onDone) override;
    void restartEngine(std::function<void(bool)> onDone) override;
    bool busy() const override;
    bool engineActive() const override;
    std::string lastError() const override;
    void warmup(bool restoreFailed) override;
    void shutdown() override;

private:
    struct Task;
    bool ensureDaemon() const;      // spawn + wait until RPC answers
    void recoverSession() const;    // 重启后重建会话任务（tellActive/Waiting/Stopped）
    void refreshStates() const;     // poll tellStatus for live tasks (~1 Hz), 内部管理锁
    std::shared_ptr<Task> findTask(std::uint64_t id) const;
    std::filesystem::path makeUniqueDest(const std::filesystem::path& dest) const;
    // 后台命令队列：任务动作（暂停/继续/重试/删除/镜像）的 rpcCall 在独立线程跑，
    // UI 线程只乐观更新状态后立即返回，绝不发网络请求。
    void enqueue(std::function<void()> fn);
    void commandLoop();
    void retryOnWorker(std::uint64_t id);   // retry 的实际流程（命令线程执行）
    // 用一条 tellStatus 的 JSON 刷新任务字段（含 status→State 映射）。调用方须持锁。
    void applyTellStatus(const std::shared_ptr<Task>& task, const nlohmann::json& st) const;
    // WebSocket 推送事件回调（IXWebSocket 后台线程）。持锁按 gid 更新状态。
    void handleWsEvent(const std::string& method, const std::string& gid) const;

    // daemon 生命周期成员（port_/secret_/daemonSpawned_/ws_/processHandle_/
    // lastError_）由 UI 线程（start/retry/shutdown）与启动预热后台线程（warmup，
    // 见 app.cpp）共享，一律经 daemonMutex_ 访问；锁序固定 daemonMutex_ →
    // tasksMutex_（recoverSession 在 ensureDaemon 内取 tasksMutex_）。
    // tasks_ 及其 Task 字段由 UI 线程 + WS 后台线程共享，一律经 tasksMutex_ 访问。
    mutable std::mutex daemonMutex_;
    mutable std::vector<std::shared_ptr<Task>> tasks_;
    mutable std::mutex tasksMutex_;
    mutable std::uint64_t nextId_ = 1;
    mutable bool daemonSpawned_ = false;
    // warmup(restoreFailed) 记录「启动时自动重试失败任务」开关：recoverSession
    // 据此决定失败记录是恢复成 Failed 任务（供上层随后 retry）还是丢弃。
    // 进程生命周期内只在 warmup 写一次（daemon 未 spawn 前），此后只读。
    bool restoreFailedOnRecover_ = false;
    mutable int port_ = 0;
    mutable std::string secret_;
    mutable std::string lastError_;   // 最近一次 daemon 启动失败原因（仅 UI 线程）
    mutable void* processHandle_ = nullptr;   // HANDLE on Windows
    mutable std::chrono::steady_clock::time_point lastPoll_{};
    mutable std::unique_ptr<WsNotifier> ws_;  // 事件监听（仅收推送，请求仍走 HTTP）

    // 健康缓存：refreshHealth（后台命令线程）写，health()（UI 线程）读，互斥保护。
    mutable std::mutex healthMutex_;
    mutable HealthInfo healthInfo_;

    // 命令队列 worker（串行消费动作 rpcCall，FIFO）。cmdShutdown_ 置位后 worker
    // 丢弃未处理命令并退出；shutdown() 在取 daemonMutex_ 之前 join，避免 worker 正
    // 调 ensureDaemon 等 daemonMutex_ 时死锁。
    mutable std::thread cmdThread_;
    mutable std::mutex cmdMutex_;
    mutable std::condition_variable cmdCv_;
    mutable std::deque<std::function<void()>> cmdQueue_;
    mutable bool cmdShutdown_ = false;
};

} // namespace dl
