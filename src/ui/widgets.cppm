// ui/widgets.cppm — reusable UI controls: the generic up/down list picker,
// sidebar / rail list items, and the small icon card-action button.
module;

#include "eui_ui.h"

export module tinynext.ui.widgets;

import std;
import tinynext.ui.theme;
import tinynext.ui.utils;

// ----------------------------------------------------- 外层"岛"卡片背景 --
// 岛屿卡片风：内容区 / 侧边栏都做成浮在页面背景上的圆角"岛"。底色取
// background↔surface 的中间色调，保证与纯 surface 的内层卡片（如任务卡）分层，
// 不会糊在一起；统一圆角 + 细边框 + 柔和投影。
// 毛玻璃填充色：surface 基色半透明，与背后被模糊的背景混合成玻璃质感。
// alpha 越高越实、越低越透。drawPanel（整页岛卡）与各弹窗 / 任务卡 / 翻页 /
// 下拉弹层复用同一套玻璃色。
export core::Color glassFill(const AppTheme& theme, float alpha = 0.6f) {
    const core::Color base = core::mixColor(theme.components.background,
                                            theme.components.surface, 0.5f);
    return {base.r, base.g, base.b, alpha};
}

export void drawPanel(eui::Ui& ui, const std::string& id, float x, float y,
                      float w, float h, const AppTheme& theme) {
    const auto& tokens = theme.components;
    // 毛玻璃面板：blur(n) 让 eui 捕获 rect 背后画面并模糊（backdrop blur，
    // shader mix(blurred, fill.rgb, fill.a)），半透明 fill 与模糊背景混合成玻璃质感。
    // 背景须有渐变/内容变化才有可模糊的细节（见 app.cpp 的 theme.background 渐变）。
    const core::Color shadowColor =
        tokens.dark ? core::Color{0.0f, 0.0f, 0.0f, 0.25f}
                    : core::Color{0.10f, 0.14f, 0.22f, 0.12f};
    ui.rect(id)
        .position(x, y)
        .size(w, h)
        .blur(18.0f)  // 官方 demo 的玻璃强度（gallery_controls.h 用 18）
        .color(glassFill(theme, 0.55f))
        .radius(kIslandRadius)
        .border(1.0f, components::theme::withOpacity(tokens.border, 0.6f))
        .shadow(14.0f, 3.0f, shadowColor)
        .build();
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

// 工具栏图标按钮：非主按钮默认无描边，hover 时浮现细边框 + 轻微底色（利用
// ui.state 持久存 hover，每帧读它切边框颜色，transition 过渡）；primary 时一直
// 主色填充（保持默认描边，如 ➕ 添加按钮）。
export void drawToolbarIconButton(eui::Ui& ui, const std::string& id, float x, float y,
                                  float w, float h, unsigned int icon, bool primary,
                                  const AppTheme& theme, std::function<void()> onClick) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
    // 完美正圆：半径 = 短边一半（调用方传正方形尺寸即可）。
    const float radius = std::min(w, h) * 0.5f;

    if (primary) {
        components::button(ui, id)
            .position(x, y)
            .size(w, h)
            .icon(icon)
            .text("")
            .iconSize(13.0f)
            .theme(tokens, true)
            .radius(radius)
            .onClick(std::move(onClick))
            .build();
        return;
    }

    bool& hovered = ui.state<bool>(id + ".hover");
    const core::Color borderColor = components::theme::withOpacity(tokens.border, 0.9f);
    ui.rect(id + ".fill")
        .position(x, y)
        .size(w, h)
        .states(transparent, tokens.surfaceHover, tokens.surfaceActive)
        .radius(radius)
        .border(1.0f, hovered ? borderColor : transparent)
        .transition(transition)
        .onHover([&hovered](bool h) { hovered = h; })
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
                // 纯文本字段：无边框条，当前项文字居中 + 右侧小箭头（无尾 chevron），
                // 点击弹出列表。默认透明，仅 hover/按下给出轻微底色反馈。
                ui.rect(id + ".hit")
                    .size(width, height)
                    .states({0.0f, 0.0f, 0.0f, 0.0f}, tokens.surfaceHover,
                            tokens.surfaceActive)
                    .radius(6.0f)
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
                ui.rect(id + ".field")
                    .size(width, height)
                    .color(tokens.surface)
                    .radius(8.0f)
                    .border(1.0f, components::theme::withOpacity(tokens.border, 0.78f))
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
                        ui.rect(id + ".popup.bg")
                            .size(popWidth, popupHeight)
                            .blur(12.0f)
                            .color(glassFill(theme, 0.7f))
                            .radius(8.0f)
                            .border(1.0f, components::theme::withOpacity(tokens.border, 0.78f))
                            .onClick([] {})  // 吞掉弹层内部空白点击，避免穿透到遮罩关闭弹窗
                            .build();

                        for (int i = 0; i < count; ++i) {
                            const float itemY = popupPad + i * itemHeight;
                            ui.rect(id + ".item." + std::to_string(i))
                                .x(popupPad)
                                .y(itemY)
                                .size(popWidth - popupPad * 2.0f, itemHeight)
                                .states({0.0f, 0.0f, 0.0f, 0.0f}, tokens.surfaceHover,
                                        tokens.surfaceActive)
                                .radius(5.0f)
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
                                .color(i == selected ? tokens.primary : tokens.text)
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
    const float btnSize = std::min(20.0f, height);
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
        .iconSize(9.0f)
        .theme(tokens, false)
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
        .iconSize(9.0f)
        .theme(tokens, false)
        .onClick([value, onChange, min, max, step] {
            int cur = 0;
            try { cur = std::stoi(trimText(value)); } catch (...) {}
            onChange(std::to_string(std::clamp(cur + step, min, max)));
        })
        .build();
}

// 侧边栏列表项（普通列表）：图标 + 文字，激活时主色高亮 + 左侧竖条。
// 用于页面导航（下载列表 / 设置）和下载状态筛选（所有 / 下载中 / 已完成）。
// count >= 0 时在右侧显示数量徽标（如筛选项的任务数），文字区相应让位。
export void drawSidebarItem(eui::Ui& ui, const std::string& id, float x, float y,
                            float width, float height, const std::string& label,
                            unsigned int icon, bool active, const AppTheme& theme,
                            std::function<void()> onClick, int count = -1) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color idle = {0.0f, 0.0f, 0.0f, 0.0f};
    const core::Color activeFill =
        components::theme::withAlpha(tokens.primary, theme.dark ? 0.22f : 0.14f);
    const core::Color textColor = active ? tokens.primary : tokens.text;

    // 激活高亮底色（始终存在，透明即隐藏，避免创建/移除图层）。
    ui.rect(id + ".bg")
        .position(x, y)
        .size(width, height)
        .color(active ? activeFill : idle)
        .radius(8.0f)
        .transition(transition)
        .build();

    if (active) {
        ui.rect(id + ".bar")
            .position(x, y + 6.0f)
            .size(3.0f, height - 12.0f)
            .color(tokens.primary)
            .radius(1.5f)
            .build();
    }

    // 点击命中区（悬停反馈；激活项不再叠加 hover 底色）。
    ui.rect(id + ".hit")
        .position(x, y)
        .size(width, height)
        .states(idle, active ? idle : tokens.surfaceHover, tokens.surfaceActive)
        .radius(8.0f)
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
    // 浅底 + 主色数字，其余次要文本色。
    if (count >= 0) {
        const std::string text = std::to_string(count);
        const float bubbleW = 12.0f + static_cast<float>(text.size()) * 6.0f;
        const float bubbleH = 14.0f;
        const float bubbleX = x + width - bubbleW - 6.0f;
        const float bubbleY = y + (height - bubbleH) * 0.5f;
        const core::Color bubbleFill =
            active ? components::theme::withAlpha(tokens.primary,
                                                  theme.dark ? 0.26f : 0.14f)
                   : tokens.surfaceHover;
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
            .color(active ? tokens.primary : theme.metaText)
            .horizontalAlign(core::HorizontalAlign::Center)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
    }
}

// 主侧边栏（图标栏）列表项：仅图标、无文字，激活时主色高亮 + 左侧竖条。
export void drawRailItem(eui::Ui& ui, const std::string& id, float y, float railWidth,
                         unsigned int icon, bool active, const AppTheme& theme,
                         std::function<void()> onClick) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color idle = {0.0f, 0.0f, 0.0f, 0.0f};
    const core::Color activeFill =
        components::theme::withAlpha(tokens.primary, theme.dark ? 0.22f : 0.14f);
    const core::Color iconColor = active ? tokens.primary : tokens.text;
    const float itemW = railWidth - 8.0f;   // 两侧各留 4px
    const float x = (railWidth - itemW) * 0.5f;

    // 激活高亮底色。
    ui.rect(id + ".bg")
        .position(x, y)
        .size(itemW, 24.0f)
        .color(active ? activeFill : idle)
        .radius(7.0f)
        .transition(transition)
        .build();

    if (active) {
        ui.rect(id + ".bar")
            .position(0, y + 4.0f)
            .size(2.0f, 16.0f)
            .color(tokens.primary)
            .radius(1.0f)
            .build();
    }

    // 点击命中区（悬停反馈）。
    ui.rect(id + ".hit")
        .position(x, y)
        .size(itemW, 24.0f)
        .states(idle, active ? idle : tokens.surfaceHover, tokens.surfaceActive)
        .radius(7.0f)
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    // 图标（水平居中）。
    ui.text(id + ".icon")
        .position(0, y)
        .size(railWidth, 24.0f)
        .icon(icon)
        .fontSize(13.0f)
        .lineHeight(24.0f)
        .color(iconColor)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}

// 卡片内的小图标操作按钮：纯图标、无文字，hover/按下反馈走组件默认状态。
export void drawCardAction(eui::Ui& ui, const std::string& id, float x, float y,
                           unsigned int icon, bool primary, const AppTheme& theme,
                           std::function<void()> onClick) {
    components::button(ui, id)
        .position(x, y)
        .size(kCardIconW, kCardIconW)
        .icon(icon)
        .text("")
        .iconSize(11.0f)
        .theme(theme.components, primary)
        .onClick(std::move(onClick))
        .build();
}
