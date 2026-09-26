// ui/theme.cppm — 黑白极客风双主题（亮/暗）+ currentTheme()。
//
// 色板是纯中性灰镜像体系（对标 Codex / Claude Code 页面的 monochrome geek 风）：
// 主色即「反白」——暗主题 primary=纯白/onPrimary=近黑，亮主题镜像反转；层次靠
// 灰阶梯（surfaceContainerLow..Highest）+ 1px hairline 描边（outline），不靠
// 投影与彩色。状态色仅保留哑化的绿/红做功能区分，活动态用单色灰强调。
// （字段名沿用 M3 角色名只是控制改动面，语义已是单色体系。）
//
// eui_neo.h is header-only (no module interface), so it is pulled into the
// global module fragment. Only eui types (eui::Color, components::theme tokens)
// cross this module's boundary — consumers must include eui_ui.h themselves.
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
//
// 语义分层（黑白极客）：
//   surface            页面底层（窗口背景）
//   surfaceContainer*  灰阶梯容器层（岛卡/任务卡/弹层），Low<Default<High<Highest
//   primary/onPrimary  反白强调（主按钮、激活指示、进度条）：暗=白/黑，亮=黑/白
//   outline*           hairline 描边/分隔（扁平化的层次来源）
//   inverse*           tooltip/snackbar 反色面
export struct AppTheme {
    bool dark;
    // -- 文本角色（= onSurface / onSurfaceVariant 的应用侧别名） --
    eui::Color titleText;    // 大标题        = onSurface
    eui::Color nameText;     // 文件名        = onSurface
    eui::Color metaText;     // 次要文本      = onSurfaceVariant
    eui::Color hintText;     // 空态提示      = onSurfaceVariant（α 降显弱）
    eui::Color statusText;   // 状态消息      = onSurfaceVariant
    // -- 状态色 --
    eui::Color downloading;  // 活动态 = primary
    eui::Color paused;
    eui::Color done;
    eui::Color failed;
    eui::Color idle;
    // -- 灰阶角色（字段名沿用 M3 叫法，语义已是单色体系） --
    eui::Color primary;
    eui::Color onPrimary;
    eui::Color primaryContainer;
    eui::Color onPrimaryContainer;
    eui::Color secondaryContainer;
    eui::Color onSecondaryContainer;
    eui::Color tertiaryContainer;
    eui::Color error;
    eui::Color errorContainer;
    eui::Color onErrorContainer;
    eui::Color surface;
    eui::Color surfaceContainerLow;
    eui::Color surfaceContainer;
    eui::Color surfaceContainerHigh;
    eui::Color surfaceContainerHighest;
    eui::Color onSurface;
    eui::Color onSurfaceVariant;
    eui::Color outline;
    eui::Color outlineVariant;
    eui::Color inverseSurface;
    eui::Color onInverseSurface;
    eui::Color scrim;
    // -- 语义容器层（从上面的灰阶角色派生） --
    // 岛卡必须比窗口背景（=surface）「抬」一层：暗色向亮抬（surfaceContainerLow），
    // 亮色用最亮一档（白）。内容卡再比岛卡收一层。扁平风格下层次主要靠
    // hairline 描边，灰阶差只做辅助。
    eui::Color panelBg;   // dark=surfaceContainerLow #111 / light=白 #FFF
    eui::Color cardBg;    // dark=surfaceContainer #161616 / light=#F5F5F5
    components::theme::ThemeColorTokens components;  // 传给组件的完整 tokens
};

// state layer：交互控件 hover/pressed 时在底色上叠加 onColor@α
// （hover 8% / pressed 12%）。单色体系下叠加的是白/黑灰雾，依旧成立。
export eui::Color stateLayer(eui::Color onColor, float alpha) {
    return components::theme::withAlpha(onColor, alpha);
}

// 深色主题：纯中性灰（无蓝调），亮暗两板严格镜像。
export const AppTheme kDarkTheme = {
    true,
    {0.961f, 0.961f, 0.961f, 1.0f},   // 标题 = onSurface #F5F5F5
    {0.961f, 0.961f, 0.961f, 1.0f},   // 文件名 = onSurface
    {0.612f, 0.612f, 0.612f, 1.0f},   // 次要文本 = onSurfaceVariant #9C9C9C
    {0.612f, 0.612f, 0.612f, 0.70f},  // 空态提示（α 弱化）
    {0.612f, 0.612f, 0.612f, 1.0f},   // 状态消息 = onSurfaceVariant
    {0.860f, 0.860f, 0.860f, 1.0f},   // 下载中 = 近白单色强调（活动态不上彩色）
    {0.600f, 0.600f, 0.600f, 1.0f},   // 暂停 灰
    {0.550f, 0.820f, 0.600f, 1.0f},   // 完成 哑化绿（仅状态用彩色）
    {0.920f, 0.480f, 0.440f, 1.0f},   // 失败 哑化红
    {0.500f, 0.500f, 0.500f, 1.0f},   // 空闲 灰
    // ---- 灰阶角色 ----
    {1.0f, 1.0f, 1.0f, 1.0f},         // primary        #FFFFFF（反白主色）
    {0.039f, 0.039f, 0.039f, 1.0f},   // onPrimary      #0A0A0A
    {0.141f, 0.141f, 0.141f, 1.0f},   // primaryCont.   = surfContHighst #242424
    {0.961f, 0.961f, 0.961f, 1.0f},   // onPrimaryCont. = onSurface
    {0.110f, 0.110f, 0.110f, 1.0f},   // secondaryCont. = surfContHigh #1C1C1C
    {0.961f, 0.961f, 0.961f, 1.0f},   // onSecondaryCt. = onSurface
    {0.110f, 0.110f, 0.110f, 1.0f},   // tertiaryCont.  = surfContHigh
    {0.920f, 0.480f, 0.440f, 1.0f},   // error          = failed 哑红
    {0.300f, 0.100f, 0.090f, 1.0f},   // errorContainer 暗红底
    {0.950f, 0.850f, 0.830f, 1.0f},   // onErrorCont.
    {0.039f, 0.039f, 0.039f, 1.0f},   // surface        #0A0A0A（近纯黑）
    {0.067f, 0.067f, 0.067f, 1.0f},   // surfContLow    #111111
    {0.086f, 0.086f, 0.086f, 1.0f},   // surfCont       #161616
    {0.110f, 0.110f, 0.110f, 1.0f},   // surfContHigh   #1C1C1C
    {0.141f, 0.141f, 0.141f, 1.0f},   // surfContHighst #242424
    {0.961f, 0.961f, 0.961f, 1.0f},   // onSurface      #F5F5F5
    {0.612f, 0.612f, 0.612f, 1.0f},   // onSurfaceVar.  #9C9C9C
    {0.180f, 0.180f, 0.180f, 1.0f},   // outline        #2E2E2E（hairline）
    {0.137f, 0.137f, 0.137f, 1.0f},   // outlineVariant #232323
    {0.961f, 0.961f, 0.961f, 1.0f},   // inverseSurface #F5F5F5
    {0.067f, 0.067f, 0.067f, 1.0f},   // onInverseSurf. #111111
    {0.0f, 0.0f, 0.0f, 0.50f},        // scrim α50%
    {0.067f, 0.067f, 0.067f, 1.0f},   // panelBg = surfContLow #111111
    {0.086f, 0.086f, 0.086f, 1.0f},   // cardBg  = surfCont    #161616
    [] {
        auto tokens = components::theme::dark();
        // 单色映射：primary=纯白（反白主色，组件填充态的黑字由 onPrimaryColor
        // 翻转，见 widgets.cppm）；surface*=灰阶梯；border=outline（hairline）。
        tokens.background = {0.039f, 0.039f, 0.039f, 1.0f};
        tokens.primary = {1.0f, 1.0f, 1.0f, 1.0f};
        tokens.surface = {0.067f, 0.067f, 0.067f, 1.0f};
        tokens.surfaceHover = {0.086f, 0.086f, 0.086f, 1.0f};
        tokens.surfaceActive = {0.110f, 0.110f, 0.110f, 1.0f};
        tokens.text = {0.961f, 0.961f, 0.961f, 1.0f};
        tokens.border = {0.180f, 0.180f, 0.180f, 1.0f};
        // eui input 组件内部默认 `metrics_.typography.input = 17`（未按设计值书写）。
        // uiScale 原生缩放后，app 字号已回到设计值（标签 11-12），这个 17 却仍按
        // 设计值放大 → 输入框文字比标签大 ~60%。覆写为设计值 13。
        tokens.metrics.typography.input = 13.0f;
        return tokens;
    }(),
};

// 浅色主题：同一套灰阶的亮调镜像（primary 反转为近黑）。
export const AppTheme kLightTheme = {
    false,
    {0.067f, 0.067f, 0.067f, 1.0f},   // 标题 = onSurface #111111
    {0.067f, 0.067f, 0.067f, 1.0f},   // 文件名 = onSurface
    {0.420f, 0.420f, 0.420f, 1.0f},   // 次要文本 = onSurfaceVariant #6B6B6B
    {0.420f, 0.420f, 0.420f, 0.70f},  // 空态提示（α 弱化）
    {0.420f, 0.420f, 0.420f, 1.0f},   // 状态消息 = onSurfaceVariant
    {0.150f, 0.150f, 0.150f, 1.0f},   // 下载中 = 近黑单色强调
    {0.550f, 0.550f, 0.550f, 1.0f},   // 暂停 灰
    {0.100f, 0.550f, 0.300f, 1.0f},   // 完成 哑化绿
    {0.800f, 0.250f, 0.200f, 1.0f},   // 失败 哑化红
    {0.500f, 0.500f, 0.500f, 1.0f},   // 空闲 灰
    // ---- 灰阶角色 ----
    {0.067f, 0.067f, 0.067f, 1.0f},   // primary        #111111（反黑主色）
    {1.0f, 1.0f, 1.0f, 1.0f},         // onPrimary      #FFFFFF
    {0.898f, 0.898f, 0.898f, 1.0f},   // primaryCont.   = surfContHighst #E5E5E5
    {0.067f, 0.067f, 0.067f, 1.0f},   // onPrimaryCont. = onSurface
    {0.933f, 0.933f, 0.933f, 1.0f},   // secondaryCont. = surfContHigh #EEEEEE
    {0.067f, 0.067f, 0.067f, 1.0f},   // onSecondaryCt. = onSurface
    {0.933f, 0.933f, 0.933f, 1.0f},   // tertiaryCont.  = surfContHigh
    {0.800f, 0.250f, 0.200f, 1.0f},   // error          = failed 哑红
    {0.980f, 0.900f, 0.880f, 1.0f},   // errorContainer 浅红底
    {0.500f, 0.100f, 0.080f, 1.0f},   // onErrorCont.
    {0.980f, 0.980f, 0.980f, 1.0f},   // surface        #FAFAFA
    {1.0f, 1.0f, 1.0f, 1.0f},         // surfContLow    #FFFFFF
    {0.961f, 0.961f, 0.961f, 1.0f},   // surfCont       #F5F5F5
    {0.933f, 0.933f, 0.933f, 1.0f},   // surfContHigh   #EEEEEE
    {0.898f, 0.898f, 0.898f, 1.0f},   // surfContHighst #E5E5E5
    {0.067f, 0.067f, 0.067f, 1.0f},   // onSurface      #111111
    {0.420f, 0.420f, 0.420f, 1.0f},   // onSurfaceVar.  #6B6B6B
    {0.851f, 0.851f, 0.851f, 1.0f},   // outline        #D9D9D9（hairline）
    {0.898f, 0.898f, 0.898f, 1.0f},   // outlineVariant #E5E5E5
    {0.102f, 0.102f, 0.102f, 1.0f},   // inverseSurface #1A1A1A
    {0.961f, 0.961f, 0.961f, 1.0f},   // onInverseSurf. #F5F5F5
    {0.0f, 0.0f, 0.0f, 0.50f},        // scrim α50%
    {1.0f, 1.0f, 1.0f, 1.0f},         // panelBg = surfContLow #FFFFFF
    {0.961f, 0.961f, 0.961f, 1.0f},   // cardBg  = surfCont    #F5F5F5
    [] {
        auto tokens = components::theme::light();
        tokens.background = {0.980f, 0.980f, 0.980f, 1.0f};
        tokens.primary = {0.067f, 0.067f, 0.067f, 1.0f};
        tokens.surface = {1.0f, 1.0f, 1.0f, 1.0f};
        tokens.surfaceHover = {0.961f, 0.961f, 0.961f, 1.0f};
        tokens.surfaceActive = {0.933f, 0.933f, 0.933f, 1.0f};
        tokens.text = {0.067f, 0.067f, 0.067f, 1.0f};
        tokens.border = {0.851f, 0.851f, 0.851f, 1.0f};
        // 与深色主题一致：input 默认字号覆写为设计值 13（见 kDarkTheme 注释）。
        tokens.metrics.typography.input = 13.0f;
        return tokens;
    }(),
};

// 当前生效主题：读本模块的 g_dark（System 模式实时跟随 OS）。
export const AppTheme& currentTheme() {
    return g_dark ? kDarkTheme : kLightTheme;
}
