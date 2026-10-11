// app.cpp — thin entry for the TinyNext UI.
//
// The `app-main` feature of compat.eui-neo supplies main()
// (core/app/glfw_app_main.cpp); this TU defines the two symbols every EUI
// application must provide — app::dslAppConfig() and app::compose() — and
// delegates drawing to the tinynext.ui.* modules:
//
//   tinynext.utils            pure string/number helpers (no UI dependency)
//   tinynext.store.tasks      TaskStore：引擎 + 任务数据 + 下载命令（领域 store）
//   tinynext.store.ui         状态消息 / 页面 / 筛选·排序·分页（视图 store）
//   tinynext.store.dialogs    弹窗状态机 + 提交动作（视图 store）
//   tinynext.ui.utils         UI layout constants (design logical pixels)
//   tinynext.ui.theme         dark/light AppTheme + currentTheme() + 主题全局
//   tinynext.ui.platform      DPI boot + folder picker + open helpers
//   tinynext.ui.widgets       choice buttons + list picker + task actions
//   tinynext.ui.cards         the download task card
//   tinynext.ui.downloads_page  downloads page + add-download dialog
//   tinynext.ui.settings_page   settings page
//   tinynext.cli              single-instance + CLI (boot + inbox polling)
//
// eui_neo.h is included HERE for the DslAppConfig / app::compose declarations.
// Since eui-neo 0.5.6 the umbrella no longer pulls in eui/detail/dsl_app_impl.h:
// the app::* machinery (update / render / initialize / ...) now lives inside the
// `app-main` feature's own TU (core/app/glfw_app_main.cpp), which includes
// dsl_app_impl.h itself. The tinynext.ui.* modules include the reduced
// "ui/eui_ui.h" — kept as a minimal include surface (the old mangled-name clash
// motivation is gone in 0.5.6).
#include <eui_neo.h>

import std;
import tinynext.config;
import tinynext.i18n;   // tr / trf（启动失败提示）
import tinynext.cli;
import tinynext.ui.utils;
import tinynext.ui.theme_watch;
import tinynext.ui.theme;
import tinynext.ui.downloads_page;
import tinynext.ui.settings_page;
import tinynext.ui.extensions_page;
import tinynext.ui.platform;
import tinynext.ui.housekeep;
import tinynext.store.tasks;    // g_tasks（启动预热 warmup）
import tinynext.browser_host;   // 浏览器扩展宿主注册（Native Messaging 清单）
import tinynext.component_updater;  // 引擎更新生命周期与版本探测
import tinynext.store.ui;       // 状态消息 / 页面

namespace app {

// 启动预热结果（后台线程 → UI 线程）：warmup 失败时置位，compose 下一帧消费并
// 弹状态消息（showStatus 只能 UI 线程写，所以走原子标志中转）。
std::atomic<bool> g_warmupFailed{false};
// 预热完成（无论成败）：compose 消费后按配置对恢复出的失败任务自动重试。
std::atomic<bool> g_warmupDone{false};
std::atomic<bool> g_appShuttingDown{false};
std::thread g_warmupThread;

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("TinyNext 下载器")
        .pageId("tinynext")
        // 起底色 = 暗主题 surface（#0A0A0A）；运行期背景由 compose 的全屏矩形覆盖，
        // 主题切换即时生效，这里只是启动瞬间防白闪。
        .clearColor({0.039f, 0.039f, 0.039f, 1.0f})
        // 原生全局缩放（eui-neo 0.5.6）：uiScale 按 dpiScale*uiScale 放大整个逻辑
        // 坐标系（布局+字号），所有尺寸按设计逻辑像素书写、不再 S() 自乘。窗口
        // 物理尺寸 = 设计尺寸 * kUI（eui 创建窗口时按物理像素，不会自动乘 uiScale）。
        .uiScale(kUI)
        .windowSize(static_cast<int>(1120.0f * kUI), static_cast<int>(720.0f * kUI))
        // 启动时先给出 1× DPI 的窗口限制；compose 按实际窗口比例补齐高 DPI 换算。
        // 700×500 的逻辑画布可容纳顶部工具栏、滚动正文和底部操作区。
        .minWindowSize(static_cast<int>(kMinPageWidth * kUI), static_cast<int>(kMinPageHeight * kUI))
        // 最大帧率写 0 = 自动匹配显示器刷新率（eui 的 updateFrameInterval 在
        // limit<=0 时直接用 getWindowRefreshRate）。
        .fps(0.0)
        // 0.3.0 开发期开过标题栏调试统计（FPS/CPU/GPU），0.3.1 起关闭——窗口标题
        // 保持干净的「TinyNext 下载器」。需要排障时再开。
        .showDebugStatsInTitle(false)
        .textFont("NotoSansSC-Regular.ttf")
        .iconFont("FontAwesome7.otf")
        // 系统托盘强制常驻：托盘是「CLI/扩展拉起后直接进托盘待机」（不弹窗）和
        // 「后台退出入口」的前提，不再提供关闭托盘的选项。close_to_tray 配置只
        // 决定 X 关闭行为（缩托盘 / 直接退出，见 compose 尾部），eui 只在启动时
        // 读一次，改后重启生效。
        .tray(true)
        .trayTitle("TinyNext 下载器")
#ifdef _WIN32
        .trayIcon("assets/icon.ico")
#else
        .trayIcon("assets/icon.png")
#endif
        ;
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    // 初始化后的原生窗口比例才可靠；显示器 DPI 变化时也维持完整的逻辑画布。
    ensureMinimumCanvas(screen.width, screen.height, kMinPageWidth, kMinPageHeight);
    // 启动一次后台线程（幂等）：CLI 转发监听（阻塞 accept，空闲挂起）+ 杂务
    // （状态消息过期 / 下载通知 / 活动任务进度刷新）。都只在「真有事」时
    // requestUiUpdate() 唤醒 UI 一帧，空闲时 UI 睡眠、零渲染。
    static bool housekeepingStarted = false;
    if (!housekeepingStarted) {
        housekeepingStarted = true;
        // Register engine shutdown before the workers below: atexit runs in
        // reverse order, so warmup/housekeeping/CLI/updater workers stop first.
        std::atexit([] { g_tasks.shutdown(); });
        // 组件更新（领域层无 eui）：注入 UI 唤醒，工作线程状态变化才能刷新界面。
        updater::setWakeUi([] {
            if (!g_appShuttingDown.load()) core::platform::requestUiUpdate();
        });
        // 浏览器扩展宿主注册（幂等 best-effort）：写 Native Messaging 清单并
        // 登记 Chrome。扩展首连前至少要跑过一次 GUI；失败静默（扩展 popup 的
        // 「检测」会提示未注册）。
        browser_host::registerChromeNativeHost();
        updater::prepareForShutdown(g_tasks.engine());
        cli::startCliIpc();
        housekeep::startHousekeeping();
        // 启动即后台拉起引擎并恢复上次会话的历史任务（此前是懒惰拉取：首次
        // 下载才 spawn daemon，历史记录要等下一次下载才出现）。与 UI 线程的
        // start() 经引擎内部 daemonMutex_ 互斥；完成后唤醒 UI 渲染任务列表。
        g_warmupThread = std::thread([] {
            g_tasks.warmup();
            if (!g_tasks.engineActive()) g_warmupFailed.store(true);
            g_warmupDone.store(true);
            // 顺手探测引擎版本（组件更新页的当前版本；一次进程启动，放预热
            // 线程而非 UI 线程做）。
            updater::probeAria2Version();
            if (!g_appShuttingDown.load()) core::platform::requestUiUpdate();
        });
        // g_tasks is a process-lifetime singleton. Join its warmup worker before
        // explicit engine shutdown and static teardown.
        std::atexit([] {
            if (g_warmupThread.joinable()) g_warmupThread.join();
        });
        // Set this before joining warmup so it skips its final UI wake during exit.
        std::atexit([] { g_appShuttingDown.store(true); });
    }

    // 预热失败（如引擎完整性校验不通过）：在 UI 线程给出具体原因。
    if (g_warmupFailed.exchange(false)) {
        const std::string err = g_tasks.lastError();
        showStatus(err.empty() ? tr("app.startup.no_history")
                               : trf("app.startup.failed", err));
    }

    // 预热完成 + 引擎可用 + 开了「启动时自动重试失败任务」：对恢复出来的
    // Failed 任务自动续传（与卡片 ↻ 同路径），有触发才提示。
    if (g_warmupDone.exchange(false) && cfg::autoRetryFailed() &&
        g_tasks.engineActive()) {
        if (const int n = g_tasks.retryFailedTasks(); n > 0) {
            showStatus(trf("app.startup.retried_failed", n));
        }
    }

    // 事件驱动的杂务消费（取代旧的根 onFrame；onFrame 会让 eui 每帧强制重绘）。
    cli::processPendingUrls();                 // 自身 CLI URL + socket/inbox 转发 URL
    if (housekeep::consumeStatusExpired()) {   // 状态消息 4s 自动消失
        g_statusMessage.clear();
        g_statusTimer = 0.0f;
    }
    // 后台线程投递的状态消息（异步回收站结果等）——UI 线程逐条转成 showStatus。
    for (std::string& msg : drainStatus()) {
        showStatus(std::move(msg));
    }
    if (themeChangePending() && g_themeMode == cfg::ThemeMode::System) {
        g_dark = cfg::osDark();
    }
    static bool lastDark = !g_dark;            // 原生标题栏配色跟随主题
    if (g_dark != lastDark) {
        lastDark = g_dark;
        setNativeTheme(g_dark);
    }

    // 启动时设一次应用图标（窗口/任务栏）。
    static bool iconApplied = false;
    if (!iconApplied) {
        iconApplied = true;
        applyAppIcon();
    }

    // 启动一次 OS 主题变化 watcher（后台线程阻塞在系统事件上，幂等）。
    static bool themeWatcherStarted = false;
    if (!themeWatcherStarted) {
        themeWatcherStarted = true;
        startThemeWatcher();
    }

    const AppTheme& theme = currentTheme();

    // 根 Stack 管主题背景与全局弹层；页面分区定位，分区内用 Row / Column / Flow。
    //
    // 注意：这里不再挂 .onFrame。eui 会把挂 onFrame 的元素当成「每帧都在动」，
    // 无条件每帧 composeRequested/paintRequested/animating → 空闲也 90 FPS 全量
    // 重绘。周期/事件工作（CLI 转发、通知、状态计时、主题）都挪到了后台线程
    // （cli::startCliIpc / housekeep::startHousekeeping），只在真有事时唤醒 UI。
    ui.stack("root")
        .size(screen.width, screen.height)
        .content([&] {
            // 传统纯色背景（不花里胡哨）：深色纯黑 / 浅色米白，无渐变无主色柔光。
            ui.rect("theme.background")
                .position(0, 0)
                .size(screen.width, screen.height)
                .color(theme.components.background)
                .build();

            // 页面直接铺在主题背景上：分页器齿轮进设置、拼图进扩展。
            switch (g_page_view) {
                case Page::Downloads: drawDownloadsPage(ui, screen, theme); break;
                case Page::Settings:  drawSettingsPage(ui, screen, theme); break;
                case Page::Extensions: drawExtensionsPage(ui, screen, theme); break;
            }
        })
        .build();

    // 首帧收尾（一次性）：此时 URL 已处理、warmup/IPC/宿主注册全部就绪，可以
    // 应用关闭策略。
    //  - CLI 带下载参数 / 扩展宿主拉起的启动：合成一次「点 X」事件 → eui 的
    //    关闭回调把它转成「隐藏到托盘」，主循环进托盘待机（引擎/IPC/housekeep
    //    后台线程继续跑，托盘菜单「显示」唤回）。仅「关闭时缩到托盘」开启时
    //    生效；窗口会闪现一帧，属于 eui 机制下能做到的「直接进托盘」。
    //  - 用户关闭「关闭时缩到托盘」：托盘仍常驻，但 X 恢复「直接退出」语义
    //    （eui 只要托盘在就把 X 一律转成缩托盘，见 platform 注释）。
    static bool closePolicyApplied = false;
    if (!closePolicyApplied) {
        closePolicyApplied = true;
        if (!cfg::closeToTray()) {
            overrideWindowCloseToExit();
        } else if (cli::shouldStartHiddenToTray()) {
            requestHideWindowToTray();
        }
    }
}

} // namespace app
