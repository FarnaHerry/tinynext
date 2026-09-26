// ui/widgets.cppm — reusable UI controls: the generic up/down list picker,
// sidebar / rail list items, and the small icon card-action button.
module;

#include "eui_ui.h"

// 悬浮气泡/点击暂停等需要 UI 重绘一帧（纯状态变化不走 eui 内建 re-compose）。
namespace core::platform { void requestUiUpdate(); }

export module tinynext.ui.widgets;

import std;
import tinynext.ui.theme;
import tinynext.ui.utils;

namespace {
// ---- 悬浮提示（tooltip）状态与延迟逻辑 ----
// 用模块级 atomic 状态按 id 存放：eui 的 ui.state 不是线程安全的，延迟线程只碰
// 这里（cross-thread race-free）。hover 进入后延迟 hoverDelayMs 才置 shown，让
// 气泡「等一小会」再出现，避免鼠标一碰就弹。
struct TipState {
    std::atomic<bool> hovered{false};
    std::atomic<bool> shown{false};
};
std::mutex g_tipMutex;
std::unordered_map<std::string, TipState> g_tips;

TipState& tipState(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_tipMutex);
    return g_tips[id];
}

// .onHover 回调：进入/离开 hover 时更新状态并在延迟后显示气泡。h 恒为当前 hover。
void onTipHover(const std::string& id, bool h) {
    TipState& s = tipState(id);
    const bool prev = s.hovered.exchange(h);
    if (h == prev) return;
    if (!h) {
        s.shown.store(false);
        core::platform::requestUiUpdate();
        return;
    }
    // 进入 hover：spawn 一个延迟线程，400ms 后若仍 hovered 才显示气泡。
    std::thread([id] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        TipState& st = tipState(id);
        if (st.hovered.load()) {
            st.shown.store(true);
            core::platform::requestUiUpdate();
        }
    }).detach();
}

// 画气泡 + 尾巴。气泡在图标栏右侧，尾巴是向左的小三角形（用 polygon，局部坐标，
// 相对 .position 的左上角），指向图标栏上的按钮。btnY/btnH 是按钮位置尺寸，
// railWidth 是图标栏宽。
void drawTipBubble(eui::Ui& ui, const std::string& id, float btnY, float btnH,
                   float railWidth, const std::string& text, const AppTheme& theme) {
    const TipState& s = tipState(id);
    if (!s.shown.load() || text.empty()) return;
    const float tipH = 20.0f;
    const float tipInnerPad = 8.0f;
    const float tailW = 7.0f;                        // 尾巴宽度（连接条）
    const float tipW = std::max(
        core::TextPrimitive::measureTextWidth(text, "", 11.0f) + tipInnerPad * 2.0f, 22.0f);
    const float tipX = railWidth + tailW + 2.0f;   // 气泡左缘
    const float tipY = btnY + (btnH - tipH) * 0.5f;  // 气泡垂直居中于按钮
    // M3 tooltip：反色面（深色主题亮灰底深字 / 浅色主题深底浅字），文字随 onInverseSurface。
    const core::Color tipBg = theme.inverseSurface;
    const core::Color tipText = theme.onInverseSurface;

    // 气泡主体（绝对定位的圆角矩形）。**不加边框**：边框会在尾巴与气泡相接处切出竖线，
    // 造成「割裂」感。改用低透明度投影提供层次（分离感由阴影承担），尾巴与气泡同色重叠
    // → 视觉上气泡直接延伸出小箭头。
    ui.stack(id + ".tip")
        .position(tipX, tipY)
        .size(tipW, tipH)
        .zIndex(60)
        .content([&] {
            ui.rect(id + ".tip.bg")
                .size(tipW, tipH)
                .color(tipBg)
                .radius(8.0f)
                .shadow(6.0f, 1.5f, core::Color{0.0f, 0.0f, 0.0f, 0.18f})
                .build();
            ui.text(id + ".tip.label")
                .size(tipW, tipH)
                .text(text)
                .fontSize(11.0f)
                .lineHeight(tipH)
                .color(tipText)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center)
                .build();
        })
        .build();

    // 尾巴：向左的圆角小三角（polygon，局部坐标相对 .position 左上角）。尖端贴图标栏
    // 右缘、垂直居中于按钮；底边**伸进气泡内部 tailOverlap 深**，与气泡同色重叠盖住
    // 接缝 → 无割裂，气泡直接延伸出箭头（polygon 的 .radius 就是 eui 版的「伪元素
    // 圆角」，效果等同 CSS 气泡 ::after）。zIndex 盖在气泡主体之上。
    const float tailH = 16.0f;                              // 三角形高（垂直居中于按钮）
    const float tailOverlap = 3.0f;                         // 底边伸进气泡的深度（藏接缝）
    const float tailGap = std::max(0.0f, tipX - railWidth) + tailOverlap;
    const float tailX = railWidth;                          // 尖端贴图标栏右缘
    const float tailY = btnY + (btnH - tailH) * 0.5f;       // 垂直居中于按钮
    ui.polygon(id + ".tip.tail")
        .position(tailX, tailY)
        .size(tailGap, tailH)
        .point(0.0f, tailH * 0.5f)     // 尖端：左，垂直居中
        .point(tailGap, 0.0f)          // 底边右上（伸进气泡）
        .point(tailGap, tailH)         // 底边右下
        .radius(3.0f)                  // 圆角三个顶点
        .color(tipBg)
        .zIndex(61)
        .build();
}
} // namespace
// ----------------------------------------------------- 外层"岛"卡片背景 --
// M3 页面层：岛卡 = 不透明 surface 大圆角平面（28dp 体系下的 16），无描边无投影 ——
// 底色 = panelBg（暗色 surfaceContainerLow / 亮色纯白），层级靠表面分层表达，不再用玻璃拟态/阴影。
export void drawPanel(eui::Ui& ui, const std::string& id, float x, float y,
                      float w, float h, const AppTheme& theme) {
    ui.rect(id)
        .position(x, y)
        .size(w, h)
        .color(theme.panelBg)
        .radius(kIslandRadius)
        .build();
}

// 按钮文字/图标色：primary 按钮文字 = onPrimary（M3 角色，亮暗各定义）；
// 非 primary 走组件默认（text = onSurface）。玻璃拟态时代的黑白翻色已随 M3 色板退役。
export core::Color onPrimaryColor(const AppTheme& theme, bool primary = true) {
    return primary ? theme.onPrimary : theme.components.text;
}

// 单岛布局里的竖向分隔线：同一张岛卡内，把二级侧边栏与内容区区分开（整页一张岛、
// 不再各自成卡，用竖线分隔）。细 1px 竖线，从岛卡顶边划到底边（仅留 2px 不压到
// 上下边框线），让两侧有明确的纵向分隔感；线在侧边栏/内容交界处、远离圆角，无需内缩。
export void drawVDivider(eui::Ui& ui, const std::string& id, float x, float y,
                         float h, const AppTheme& theme) {
    constexpr float kInset = 2.0f;
    ui.rect(id)
        .position(x, y + kInset)
        .size(1.0f, std::max(0.0f, h - 2.0f * kInset))
        .color(components::theme::withOpacity(theme.components.border, 0.55f))
        .build();
}

// 工具栏图标按钮（M3 Icon Button）：圆形，无描边；standard 款 hover/pressed 叠
// onSurface state layer（8%/12%），selected 款（primary=true）主色填充 + onPrimary 图标。
// fab=true 时加 M3 FAB 投影（主行动强调，如「添加下载」）。
export void drawToolbarIconButton(eui::Ui& ui, const std::string& id, float x, float y,
                                  float w, float h, unsigned int icon, bool primary,
                                  const AppTheme& theme, std::function<void()> onClick,
                                  bool fab = false) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
    const float radius = std::min(w, h) * 0.5f;

    if (primary) {
        components::button(ui, id)
            .position(x, y)
            .size(w, h)
            .icon(icon)
            .text("")
            .iconSize(kToolbarIconSize)
            .theme(tokens, true)
            .iconColor(onPrimaryColor(theme))  // 主色底 → onPrimary 图标
            .shadow(fab ? 12.0f : 0.0f, 0.0f, fab ? 4.0f : 0.0f,
                    fab ? (theme.dark ? core::Color{0.0f, 0.0f, 0.0f, 0.35f}
                                      : core::Color{0.10f, 0.14f, 0.22f, 0.22f})
                        : core::Color{0.0f, 0.0f, 0.0f, 0.0f})
            .radius(radius)
            .onClick(std::move(onClick))
            .build();
        return;
    }

    // standard 款：透明底 + onSurface state layer，选中态（调用方改 icon 色）用主色。
    ui.rect(id + ".fill")
        .position(x, y)
        .size(w, h)
        .states(transparent, stateLayer(theme.onSurface, 0.08f),
                stateLayer(theme.onSurface, 0.12f))
        .radius(radius)
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    ui.text(id + ".icon")
        .position(x, y)
        .size(w, h)
        .icon(icon)
        .fontSize(13.0f)
        .lineHeight(h)
        .color(tokens.text)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}

// ----------------------------------------------------- 通用上下拉列表选择器 --
//
// eui 的 components::dropdown 只会向下弹出，放在底部翻页行时弹层会超出窗口下缘。
// 这里做一个通用选择器：字段（文字显示当前项，或纯图标 fa-sort）+ 向上/向下
// 展开的 popup，样式取自当前主题 tokens。分页大小（向上）与排序（向下）共用。
export enum class PickerField { Text, Icon, Plain };

export void buildListPicker(eui::Ui& ui, const std::string& id, float width, float height,
                            const AppTheme& theme, bool& open, const char* const* labels,
                            int count, int selected, bool opensUp, PickerField field,
                            const std::function<void(int)>& onPick,
                            float popupWidth = 0.0f) {
    const float itemHeight = 22.0f;
    const float popupPad = 3.0f;
    const float popupGap = 3.0f;
    // 弹层宽度：默认与字段同宽；字段是纯图标（如排序）时可传入更宽的值容纳文字。
    const float popWidth = popupWidth > 0.0f ? popupWidth : width;
    const float popupHeight = itemHeight * count + popupPad * 2.0f;
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);

    ui.stack(id)
        .size(width, height)
        .zIndex(30)
        .content([&] {
            // ---- 字段（点击切换展开/收起）：文字显示当前项，或纯图标 ----
            if (field == PickerField::Icon) {
                // 图标字段（如排序）：默认无描边，hover 才浮现。
                drawToolbarIconButton(ui, id + ".btn", 0, 0, width, height,
                                      0xF0DC, false, theme,
                                      [&open] { open = !open; });
            } else if (field == PickerField::Plain) {
                // 纯文本字段（翻页器用）：无边框条，当前项文字居中 + 右侧小箭头（无尾 chevron），
                // 点击弹出列表。M3 text-button 风：透明底 + onSurface state layer。
                ui.rect(id + ".hit")
                    .size(width, height)
                    .states({0.0f, 0.0f, 0.0f, 0.0f}, stateLayer(theme.onSurface, 0.08f),
                            stateLayer(theme.onSurface, 0.12f))
                    .radius(kChipRadius)
                    .onClick([&open] { open = !open; })
                    .build();
                ui.text(id + ".label")
                    .x(-4.0f)  // 给右侧箭头让位，视觉上仍居中
                    .size(width - 10.0f, height)
                    .text(labels[selected])
                    .fontSize(11.0f)
                    .lineHeight(height)
                    .color(tokens.text)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();
                ui.text(id + ".chevron")
                    .x(width - 15.0f)
                    .size(12.0f, height)
                    .icon(open ? 0xF077 : 0xF078)  // chevron-up / chevron-down
                    .fontSize(9.0f)
                    .lineHeight(height)
                    .color(tokens.primary)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();
            } else {
                // M3 outlined 字段：surface 底 + outline 描边，选中/展开时描边转主色。
                ui.rect(id + ".field")
                    .size(width, height)
                    .color(tokens.surface)
                    .radius(kChipRadius)
                    .border(1.0f, open ? theme.primary
                                       : components::theme::withOpacity(theme.outline, 0.7f))
                    .transition(transition)
                    .onClick([&open] { open = !open; })
                    .build();

                ui.text(id + ".label")
                    .x(9.0f)
                    .size(width - 30.0f, height)
                    .text(labels[selected])
                    .fontSize(11.0f)
                    .lineHeight(height)
                    .color(tokens.text)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();

                ui.text(id + ".chevron")
                    .x(width - 20.0f)
                    .size(14.0f, height)
                    .icon(open ? 0xF077 : 0xF078)  // chevron-up / chevron-down
                    .fontSize(10.0f)
                    .lineHeight(height)
                    .color(tokens.primary)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();
            }

            // ---- 弹出列表（向上或向下展开）----
            if (open) {
                // 全屏透明拦截层（在弹层之下）：点击弹层外任意处收起，并吞掉点击
                // 防止穿透到弹窗遮罩/其他控件。尺寸放大覆盖任意窗口。
                ui.rect(id + ".dismiss")
                    .position(-2000.0f, -2000.0f)
                    .size(5000.0f, 5000.0f)
                    .states({0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f},
                            {0.0f, 0.0f, 0.0f, 0.0f})
                    .onClick([&open] { open = false; })
                    .onScroll([](const core::ScrollEvent&) {})
                    .build();

                ui.stack(id + ".popup")
                    .x(popWidth > width ? width - popWidth : 0.0f)  // 比字段宽时向右边缘对齐
                    .y(opensUp ? -(popupHeight + popupGap) : height + popupGap)
                    .size(popWidth, popupHeight)
                    .zIndex(31)
                    .content([&] {
                        // M3 menu：不透明 surfaceContainerHigh + 发丝 outlineVariant 描边。
                        ui.rect(id + ".popup.bg")
                            .size(popWidth, popupHeight)
                            .color(theme.surfaceContainerHigh)
                            .radius(kChipRadius)
                            .border(1.0f,
                                    components::theme::withOpacity(theme.outlineVariant, 0.6f))
                            .shadow(8.0f, 2.0f,
                                    theme.dark ? core::Color{0.0f, 0.0f, 0.0f, 0.30f}
                                               : core::Color{0.10f, 0.14f, 0.22f, 0.14f})
                            .onClick([] {})  // 吞掉弹层内部空白点击，避免穿透到遮罩关闭弹窗
                            .build();

                        for (int i = 0; i < count; ++i) {
                            const float itemY = popupPad + i * itemHeight;
                            const bool itemSelected = i == selected;
                            ui.rect(id + ".item." + std::to_string(i))
                                .x(popupPad)
                                .y(itemY)
                                .size(popWidth - popupPad * 2.0f, itemHeight)
                                .states(itemSelected ? theme.primaryContainer
                                                     : core::Color{0.0f, 0.0f, 0.0f, 0.0f},
                                        stateLayer(theme.onSurface, 0.08f),
                                        stateLayer(theme.onSurface, 0.12f))
                                .radius(6.0f)
                                .onClick([&open, i, onPick] {
                                    open = false;
                                    onPick(i);
                                })
                                .build();

                            ui.text(id + ".item.label." + std::to_string(i))
                                .x(popupPad + 8.0f)
                                .y(itemY)
                                .size(popWidth - popupPad * 2.0f - 16.0f, itemHeight)
                                .text(labels[i])
                                .fontSize(11.0f)
                                .lineHeight(itemHeight)
                                .color(itemSelected ? theme.onPrimaryContainer : tokens.text)
                                .verticalAlign(core::VerticalAlign::Center)
                                .build();
                        }
                    })
                    .build();
            }
        })
        .build();
}

// 数字步进输入：文本输入 + 内嵌 -/+ 按钮。value 是当前文本（可手输数字；空/非法
// 按 0），加减基于解析出的整数，夹到 [min,max]、步长 step，改完写回并回调。
// 布局：[-] [输入] [+]。
export void buildNumberStepper(eui::Ui& ui, const std::string& id, float x, float y,
                               float width, float height, const AppTheme& theme,
                               const std::string& value,
                               const std::function<void(const std::string&)>& onChange,
                               int min, int max, int step) {
    // -/+ 按钮做成正方形 → 纯圆（radius = 边长/2），垂直居中于输入框高度。
    const float btnSize = std::min(kStepperButtonSize, height);
    const float btnY = y + (height - btnSize) * 0.5f;
    const float gap = 3.0f;
    const float inputW = width - btnSize * 2.0f - gap * 2.0f;
    const auto& tokens = theme.components;

    components::button(ui, id + ".minus")
        .position(x, btnY)
        .size(btnSize, btnSize)
        .radius(btnSize * 0.5f)
        .icon(0xF068)  // fa-minus
        .text("")
        .iconSize(kStepperIconSize)
        .theme(tokens, false)
        .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([value, onChange, min, max, step] {
            int cur = 0;
            try { cur = std::stoi(trimText(value)); } catch (...) {}
            onChange(std::to_string(std::clamp(cur - step, min, max)));
        })
        .build();
    components::input(ui, id + ".input")
        .position(x + btnSize + gap, y)
        .size(inputW, height)
        .value(value)
        .fontFamily("")  // 用应用字体（Noto Sans SC），不要 eui 默认的 Microsoft YaHei
        .theme(tokens)
        .onChange([onChange](const std::string& v) {
            // 只保留数字：手输字母/符号会被滤掉（eui input 每帧用 value() 覆盖
            // 内部文本，写回纯数字状态后显示即同步）。范围校验在保存层。
            std::string digits;
            for (char c : v) {
                if (c >= '0' && c <= '9') digits += c;
            }
            onChange(digits);
        })
        .build();
    components::button(ui, id + ".plus")
        .position(x + btnSize + gap + inputW + gap, btnY)
        .size(btnSize, btnSize)
        .radius(btnSize * 0.5f)
        .icon(0xF067)  // fa-plus
        .text("")
        .iconSize(kStepperIconSize)
        .theme(tokens, false)
        .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([value, onChange, min, max, step] {
            int cur = 0;
            try { cur = std::stoi(trimText(value)); } catch (...) {}
            onChange(std::to_string(std::clamp(cur + step, min, max)));
        })
        .build();
}

// 侧边栏列表项（M3 Navigation Drawer item）：图标 + 文字，激活指示为全宽
// secondaryContainer pill（无左侧竖条），激活图标/文字 = onSecondaryContainer；
// 非激活 = onSurfaceVariant，hover 叠 onSurface state layer（激活项不叠）。
// count >= 0 时在右侧显示数量徽标，文字区相应让位。
export void drawSidebarItem(eui::Ui& ui, const std::string& id, float x, float y,
                            float width, float height, const std::string& label,
                            unsigned int icon, bool active, const AppTheme& theme,
                            std::function<void()> onClick, int count = -1) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color idle = {0.0f, 0.0f, 0.0f, 0.0f};
    const core::Color activeFill = theme.secondaryContainer;
    const core::Color textColor =
        active ? theme.onSecondaryContainer : theme.onSurfaceVariant;

    // 激活指示 pill（始终存在，透明即隐藏，避免创建/移除图层）。
    ui.rect(id + ".bg")
        .position(x, y)
        .size(width, height)
        .color(active ? activeFill : idle)
        .radius(kButtonRadius)
        .transition(transition)
        .build();

    // 点击命中区（悬停反馈；激活项不再叠加 hover 底色）。
    ui.rect(id + ".hit")
        .position(x, y)
        .size(width, height)
        .states(idle, active ? idle : stateLayer(theme.onSurface, 0.08f),
                stateLayer(theme.onSurface, 0.12f))
        .radius(kButtonRadius)
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    // 图标。
    ui.text(id + ".icon")
        .position(x + 8.0f, y)
        .size(16.0f, height)
        .icon(icon)
        .fontSize(11.0f)
        .lineHeight(height)
        .color(textColor)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // 文字（有数量徽标时让出右侧空间，宽度与下方气泡一致 + 间隙）。
    const float countW = count >= 0
        ? 12.0f + static_cast<float>(std::to_string(count).size()) * 6.0f + 10.0f
        : 0.0f;
    ui.text(id + ".label")
        .position(x + 30.0f, y)
        .size(width - 36.0f - countW, height)
        .text(label)
        .fontSize(12.0f)
        .lineHeight(height)
        .color(textColor)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // 右侧数量徽标：小气泡（圆角 pill 底 + 数字），宽度随位数自适应；激活项主色
    // 浅底 + 主色数字，其余 surfaceContainerHighest + onSurfaceVariant。
    if (count >= 0) {
        const std::string text = std::to_string(count);
        const float bubbleW = 12.0f + static_cast<float>(text.size()) * 6.0f;
        const float bubbleH = 14.0f;
        const float bubbleX = x + width - bubbleW - 6.0f;
        const float bubbleY = y + (height - bubbleH) * 0.5f;
        const core::Color bubbleFill =
            active ? stateLayer(theme.primary, theme.dark ? 0.16f : 0.12f)
                   : theme.surfaceContainerHighest;
        ui.rect(id + ".count.bg")
            .position(bubbleX, bubbleY)
            .size(bubbleW, bubbleH)
            .color(bubbleFill)
            .radius(bubbleH * 0.5f)
            .build();
        ui.text(id + ".count")
            .position(bubbleX, bubbleY)
            .size(bubbleW, bubbleH)
            .text(text)
            .fontSize(10.0f)
            .lineHeight(bubbleH)
            .color(active ? theme.primary : theme.onSurfaceVariant)
            .horizontalAlign(core::HorizontalAlign::Center)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
    }
}

// M3 Navigation Rail 项：块高 kNavItemH（56）= 32 高激活 pill（图标居中）
// + 10px 标签。激活 = secondaryContainer pill + onSecondaryContainer 图标/标签；
// 非激活 = onSurfaceVariant；hover 叠 onSurface state layer。tooltip 延迟气泡可选。
export void drawRailItem(eui::Ui& ui, const std::string& id, float y, float railWidth,
                         unsigned int icon, const std::string& label, bool active,
                         const AppTheme& theme, std::function<void()> onClick,
                         const std::string& tooltip = {}) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color idle = {0.0f, 0.0f, 0.0f, 0.0f};
    const core::Color iconColor =
        active ? theme.onSecondaryContainer : theme.onSurfaceVariant;
    const float pillH = 32.0f;
    const float pillW = 56.0f;
    const float pillX = (railWidth - pillW) * 0.5f;

    // 激活指示 pill。
    ui.rect(id + ".bg")
        .position(pillX, y)
        .size(pillW, pillH)
        .color(active ? theme.secondaryContainer : idle)
        .radius(kButtonRadius)
        .transition(transition)
        .build();

    // 点击命中区 + 悬停状态（有 tooltip 时，hover 走延迟显示逻辑）。
    ui.rect(id + ".hit")
        .position(pillX, y)
        .size(pillW, pillH)
        .states(idle, active ? idle : stateLayer(theme.onSurface, 0.08f),
                stateLayer(theme.onSurface, 0.12f))
        .radius(kButtonRadius)
        .transition(transition)
        .onHover([id](bool h) { onTipHover(id, h); })
        .onClick(std::move(onClick))
        .build();

    // 图标（pill 内水平居中）。
    ui.text(id + ".icon")
        .position(pillX, y)
        .size(pillW, pillH)
        .icon(icon)
        .fontSize(20.0f)
        .lineHeight(pillH)
        .color(iconColor)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // 标签（pill 下方，全 rail 宽居中）。
    ui.text(id + ".label")
        .position(0, y + pillH + 2.0f)
        .size(railWidth, 12.0f)
        .text(label)
        .fontSize(10.0f)
        .lineHeight(12.0f)
        .color(iconColor)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // 悬浮提示气泡（带尾巴，延迟显示）。
    if (!tooltip.empty()) {
        drawTipBubble(ui, id, y, pillH, railWidth, tooltip, theme);
    }
}

// M3 text button：透明底 pill，文字 primary（error=true 时 error 色），
// hover/pressed 叠对应色 state layer。用于弹窗/设置页的次级操作与破坏性操作。
export void drawTextButton(eui::Ui& ui, const std::string& id, float x, float y,
                           float w, float h, const std::string& label,
                           const AppTheme& theme, std::function<void()> onClick,
                           bool error = false) {
    const core::Color c = error ? theme.error : theme.primary;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
    ui.rect(id)
        .position(x, y)
        .size(w, h)
        .states(transparent, stateLayer(c, 0.08f), stateLayer(c, 0.12f))
        .radius(kButtonRadius)
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    ui.text(id + ".label")
        .position(x, y)
        .size(w, h)
        .text(label)
        .fontSize(kButtonFontSize)
        .lineHeight(h)
        .color(c)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}

// 卡片内的小图标操作按钮（M3 icon button）：圆形透明底，hover/pressed 叠
// onSurface state layer；primary 动作图标用主色，其余 onSurfaceVariant。无投影无描边。
export void drawCardAction(eui::Ui& ui, const std::string& id, float x, float y,
                           unsigned int icon, bool primary, const AppTheme& theme,
                           std::function<void()> onClick) {
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
    ui.rect(id)
        .position(x, y)
        .size(kCardActionSize, kCardActionSize)
        .states(transparent, stateLayer(theme.onSurface, 0.08f),
                stateLayer(theme.onSurface, 0.12f))
        .radius(kCardActionSize * 0.5f)
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    ui.text(id + ".icon")
        .position(x, y)
        .size(kCardActionSize, kCardActionSize)
        .icon(icon)
        .fontSize(kCardActionIconSize + 1.0f)
        .lineHeight(kCardActionSize)
        .color(primary ? theme.primary : theme.onSurfaceVariant)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}

// 滑动开关（M3 Switch）：轨道 pill + 圆形滑块。开启 = 轨道 primary + 滑块 onPrimary；
// 关闭 = 轨道 surfaceContainerHighest + outline 描边 + 滑块 outline。点击切换。
export void buildToggleSwitch(eui::Ui& ui, const std::string& id, float x, float y,
                              float trackW, float trackH, bool on,
                              const AppTheme& theme, std::function<void(bool)> onToggle) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.16f, core::Ease::OutCubic);
    const float thumbR = trackH * 0.5f - 2.0f;   // 滑块半径（轨道内缩 2px）
    const float thumbX = on ? trackW - trackH + thumbR : thumbR;  // 滑块圆心 x
    const float thumbY = trackH * 0.5f;

    // 轨道：开启 primary 填充（无描边）、关闭 surfaceContainerHighest + outline 描边。
    ui.rect(id + ".track")
        .position(x, y)
        .size(trackW, trackH)
        .color(on ? tokens.primary : theme.surfaceContainerHighest)
        .radius(trackH * 0.5f)
        .border(1.0f, on ? core::Color{0.0f, 0.0f, 0.0f, 0.0f} : theme.outline)
        .transition(transition)
        .onClick([on, onToggle] { onToggle(!on); })
        .build();

    // 滑块：开启 = onPrimary（主色底上的反色），关闭 = outline。
    ui.rect(id + ".thumb")
        .position(x + thumbX - thumbR, y + thumbY - thumbR)
        .size(thumbR * 2.0f, thumbR * 2.0f)
        .color(on ? theme.onPrimary : theme.outline)
        .radius(thumbR)
        .build();
}
