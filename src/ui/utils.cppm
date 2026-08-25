// ui/utils.cppm — UI 布局常量（设计逻辑像素）+ 缩放系数 kUI +
// 精确文本截断（用 eui 真实字体度量）。
// 纯 string/number 帮助函数已下移到 tinynext.utils（src/utils.cppm），这里
// export import 转发，既有 UI 模块继续只 import tinynext.ui.utils 即可。
module;

#include "eui_ui.h"   // core::TextPrimitive::measureTextWidth（真实字体测宽，截断精确）

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

// ---- 按钮设计令牌（设计逻辑像素）----
// 宽度继续由文案和具体场景决定；高度、字号、间距和图标尺寸集中管理，
// 让页面级操作、弹窗操作、工具栏和卡片按钮保持同一套视觉节奏。
export constexpr float kButtonHeight = 26.0f;
export constexpr float kButtonFontSize = 12.0f;
export constexpr float kCompactButtonHeight = 24.0f;
export constexpr float kCompactButtonFontSize = 11.0f;
export constexpr float kDialogButtonHeight = 28.0f;
export constexpr float kDialogButtonFontSize = 12.0f;
export constexpr float kButtonGap = 8.0f;
export constexpr float kToolbarButtonSize = 26.0f;
export constexpr float kToolbarIconSize = 13.0f;
export constexpr float kCardActionSize = 22.0f;
export constexpr float kCardActionIconSize = 11.0f;
export constexpr float kStepperButtonSize = 20.0f;
export constexpr float kStepperIconSize = 9.0f;

// ---- 滚动条设计令牌（设计逻辑像素）----
// 所有 TinyNext scrollView 显式使用这组值，避免依赖 EUI 默认的 8px 宽度
// 和 18px 间距；uiScale(kUI) 会负责原生缩放。
export constexpr float kScrollbarWidth = 4.0f;
export constexpr float kScrollbarGap = 6.0f;

export constexpr float kRailWidth = 40.0f;          // 大侧边栏（图标栏）宽
export constexpr float kSubSidebarWidth = 96.0f;    // 下载页内任务列表子侧边栏宽
// 岛屿卡片布局：外层"岛"卡片之间的间距 / 大卡内边距 / 岛卡片圆角。
export constexpr float kIslandGap = 2.0f;
export constexpr float kIslandVInset = 6.0f;   // 岛卡距窗口上/下的空隙（卡片感）
export constexpr float kPanelPad = 10.0f;
export constexpr float kIslandRadius = 10.0f;

namespace {
// s 里的码点数（UTF-8）。
std::size_t utf8CodepointCount(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int seq = 1;
        if ((c & 0x80) == 0) seq = 1;
        else if ((c & 0xE0) == 0xC0) seq = 2;
        else if ((c & 0xF0) == 0xE0) seq = 3;
        else if ((c & 0xF8) == 0xF0) seq = 4;
        i += seq;
        ++n;
    }
    return n;
}
// 取前 k 个码点（不切断多字节字符）。
std::string takeCodepoints(const std::string& s, std::size_t k) {
    std::size_t byteOff = 0, count = 0;
    while (byteOff < s.size() && count < k) {
        const unsigned char c = static_cast<unsigned char>(s[byteOff]);
        int seq = 1;
        if ((c & 0x80) == 0) seq = 1;
        else if ((c & 0xE0) == 0xC0) seq = 2;
        else if ((c & 0xF0) == 0xE0) seq = 3;
        else if ((c & 0xF8) == 0xF0) seq = 4;
        byteOff += seq;
        ++count;
    }
    return s.substr(0, byteOff);
}
} // namespace

// ---- 精确单行截断 ----
// 用 eui 的真实字体度量（core::TextPrimitive::measureTextWidth）算宽，超出可用宽度
// 就截断并补省略号 "…"。相比按字号比例猜宽，交给字体自身度量 → 与渲染完全一致，
// 不存在估算偏差（中英混排也正确）。fontFamily/fontWeight 与文本一致（默认应用字体）。
// 返回完整字符串当放得下，否则截断。
export std::string ellipsizeText(const std::string& s, float widthPx, float fontSize,
                                 const std::string& fontFamily = {}, int fontWeight = 400) {
    if (s.empty() || widthPx <= 0.0f) return s;
    const float fullW = core::TextPrimitive::measureTextWidth(s, fontFamily, fontSize, fontWeight);
    if (fullW <= widthPx) return s;
    const float dotW = core::TextPrimitive::measureTextWidth("…", fontFamily, fontSize, fontWeight);
    const std::size_t total = utf8CodepointCount(s);

    // 二分：在 [0, total] 里找最大的 k，使得 前 k 个码点 + "…" 的宽 ≤ widthPx。
    auto widthOf = [&](std::size_t k) {
        return core::TextPrimitive::measureTextWidth(takeCodepoints(s, k), fontFamily, fontSize, fontWeight);
    };
    std::size_t lo = 0, hi = total;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo + 1) / 2;
        if (widthOf(mid) + dotW <= widthPx) lo = mid;
        else hi = mid - 1;
    }
    // lo 个码点放得下；若连 1 个都放不下，只留省略号。
    return lo == 0 ? "…" : takeCodepoints(s, lo) + "…";
}
