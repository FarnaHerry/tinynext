// ui/housekeep.cppm — 后台杂务线程：状态消息过期 + 下载通知检测。
//
// 取代 app.cpp 根 stack 的 onFrame。onFrame 会让 eui 把挂它的元素当成「每帧都
// 在动」：composeRequested / paintRequested / animating 每帧置位 → 空闲也 90 FPS
// 全量重绘（GPU 占用跳跃的根因）。这里改为后台线程只在「真有事」时
// core::platform::requestUiUpdate() 唤醒 UI 一帧；空闲时 UI 走 glfwWaitEvents
// 睡眠、零渲染。
module;

// eui 的 UI 唤醒（跨线程安全，eui 的 network 线程也这么用）。
namespace core::platform { void requestUiUpdate(); }

export module tinynext.ui.housekeep;

import std;
import tinynext.download_engine;  // dl::State（通知的状态迁移判断）
import tinynext.i18n;             // tr / trf（下载通知文案）
import tinynext.store.tasks;      // g_tasks.snapshot + taskDisplayName
import tinynext.store.ui;         // statusExpired
import tinynext.ui.platform;      // notifyDownload

namespace housekeep {

namespace {

std::atomic<bool> g_statusExpired{false};

// 检查任务状态迁移，仅当任务从进行中（排队/下载/暂停）迁移到 Done/Failed 时发
// 系统通知（避免会话恢复等历史状态误触发）。snapshot 走引擎 tasksMutex_，后台
// 线程安全；lastStates 由 housekeep 单线程独占。
void checkDownloadNotifications() {
    static std::unordered_map<std::uint64_t, dl::State> lastStates;
    const auto tasks = g_tasks.snapshot();
    std::unordered_set<std::uint64_t> seen;
    seen.reserve(tasks.size());
    for (const auto& t : tasks) {
        seen.insert(t.id);
        const auto it = lastStates.find(t.id);
        // 会话恢复的历史任务（fromSession）一律不通知：它们是本次启动之前就存在/
        // 完成的下载，重新载入时不应再弹「完成/失败」提醒。
        if (t.fromSession) {
            lastStates[t.id] = t.state;
            continue;
        }
        if (it != lastStates.end()) {
            const dl::State prev = it->second;
            const bool wasActive = prev == dl::State::Queued ||
                                   prev == dl::State::Downloading ||
                                   prev == dl::State::Paused ||
                                   prev == dl::State::Merging;  // 视频合并中→完成也通知
            if (wasActive && prev != t.state) {
                const std::string name = taskDisplayName(t);
                if (t.state == dl::State::Done) {
                    notifyDownload(tr("hk.download_complete"),
                                   trf("hk.downloaded", name));
                } else if (t.state == dl::State::Failed) {
                    notifyDownload(tr("hk.download_failed"),
                                   trf("hk.failed", name));
                }
            }
        }
        lastStates[t.id] = t.state;
    }
    // 清掉已从列表移除的任务记录，避免 map 无限增长。
    for (auto it = lastStates.begin(); it != lastStates.end();) {
        if (seen.count(it->first) == 0) {
            it = lastStates.erase(it);
        } else {
            ++it;
        }
    }
}

void housekeepLoop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // 状态消息 4s 自动消失：置位 + 唤醒，UI 线程在下一帧清空显示。
        if (statusExpired()) {
            g_statusExpired.store(true);
            core::platform::requestUiUpdate();
        }
        // 下载完成/失败通知 + 有活动任务时的进度刷新。进度 RPC 显式走
        // pollProgress（snapshot 已剥离 RPC，绝不在 UI 线程发）；空任务时
        // pollProgress 内部节流 + refreshStates 不发任何 RPC，空闲零开销。
        checkDownloadNotifications();
        if (g_tasks.busy()) {
            g_tasks.pollProgress();
        }
        // 有活动任务时每 500ms 唤醒 UI 重绘一帧进度（连接数/速度/百分比实时更新，
        // 不依赖 RPC 结果回调）。注意：唤醒必须无条件——pollProgress 的 ~1s 节流
        // 只在 RPC 间隙跳过 RPC，不代表进度没变，但 snapshot 是纯读缓存，每帧读取
        // 最新数据就够。把 requestUiUpdate 移出 {} 确保 busy 为 true 时每 500ms 都
        // 唤醒，解决之前 aria2 进度刷新断断续续、信息行间歇空白的问题。
        if (g_tasks.busy() || g_tasks.pollVideoMerges()) {
            core::platform::requestUiUpdate();
        }
        // 引擎监控页打开时 ~2s 刷新一次健康信息（getVersion/getGlobalStat RPC 在
        // 后台命令线程执行，不阻塞 UI）。离开监控页即停，空闲时零额外开销。
        const auto now = std::chrono::steady_clock::now();
        static auto lastHealthRefresh = now;
        if (g_page_view == Page::Monitor &&
            now - lastHealthRefresh >= std::chrono::seconds(2)) {
            lastHealthRefresh = now;
            g_tasks.refreshHealth([](const dl::HealthInfo&) {
                core::platform::requestUiUpdate();
            });
        }
    }
}

} // namespace

// 启动后台杂务线程（幂等）。在首次 compose 时调用。
export void startHousekeeping() {
    static std::atomic<bool> started = false;
    if (started.exchange(true)) return;
    std::thread(housekeepLoop).detach();
}

// UI 线程消费：状态消息是否已过期（过期则清空 g_statusMessage/g_statusTimer）。
export bool consumeStatusExpired() {
    return g_statusExpired.exchange(false);
}

} // namespace housekeep
