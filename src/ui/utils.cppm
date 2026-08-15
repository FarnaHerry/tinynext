// ui/utils.cppm — UI 布局常量（设计逻辑像素）+ 缩放系数 kUI。
// 纯 string/number 帮助函数已下移到 tinynext.utils（src/utils.cppm），这里
// export import 转发，既有 UI 模块继续只 import tinynext.ui.utils 即可。
export module tinynext.ui.utils;

import std;

export import tinynext.utils;

// ---- UI 缩放 ----
// eui-neo 0.5.6 起 DslAppConfig::uiScale(kUI) 原生放大整个逻辑坐标系（布局+字号），
// 本项目不再自乘系数。所有尺寸按“设计逻辑像素”直接书写，由 eui 按
// dpiScale*uiScale 统一放大；kUI 仍是唯一的缩放旋钮（传给 uiScale + 决定窗口物理尺寸）。
export constexpr float kUI = 1.4f;

// 布局尺寸都按“设计逻辑像素”书写，并尽量用 screen.width/height 推算，
// 随窗口缩放自适应。
// 当前仅卡片的右边缘与窗口边之间留 kRightMargin（图标栏占满左缘、卡片顶/底贴齐）。
export constexpr float kRightMargin = 6.0f;
export constexpr float kInputHeight = 26.0f;
export constexpr float kPagerHeight = 24.0f;        // 翻页行高
export constexpr float kCardHeight = 68.0f;         // 卡片高
export constexpr float kCardPad = 10.0f;
export constexpr float kCardGap = 6.0f;
export constexpr float kCardIconW = 22.0f;          // 卡片操作图标按钮边长
export constexpr float kCardIconGap = 4.0f;
export constexpr float kRailWidth = 40.0f;          // 大侧边栏（图标栏）宽
export constexpr float kSubSidebarWidth = 96.0f;    // 下载页内任务列表子侧边栏宽
// 岛屿卡片布局：外层"岛"卡片之间的间距 / 大卡内边距 / 岛卡片圆角。
export constexpr float kIslandGap = 2.0f;
export constexpr float kIslandVInset = 6.0f;   // 岛卡距窗口上/下的空隙（卡片感）
export constexpr float kPanelPad = 10.0f;
export constexpr float kIslandRadius = 10.0f;
