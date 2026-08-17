// ui/theme.cppm — dark/light AppTheme + currentTheme().
//
// eui_neo.h is header-only (no module interface), so it is pulled into the
// global module fragment. Only eui types (eui::Color, components::theme tokens)
// cross this module's boundary — consumers must include eui_neo.h themselves.
module;

#include "eui_ui.h"

export module tinynext.ui.theme;

import std;
import tinynext.config;

// ---- 主题模式 / 设置 pending ----
// 主题三态：跟随系统 / 深色 / 浅色，持久化在 tinynext.conf 的 theme_mode。
// g_dark 是当前生效的深色布尔（System 模式时由 tinynext.ui.theme_watch 的事件
// 触发，重读 cfg::osDark() 更新，事件驱动而非轮询）。
export cfg::ThemeMode g_themeMode = cfg::themeMode();
export bool g_dark = cfg::effectiveDark();
// 关闭窗口行为（缩托盘开关）待提交值；点「保存」落盘（cfg::setCloseToTray），
// 重启后生效（dslAppConfig 启动时读取）。
export bool g_closeToTray = cfg::closeToTray();
// 设置页待提交的编辑值：主题只在点「保存」时写入配置并生效，点「放弃」回滚到
// 已保存值。主题在选择时即时预览（g_dark），但不落盘。
export cfg::ThemeMode g_pendingTheme = g_themeMode;

// 日间/夜间双主题。clearColor 在 eui 初始化时固化、无法运行时修改，所以
// 主题由 compose 全权控制 —— 用一个全屏背景矩形盖住窗口底色。整个系统的
// 所有颜色（背景、表面、主色、文本、状态色）都从 currentTheme() 取：
//   - 控件统一走 `.theme(theme.components)`，一套 tokens 管按钮/输入框/
//     进度条的内部配色（fill/文字/边框/hover/focus）；
//   - 文本等裸颜色从 theme 字段取，深浅主题各自定义，保证对比度；
//   - 后续新增任何控件，只要同样从 currentTheme() 取色，就自动与现有
//     UI 保持一致。compose 每帧重跑，切换即时生效。
export struct AppTheme {
    bool dark;
    eui::Color titleText;    // 大标题
    eui::Color nameText;     // 文件名
    eui::Color metaText;     // 百分比/速度等次要文本
    eui::Color hintText;     // 空态提示
    eui::Color statusText;   // 底部状态消息
    eui::Color downloading;  // 状态色
    eui::Color paused;
    eui::Color done;
    eui::Color failed;
    eui::Color idle;
    components::theme::ThemeColorTokens components;  // 传给组件的完整 tokens
};

// 主色不写死：夜间用亮蓝（暗底上醒目），日间用深蓝（浅底上对比好）。
// 两套主题共享同一套 Token 结构，只是颜色值不同。
export const AppTheme kDarkTheme = {
    true,
    {0.94f, 0.97f, 1.0f, 1.0f},
    {0.88f, 0.92f, 1.0f, 1.0f},
    {0.62f, 0.70f, 0.82f, 1.0f},
    {0.42f, 0.47f, 0.55f, 1.0f},
    {0.72f, 0.83f, 0.97f, 1.0f},
    {0.58f, 0.72f, 0.95f, 1.0f},
    {0.95f, 0.72f, 0.30f, 1.0f},
    {0.35f, 0.80f, 0.45f, 1.0f},
    {0.92f, 0.40f, 0.38f, 1.0f},
    {0.55f, 0.58f, 0.62f, 1.0f},
    [] {
        auto tokens = components::theme::dark();
        // 传统深色模式：纯黑背景 + 标准蓝主色（不花里胡哨，去掉霓虹色）。
        tokens.background = {0.08f, 0.08f, 0.08f, 1.0f};
        tokens.primary = {0.25f, 0.55f, 1.0f, 1.0f};
        // eui input 组件内部默认 `metrics_.typography.input = 17`（未按设计值书写）。
        // 0.2.10 改 uiScale 原生缩放后，app 自己的字号已回到设计值（标签 11-12），
        // 这个 17 却仍按设计值放大 → 输入框文字被放得比标签大 ~60%（比标题还大）。
        // 这里把 input 默认字号覆写成设计值 13（比标签略大、不喧宾夺主）。
        tokens.metrics.typography.input = 13.0f;
        return tokens;
    }(),
};

export const AppTheme kLightTheme = {
    false,
    {0.16f, 0.20f, 0.30f, 1.0f},
    {0.10f, 0.13f, 0.20f, 1.0f},
    {0.42f, 0.48f, 0.58f, 1.0f},
    {0.55f, 0.60f, 0.68f, 1.0f},
    {0.18f, 0.38f, 0.62f, 1.0f},
    {0.18f, 0.42f, 0.82f, 1.0f},
    {0.72f, 0.48f, 0.05f, 1.0f},
    {0.10f, 0.55f, 0.25f, 1.0f},
    {0.78f, 0.20f, 0.14f, 1.0f},
    {0.48f, 0.52f, 0.58f, 1.0f},
    [] {
        auto tokens = components::theme::light();
        tokens.primary = {0.02f, 0.62f, 0.72f, 1.0f};   // 浅色模式主色：青色
        tokens.background = {0.97f, 0.95f, 0.90f, 1.0f};  // 浅色模式背景：米白
        // 与深色主题一致：input 默认字号覆写为设计值 13（见 kDarkTheme 注释）。
        tokens.metrics.typography.input = 13.0f;
        return tokens;
    }(),
};

// 当前生效主题：读本模块的 g_dark（System 模式实时跟随 OS）。
export const AppTheme& currentTheme() {
    return g_dark ? kDarkTheme : kLightTheme;
}
