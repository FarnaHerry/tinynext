// ui/theme.cppm — Material 3 双主题（亮/暗）+ currentTheme()。
//
// 色板按 Material 3 tonal 体系从 seed 蓝 #3871E0（eui 库默认强调色，app 黑白化
// 之前的品牌色）派生：primary/primaryContainer/secondaryContainer/tertiaryContainer
// + surface 五层容器（surfaceContainerLow..Highest）+ outline + error + inverse
// 全套角色。换品牌色只需替换 seed 派生出的这几个常量。
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
// 语义分层（Material 3）：
//   surface            页面底层（岛卡底、窗口背景）
//   surfaceContainer*  内容容器层（任务卡/统计卡/弹层），Low<Default<High<Highest
//   primary*           品牌强调（填充按钮、激活指示、进度条）
//   secondaryContainer 次级强调（segmented 容器、tonal 钮）
//   outline*           描边/分隔；inverse*  tooltip/snackbar 反色面
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
    // -- Material 3 角色 --
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
    // -- 语义容器层（从上面的 MD3 角色派生） --
    // 岛卡必须在窗口背景（=surface）上「抬」出层次：暗色向亮抬一层用
    // surfaceContainerLow；亮色的 Low 反而比 surface 暗，M3 里最亮的一档是
    // surfaceContainerLowest（≈白），岛卡用它。内容卡再比岛卡「反向」收一层。
    eui::Color panelBg;   // dark=surfaceContainerLow      / light=surfaceContainerLowest
    eui::Color cardBg;    // dark=surfaceContainer         / light=surfaceContainerLow
    components::theme::ThemeColorTokens components;  // 传给组件的完整 tokens
};

// Material 3 state layer：交互控件 hover/pressed 时在底色上叠加 onColor@α
// （hover 8% / pressed 12%），替代旧的 surfaceHover/surfaceActive 灰阶切换。
export eui::Color stateLayer(eui::Color onColor, float alpha) {
    return components::theme::withAlpha(onColor, alpha);
}

// 深色主题：seed #3871E0 派生的 M3 色板（值近似 MD3 tonal，可微调）。
export const AppTheme kDarkTheme = {
    true,
    {0.882f, 0.886f, 0.910f, 1.0f},   // 标题 = onSurface
    {0.882f, 0.886f, 0.910f, 1.0f},   // 文件名 = onSurface
    {0.769f, 0.776f, 0.812f, 1.0f},   // 次要文本 = onSurfaceVariant
    {0.769f, 0.776f, 0.812f, 0.70f},  // 空态提示（α 弱化）
    {0.769f, 0.776f, 0.812f, 1.0f},   // 状态消息 = onSurfaceVariant
    {0.659f, 0.780f, 0.980f, 1.0f},   // 下载中 = primary（活动态用品牌色）
    {0.70f, 0.71f, 0.76f, 1.0f},      // 暂停 灰
    {0.482f, 0.847f, 0.561f, 1.0f},   // 完成 MD3 绿
    {0.949f, 0.722f, 0.710f, 1.0f},   // 失败 MD3 红 error80
    {0.55f, 0.56f, 0.60f, 1.0f},      // 空闲 灰
    // ---- Material 3 角色 ----
    {0.659f, 0.780f, 0.980f, 1.0f},   // primary        #A8C7FA
    {0.024f, 0.180f, 0.435f, 1.0f},   // onPrimary      #062E6F
    {0.157f, 0.278f, 0.467f, 1.0f},   // primaryCont.   #284777
    {0.839f, 0.886f, 1.0f, 1.0f},     // onPrimaryCont. #D6E2FF
    {0.243f, 0.278f, 0.349f, 1.0f},   // secondaryCont. #3E4759
    {0.859f, 0.882f, 0.976f, 1.0f},   // onSecondaryCt. #DBE1F9
    {0.290f, 0.212f, 0.337f, 1.0f},   // tertiaryCont.  #4A3656
    {1.0f, 0.706f, 0.671f, 1.0f},     // error          #FFB4AB
    {0.576f, 0.0f, 0.039f, 1.0f},     // errorContainer #93000A
    {1.0f, 0.855f, 0.839f, 1.0f},     // onErrorCont.   #FFDAD6
    {0.063f, 0.075f, 0.102f, 1.0f},   // surface        #10131A（蓝灰黑）
    {0.098f, 0.110f, 0.133f, 1.0f},   // surfContLow    #191C22
    {0.114f, 0.125f, 0.153f, 1.0f},   // surfCont       #1D2027
    {0.157f, 0.173f, 0.204f, 1.0f},   // surfContHigh   #282C34
    {0.200f, 0.216f, 0.247f, 1.0f},   // surfContHighst #33373F
    {0.882f, 0.886f, 0.910f, 1.0f},   // onSurface      #E1E2E8
    {0.769f, 0.776f, 0.812f, 1.0f},   // onSurfaceVar.  #C4C6CF
    {0.557f, 0.565f, 0.600f, 1.0f},   // outline        #8E9099
    {0.263f, 0.278f, 0.306f, 1.0f},   // outlineVariant #43474E
    {0.886f, 0.882f, 0.902f, 1.0f},   // inverseSurface #E2E1E6
    {0.098f, 0.110f, 0.118f, 1.0f},   // onInverseSurf. #191C1E
    {0.0f, 0.0f, 0.0f, 0.32f},        // scrim α32%
    {0.098f, 0.110f, 0.133f, 1.0f},   // panelBg = surfContLow #191C22
    {0.114f, 0.125f, 0.153f, 1.0f},   // cardBg  = surfCont    #1D2027
    [] {
        auto tokens = components::theme::dark();
        // M3 映射：背景=页面 surface，surface*=容器层（组件 hover 走容器灰阶），
        // border=outlineVariant（发丝描边），primary=暗主题亮蓝。
        tokens.background = {0.063f, 0.075f, 0.102f, 1.0f};
        tokens.primary = {0.659f, 0.780f, 0.980f, 1.0f};
        tokens.surface = {0.098f, 0.110f, 0.133f, 1.0f};
        tokens.surfaceHover = {0.114f, 0.125f, 0.153f, 1.0f};
        tokens.surfaceActive = {0.157f, 0.173f, 0.204f, 1.0f};
        tokens.text = {0.882f, 0.886f, 0.910f, 1.0f};
        tokens.border = {0.263f, 0.278f, 0.306f, 1.0f};
        // eui input 组件内部默认 `metrics_.typography.input = 17`（未按设计值书写）。
        // uiScale 原生缩放后，app 字号已回到设计值（标签 11-12），这个 17 却仍按
        // 设计值放大 → 输入框文字比标签大 ~60%。覆写为设计值 13。
        tokens.metrics.typography.input = 13.0f;
        return tokens;
    }(),
};

// 浅色主题：同一 seed 的亮调 M3 色板。
export const AppTheme kLightTheme = {
    false,
    {0.098f, 0.110f, 0.118f, 1.0f},   // 标题 = onSurface
    {0.098f, 0.110f, 0.118f, 1.0f},   // 文件名 = onSurface
    {0.267f, 0.278f, 0.306f, 1.0f},   // 次要文本 = onSurfaceVariant
    {0.267f, 0.278f, 0.306f, 0.70f},  // 空态提示（α 弱化）
    {0.267f, 0.278f, 0.306f, 1.0f},   // 状态消息 = onSurfaceVariant
    {0.216f, 0.396f, 0.784f, 1.0f},   // 下载中 = primary（活动态用品牌色）
    {0.42f, 0.44f, 0.48f, 1.0f},      // 暂停 灰
    {0.118f, 0.482f, 0.267f, 1.0f},   // 完成 MD3 绿
    {0.702f, 0.149f, 0.118f, 1.0f},   // 失败 MD3 红 error40
    {0.42f, 0.44f, 0.48f, 1.0f},      // 空闲 灰
    // ---- Material 3 角色 ----
    {0.216f, 0.396f, 0.784f, 1.0f},   // primary        #3765C8
    {1.0f, 1.0f, 1.0f, 1.0f},         // onPrimary      #FFFFFF
    {0.851f, 0.886f, 1.0f, 1.0f},     // primaryCont.   #D9E2FF
    {0.0f, 0.102f, 0.255f, 1.0f},     // onPrimaryCont. #001A41
    {0.863f, 0.882f, 0.976f, 1.0f},   // secondaryCont. #DCE1F9
    {0.082f, 0.106f, 0.173f, 1.0f},   // onSecondaryCt. #151B2C
    {0.973f, 0.847f, 0.980f, 1.0f},   // tertiaryCont.  #F8D8FA
    {0.729f, 0.102f, 0.102f, 1.0f},   // error          #BA1A1A
    {1.0f, 0.855f, 0.839f, 1.0f},     // errorContainer #FFDAD6
    {0.255f, 0.0f, 0.008f, 1.0f},     // onErrorCont.   #410002
    {0.980f, 0.976f, 0.988f, 1.0f},   // surface        #FAF9FC
    {0.957f, 0.957f, 0.973f, 1.0f},   // surfContLow    #F4F4F8
    {0.933f, 0.933f, 0.953f, 1.0f},   // surfCont       #EEEEF3
    {0.910f, 0.910f, 0.937f, 1.0f},   // surfContHigh   #E8E8EF
    {0.886f, 0.886f, 0.918f, 1.0f},   // surfContHighst #E2E2EA
    {0.098f, 0.110f, 0.118f, 1.0f},   // onSurface      #191C1E
    {0.267f, 0.278f, 0.306f, 1.0f},   // onSurfaceVar.  #44474E
    {0.455f, 0.478f, 0.498f, 1.0f},   // outline        #74777F
    {0.769f, 0.776f, 0.816f, 1.0f},   // outlineVariant #C4C6D0
    {0.184f, 0.188f, 0.200f, 1.0f},   // inverseSurface #2F3033
    {0.945f, 0.941f, 0.957f, 1.0f},   // onInverseSurf. #F1F0F4
    {0.0f, 0.0f, 0.0f, 0.32f},        // scrim α32%
    {1.0f, 1.0f, 1.0f, 1.0f},         // panelBg = surfContLowest #FFFFFF
    {0.957f, 0.957f, 0.973f, 1.0f},   // cardBg  = surfContLow    #F4F4F8
    [] {
        auto tokens = components::theme::light();
        tokens.background = {0.980f, 0.976f, 0.988f, 1.0f};
        tokens.primary = {0.216f, 0.396f, 0.784f, 1.0f};
        tokens.surface = {0.957f, 0.957f, 0.973f, 1.0f};
        tokens.surfaceHover = {0.933f, 0.933f, 0.953f, 1.0f};
        tokens.surfaceActive = {0.910f, 0.910f, 0.937f, 1.0f};
        tokens.text = {0.098f, 0.110f, 0.118f, 1.0f};
        tokens.border = {0.769f, 0.776f, 0.816f, 1.0f};
        // 与深色主题一致：input 默认字号覆写为设计值 13（见 kDarkTheme 注释）。
        tokens.metrics.typography.input = 13.0f;
        return tokens;
    }(),
};

// 当前生效主题：读本模块的 g_dark（System 模式实时跟随 OS）。
export const AppTheme& currentTheme() {
    return g_dark ? kDarkTheme : kLightTheme;
}
