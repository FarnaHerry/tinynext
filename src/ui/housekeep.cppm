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
import tinynext.ui.state;

namespace housekeep {

namespace {

std::atomic<bool> g_statusExpired{false};

void housekeepLoop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // 状态消息 4s 自动消失：置位 + 唤醒，UI 线程在下一帧清空显示。
        if (statusExpired()) {
            g_statusExpired.store(true);
            core::platform::requestUiUpdate();
        }
        // 下载完成/失败通知 + 有活动任务时的进度刷新（snapshot 内部 ~1s 才发一次
        // 进度 RPC；空任务时 refreshStates 不发任何 RPC，空闲零开销）。
        if (g_manager) {
            checkDownloadNotifications();
            if (g_manager->busy()) {
                core::platform::requestUiUpdate();
            }
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
