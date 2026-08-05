// ui/widgets.cppm — reusable UI controls: the generic up/down list picker,
// sidebar / rail list items, and the small icon card-action button.
module;

#include "eui_ui.h"

export module tinynext.ui.widgets;

import std;
import tinynext.ui.theme;
import tinynext.ui.utils;

// ----------------------------------------------------- 通用上下拉列表选择器 --
//
// eui 的 components::dropdown 只会向下弹出，放在底部翻页行时弹层会超出窗口下缘。
// 这里做一个通用选择器：字段（文字显示当前项，或纯图标 fa-sort）+ 向上/向下
// 展开的 popup，样式取自当前主题 tokens。分页大小（向上）与排序（向下）共用。
export enum class PickerField { Text, Icon };

export void buildListPicker(eui::Ui& ui, const std::string& id, float width, float height,
                            const AppTheme& theme, bool& open, const char* const* labels,
                            int count, int selected, bool opensUp, PickerField field,
                            const std::function<void(int)>& onPick) {
    const float itemHeight = S(22.0f);
    const float popupPad = S(3.0f);
    const float popupGap = S(3.0f);
    const float popupHeight = itemHeight * count + popupPad * 2.0f;
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);

    ui.stack(id)
        .size(width, height)
        .zIndex(30)
        .content([&] {
            // ---- 字段（点击切换展开/收起）：文字显示当前项，或纯图标 ----
            if (field == PickerField::Icon) {
                components::button(ui, id + ".btn")
                    .size(width, height)
                    .icon(0xF0DC)  // fa-sort
                    .text("")
                    .iconSize(S(13.0f))
                    .theme(theme.components, false)
                    .onClick([&open] { open = !open; })
                    .build();
            } else {
                ui.rect(id + ".field")
                    .size(width, height)
                    .color(tokens.surface)
                    .radius(S(8.0f))
                    .border(1.0f, components::theme::withOpacity(tokens.border, 0.78f))
                    .transition(transition)
                    .onClick([&open] { open = !open; })
                    .build();

                ui.text(id + ".label")
                    .x(S(9.0f))
                    .size(width - S(30.0f), height)
                    .text(labels[selected])
                    .fontSize(S(11.0f))
                    .lineHeight(height)
                    .color(tokens.text)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();

                ui.text(id + ".chevron")
                    .x(width - S(20.0f))
                    .size(S(14.0f), height)
                    .icon(open ? 0xF077 : 0xF078)  // chevron-up / chevron-down
                    .fontSize(S(10.0f))
                    .lineHeight(height)
                    .color(tokens.primary)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();
            }

            // ---- 弹出列表（向上或向下展开）----
            if (open) {
                ui.stack(id + ".popup")
                    .y(opensUp ? -(popupHeight + popupGap) : height + popupGap)
                    .size(width, popupHeight)
                    .zIndex(31)
                    .content([&] {
                        ui.rect(id + ".popup.bg")
                            .size(width, popupHeight)
                            .color(tokens.dark ? tokens.surfaceActive : tokens.surface)
                            .radius(S(8.0f))
                            .border(1.0f, components::theme::withOpacity(tokens.border, 0.78f))
                            .build();

                        for (int i = 0; i < count; ++i) {
                            const float itemY = popupPad + i * itemHeight;
                            ui.rect(id + ".item." + std::to_string(i))
                                .x(popupPad)
                                .y(itemY)
                                .size(width - popupPad * 2.0f, itemHeight)
                                .states({0.0f, 0.0f, 0.0f, 0.0f}, tokens.surfaceHover,
                                        tokens.surfaceActive)
                                .radius(S(5.0f))
                                .onClick([&open, i, onPick] {
                                    open = false;
                                    onPick(i);
                                })
                                .build();

                            ui.text(id + ".item.label." + std::to_string(i))
                                .x(popupPad + S(8.0f))
                                .y(itemY)
                                .size(width - popupPad * 2.0f - S(16.0f), itemHeight)
                                .text(labels[i])
                                .fontSize(S(11.0f))
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

// 侧边栏列表项（普通列表）：图标 + 文字，激活时主色高亮 + 左侧竖条。
// 用于页面导航（下载列表 / 设置）和下载状态筛选（所有 / 下载中 / 已完成）。
export void drawSidebarItem(eui::Ui& ui, const std::string& id, float x, float y,
                            float width, float height, const std::string& label,
                            unsigned int icon, bool active, const AppTheme& theme,
                            std::function<void()> onClick) {
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
        .radius(S(8.0f))
        .transition(transition)
        .build();

    if (active) {
        ui.rect(id + ".bar")
            .position(x, y + S(6.0f))
            .size(S(3.0f), height - S(12.0f))
            .color(tokens.primary)
            .radius(S(1.5f))
            .build();
    }

    // 点击命中区（悬停反馈；激活项不再叠加 hover 底色）。
    ui.rect(id + ".hit")
        .position(x, y)
        .size(width, height)
        .states(idle, active ? idle : tokens.surfaceHover, tokens.surfaceActive)
        .radius(S(8.0f))
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    // 图标。
    ui.text(id + ".icon")
        .position(x + S(8.0f), y)
        .size(S(16.0f), height)
        .icon(icon)
        .fontSize(S(11.0f))
        .lineHeight(height)
        .color(textColor)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // 文字。
    ui.text(id + ".label")
        .position(x + S(30.0f), y)
        .size(width - S(36.0f), height)
        .text(label)
        .fontSize(S(12.0f))
        .lineHeight(height)
        .color(textColor)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
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
    const float itemW = railWidth - S(8.0f);   // 两侧各留 4px
    const float x = (railWidth - itemW) * 0.5f;

    // 激活高亮底色。
    ui.rect(id + ".bg")
        .position(x, y)
        .size(itemW, S(24.0f))
        .color(active ? activeFill : idle)
        .radius(S(7.0f))
        .transition(transition)
        .build();

    if (active) {
        ui.rect(id + ".bar")
            .position(0, y + S(4.0f))
            .size(S(2.0f), S(16.0f))
            .color(tokens.primary)
            .radius(S(1.0f))
            .build();
    }

    // 点击命中区（悬停反馈）。
    ui.rect(id + ".hit")
        .position(x, y)
        .size(itemW, S(24.0f))
        .states(idle, active ? idle : tokens.surfaceHover, tokens.surfaceActive)
        .radius(S(7.0f))
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    // 图标（水平居中）。
    ui.text(id + ".icon")
        .position(0, y)
        .size(railWidth, S(24.0f))
        .icon(icon)
        .fontSize(S(13.0f))
        .lineHeight(S(24.0f))
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
        .iconSize(S(11.0f))
        .theme(theme.components, primary)
        .onClick(std::move(onClick))
        .build();
}
