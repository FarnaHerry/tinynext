// app.cpp — thin entry for the TinyNext UI.
//
// The `app-main` feature of compat.eui-neo supplies main()
// (core/app/glfw_app_main.cpp); this TU defines the two symbols every EUI
// application must provide — app::dslAppConfig() and app::compose() — and
// delegates drawing to the tinynext.ui.* modules:
//
//   tinynext.ui.utils    scale factor + formatting/parse helpers
//   tinynext.ui.theme    dark/light AppTheme + currentTheme()
//   tinynext.ui.state    shared globals + engine + add-download flow
//   tinynext.ui.platform DPI boot + folder picker + open helpers
//   tinynext.ui.widgets  list picker + sidebar/rail/card-action controls
//   tinynext.ui.cards    the download task card
//   tinynext.ui.pages    downloads / settings pages, about dialog, compose
//   tinynext.cli         single-instance + CLI (boot + inbox polling)
//
// eui_neo.h is included HERE (full, with eui/detail/dsl_app_impl.h): this TU
// provides the app::* machinery (app::update / render / initialize / ...) that
// dsl_app_impl.h defines. The tinynext.ui.* modules instead include the reduced
// "ui/eui_ui.h" (eui_neo.h minus dsl_app_impl.h) — its inline lambdas would
// otherwise clash (same mangled name) between this plain TU and the modules'
// global-module-fragment copies.
#include <eui_neo.h>

import std;
import tinynext.config;
import tinynext.cli;
import tinynext.ui.utils;
import tinynext.ui.theme;
import tinynext.ui.widgets;
import tinynext.ui.pages;
import tinynext.ui.state;

namespace app {

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("TinyNext 下载器")
        .pageId("tinynext")
        .clearColor({0.075f, 0.085f, 0.105f, 1.0f})
        .windowSize(static_cast<int>(S(920.0f)), static_cast<int>(S(620.0f)))
        .fps(90.0)
        .showDebugStatsInTitle(false)
        .textFont("JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf")
        .iconFont("FontAwesome7.otf");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    const AppTheme& theme = currentTheme();

    // 根用 stack：底层铺满窗口的主题背景（clearColor 在初始化时固化、无法运行时
    // 修改，所以背景色由 compose 每帧重绘，主题切换即时生效），其余控件用
    // .position() 绝对定位 —— eui 的 flex 引擎会压缩/居中固定尺寸子项，导致分页
    // 大小下拉与翻页被裁切；绝对定位则完全可控、随窗口高度自适应。
    ui.stack("root")
        .size(screen.width, screen.height)
        .onFrame([](float deltaSeconds) {
            if (g_statusTimer > 0.0f) {
                g_statusTimer -= deltaSeconds;
            }
            // Follow-system mode: re-read the OS theme every ~2s so a system
            // dark/light switch is picked up live (registry read is cheap).
            if (g_themeMode == cfg::ThemeMode::System) {
                g_systemThemeTimer += deltaSeconds;
                if (g_systemThemeTimer >= 2.0f) {
                    g_systemThemeTimer = 0.0f;
                    g_dark = cfg::osDark();
                }
            }
            // 单实例 inbox 轮询 + CLI 启动参数（tinynext.cli）。
            cli::handleCliAndInbox(deltaSeconds);
        })
        .content([&] {
            ui.rect("theme.background")
                .position(0, 0)
                .size(screen.width, screen.height)
                .color(theme.components.background)
                .build();

            // ===================== 主侧边栏（图标栏） =====================
            // 只放图标：左上角应用 logo（项目名称，特例做成圆角图标块），
            // 下面是各应用页图标，底部是主题切换按钮 —— 无文字，保持简洁。
            // 下载状态筛选不在主侧边栏，而是下载页内容区左侧自己的子侧边栏。
            ui.stack("sidebar")
                .position(0, 0)
                .size(kRailWidth, screen.height)
                .zIndex(5)
                .content([&] {
                    ui.rect("sidebar.bg")
                        .position(0, 0)
                        .size(kRailWidth, screen.height)
                        .color(theme.components.surface)
                        .build();

                    // 应用 logo：项目名缩写 "TN"（TinyNext），主色圆角块特例。
                    ui.rect("sidebar.logo.bg")
                        .position(S(4.0f), S(10.0f))
                        .size(S(18.0f), S(18.0f))
                        .color(theme.components.primary)
                        .radius(S(5.0f))
                        .build();
                    ui.text("sidebar.logo")
                        .position(0, S(10.0f))
                        .size(kRailWidth, S(18.0f))
                        .text("TN")
                        .fontSize(S(8.0f))
                        .lineHeight(S(18.0f))
                        .color(theme.dark ? theme.components.surface
                                          : theme.components.background)
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .verticalAlign(core::VerticalAlign::Center)
                        .build();

                    // 应用页导航：下载列表（默认第一页）/ 设置。
                    float railY = S(40.0f);
                    drawRailItem(ui, "nav.downloads", railY, kRailWidth, 0xF03A,
                                 g_page_view == Page::Downloads, theme,
                                 [] { g_page_view = Page::Downloads; });
                    railY += S(30.0f);
                    drawRailItem(ui, "nav.settings", railY, kRailWidth, 0xF013,
                                 g_page_view == Page::Settings, theme,
                                 [] { g_page_view = Page::Settings; });

                    // 关于：主题切换上方，信息图标（circle-info），打开软件信息弹窗。
                    components::button(ui, "rail.info")
                        .position((kRailWidth - S(22.0f)) * 0.5f, screen.height - S(54.0f))
                        .size(S(22.0f), S(22.0f))
                        .icon(0xF05A)  // circle-info
                        .text("")
                        .iconSize(S(11.0f))
                        .theme(theme.components, false)
                        .onClick([] { g_aboutOpen = true; })
                        .build();

                    // 主题切换：底部，仅图标（月亮/太阳）。
                    components::button(ui, "theme.toggle")
                        .position((kRailWidth - S(22.0f)) * 0.5f, screen.height - S(28.0f))
                        .size(S(22.0f), S(22.0f))
                        .icon(g_dark ? 0xF186 : 0xF185)  // moon / sun
                        .text("")
                        .iconSize(S(11.0f))
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
