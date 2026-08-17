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
//   tinynext.ui.widgets       list picker + sidebar/rail/card-action controls
//   tinynext.ui.cards         the download task card
//   tinynext.ui.downloads_page  downloads page + add-download dialog
//   tinynext.ui.settings_page   settings page
//   tinynext.ui.about_dialog    about dialog
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
import tinynext.ui.widgets;
import tinynext.ui.downloads_page;
import tinynext.ui.settings_page;
import tinynext.ui.about_dialog;
import tinynext.ui.platform;
import tinynext.ui.housekeep;
import tinynext.store.tasks;    // g_tasks（启动预热 warmup）
import tinynext.store.ui;       // 状态消息 / 页面
import tinynext.store.dialogs;  // g_aboutOpen

namespace app {

// 启动预热结果（后台线程 → UI 线程）：warmup 失败时置位，compose 下一帧消费并
// 弹状态消息（showStatus 只能 UI 线程写，所以走原子标志中转）。
std::atomic<bool> g_warmupFailed{false};

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("TinyNext 下载器")
        .pageId("tinynext")
        .clearColor({0.075f, 0.085f, 0.105f, 1.0f})
        // 原生全局缩放（eui-neo 0.5.6）：uiScale 按 dpiScale*uiScale 放大整个逻辑
        // 坐标系（布局+字号），所有尺寸按设计逻辑像素书写、不再 S() 自乘。窗口
        // 物理尺寸 = 设计尺寸 * kUI（eui 创建窗口时按物理像素，不会自动乘 uiScale）。
        .uiScale(kUI)
        .windowSize(static_cast<int>(1120.0f * kUI), static_cast<int>(720.0f * kUI))
        // 最大帧率写 0 = 自动匹配显示器刷新率（eui 的 updateFrameInterval 在
        // limit<=0 时直接用 getWindowRefreshRate）。
        .fps(0.0)
        // 0.3.0 开发期开过标题栏调试统计（FPS/CPU/GPU），0.3.1 起关闭——窗口标题
        // 保持干净的「TinyNext 下载器」。需要排障时再开。
        .showDebugStatsInTitle(false)
        .textFont("NotoSansSC-Regular.ttf")
        .iconFont("FontAwesome7.otf")
        // 系统托盘：配置 close_to_tray 决定 X 是否缩到托盘（不退出），托盘菜单
        // 「显示/退出」。仅 Windows/macOS 生效（Linux 的 eui-neo 配方托盘为 stub）；
        // 设置页可改，重启生效。Windows 图标要 .ico（Shell_NotifyIcon），其余用 PNG。
        .tray(cfg::closeToTray())
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
    // 启动一次后台线程（幂等）：CLI 转发监听（阻塞 accept，空闲挂起）+ 杂务
    // （状态消息过期 / 下载通知 / 活动任务进度刷新）。都只在「真有事」时
    // requestUiUpdate() 唤醒 UI 一帧，空闲时 UI 睡眠、零渲染。
    static bool housekeepingStarted = false;
    if (!housekeepingStarted) {
        housekeepingStarted = true;
        cli::startCliIpc();
        housekeep::startHousekeeping();
        // 启动即后台拉起引擎并恢复上次会话的历史任务（此前是懒惰拉取：首次
        // 下载才 spawn daemon，历史记录要等下一次下载才出现）。与 UI 线程的
        // start() 经引擎内部 daemonMutex_ 互斥；完成后唤醒 UI 渲染任务列表。
        std::thread([] {
            g_tasks.warmup();
            if (!g_tasks.engineActive()) g_warmupFailed.store(true);
            core::platform::requestUiUpdate();
        }).detach();
    }

    // 预热失败（如引擎完整性校验不通过）：在 UI 线程给出具体原因。
    if (g_warmupFailed.exchange(false)) {
        const std::string err = g_tasks.lastError();
        showStatus(err.empty() ? tr("下载引擎启动失败：历史任务未恢复",
                                    "Engine startup failed: history not restored")
                               : trf("下载引擎启动失败：{}",
                                     "Engine startup failed: {}", err));
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

    // 根用 stack：底层铺满窗口的主题背景（clearColor 在初始化时固化、无法运行时
    // 修改，所以背景色由 compose 重绘，主题切换即时生效），其余控件用 .position()
    // 绝对定位 —— eui 的 flex 引擎会压缩/居中固定尺寸子项，导致分页大小下拉与翻页
    // 被裁切；绝对定位则完全可控、随窗口高度自适应。
    //
    // 注意：这里不再挂 .onFrame。eui 会把挂 onFrame 的元素当成「每帧都在动」，
    // 无条件每帧 composeRequested/paintRequested/animating → 空闲也 90 FPS 全量
    // 重绘。周期/事件工作（CLI 转发、通知、状态计时、主题）都挪到了后台线程
    // （cli::startCliIpc / housekeep::startHousekeeping），只在真有事时唤醒 UI。
    ui.stack("root")
        .size(screen.width, screen.height)
        .content([&] {
            // 根背景：渐变 + 角落主色柔光斑。毛玻璃面板（drawPanel 的 backdrop blur）
            // 背后需要有带边缘/颜色的细节才看得出磨砂感——纯色或平滑渐变被模糊后
            // 看不出差别。官方 demo 的玻璃卡也是叠在彩色背景上（blur 18）。
            const auto& bgTok = theme.components;
            // 深色用霓虹主色（亮光斑 + 渐变），浅色用米白背景（几乎无彩，保持干净）。
            const float glowAlpha = bgTok.dark ? 0.16f : 0.05f;
            const float gradMix = bgTok.dark ? 0.16f : 0.05f;
            ui.rect("theme.background")
                .position(0, 0)
                .size(screen.width, screen.height)
                .gradient(bgTok.background,
                          core::mixColor(bgTok.background, bgTok.primary, gradMix))
                .build();
            // 三处主色柔光（大圆角 + 低 alpha）：玻璃岛卡/弹窗会糊掉它们形成可见
            // 磨砂感。中央那团专门垫在弹窗背后——弹窗居中，角落柔光够不到它。
            const core::Color glow = {bgTok.primary.r, bgTok.primary.g, bgTok.primary.b, glowAlpha};
            ui.rect("theme.glow.tl")
                .position(-120.0f, -140.0f)
                .size(360.0f, 300.0f)
                .color(glow)
                .radius(160.0f)
                .build();
            ui.rect("theme.glow.br")
                .position(screen.width - 260.0f, screen.height - 260.0f)
                .size(340.0f, 300.0f)
                .color(glow)
                .radius(170.0f)
                .build();
            ui.rect("theme.glow.c")
                .position(screen.width * 0.5f - 240.0f, screen.height * 0.5f - 220.0f)
                .size(480.0f, 440.0f)
                .color(glow)
                .radius(220.0f)
                .build();

            // ===================== 主侧边栏（图标栏） =====================
            // 总侧边栏不套卡片、也不铺底色：整列直接透明，logo/导航/底部按钮浮在
            // 页面背景上，让主题背景透出来更突出。左侧这列 kRailWidth 宽作为锚点，
            // 悬浮的"岛"卡片（状态子侧边栏 / 内容卡）从它右侧起排。
            ui.stack("sidebar")
                .position(0, 0)
                .size(kRailWidth, screen.height)
                .zIndex(5)
                .content([&] {

                    // 应用 logo：项目名缩写 "TN"（TinyNext），主色圆角块特例。
                    // 水平居中于图标栏（rail 加宽后不能写死 4，要按 kRailWidth 计算）。
                    ui.rect("sidebar.logo.bg")
                        .position((kRailWidth - 18.0f) * 0.5f, 10.0f)
                        .size(18.0f, 18.0f)
                        .color(theme.components.primary)
                        .radius(5.0f)
                        .build();
                    ui.text("sidebar.logo")
                        .position(0, 10.0f)
                        .size(kRailWidth, 18.0f)
                        .text("TN")
                        .fontSize(8.0f)
                        .lineHeight(18.0f)
                        .color(theme.dark ? theme.components.surface
                                          : theme.components.background)
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .verticalAlign(core::VerticalAlign::Center)
                        .build();

                    // 应用页导航：下载列表（默认第一页）/ 设置。
                    float railY = 40.0f;
                    drawRailItem(ui, "nav.downloads", railY, kRailWidth, 0xF03A,
                                 g_page_view == Page::Downloads, theme,
                                 [] { g_page_view = Page::Downloads; });
                    railY += 30.0f;
                    drawRailItem(ui, "nav.settings", railY, kRailWidth, 0xF013,
                                 g_page_view == Page::Settings, theme,
                                 [] { g_page_view = Page::Settings; });

                    // 关于：主题切换上方，信息图标（circle-info），打开软件信息弹窗。
                    components::button(ui, "rail.info")
                        .position((kRailWidth - 22.0f) * 0.5f, screen.height - 54.0f)
                        .size(22.0f, 22.0f)
                        .icon(0xF05A)  // circle-info
                        .text("")
                        .iconSize(11.0f)
                        .theme(theme.components, false)
                        .onClick([] { g_aboutOpen = true; })
                        .build();

                    // 主题切换：底部，仅图标（月亮/太阳）。
                    components::button(ui, "theme.toggle")
                        .position((kRailWidth - 22.0f) * 0.5f, screen.height - 28.0f)
                        .size(22.0f, 22.0f)
                        .icon(g_dark ? 0xF186 : 0xF185)  // moon / sun
                        .text("")
                        .iconSize(11.0f)
                        .theme(theme.components, false)
                        .onClick([] {
                            // Quick flip switches to the opposite explicit mode
                            // (overrides follow-system) and persists immediately;
                            // the settings pending value follows so a later
                            // 「保存」 doesn't clobber it.
                            g_themeMode = g_dark ? cfg::ThemeMode::Light
                                                 : cfg::ThemeMode::Dark;
                            g_pendingTheme = g_themeMode;
                            g_dark = !g_dark;
                            cfg::setThemeMode(g_themeMode);
                        })
                        .build();
                })
                .build();

            // ===================== 内容区（页面分发） =====================
            if (g_page_view == Page::Downloads) {
                drawDownloadsPage(ui, screen, theme);
            } else {
                drawSettingsPage(ui, screen, theme);
            }

            // 关于弹窗（所有页面可见）。
            drawAboutDialog(ui, screen, theme);
        })
        .build();
}

} // namespace app
