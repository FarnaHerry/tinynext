// ui/about_dialog.cppm — 关于弹窗：软件信息 + 项目链接。
// 从 tinynext.ui.pages 拆出，独立成模块。
module;

#include "eui_ui.h"

export module tinynext.ui.about_dialog;

import std;
import tinynext.ui.theme;
import tinynext.ui.utils;
import tinynext.ui.state;
import tinynext.ui.platform;

// ---- 关于弹窗：软件信息（所有页面可见）----
export void drawAboutDialog(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    if (!g_aboutOpen) return;
    const float dlgW = S(300.0f);
    const float dlgH = S(306.0f);
    const float dlgX = (screen.width - dlgW) * 0.5f;
    const float dlgY = (screen.height - dlgH) * 0.5f;

    ui.rect("about.backdrop")
        .position(0, 0)
        .size(screen.width, screen.height)
        .zIndex(100)
        .color({0.0f, 0.0f, 0.0f, 0.45f})
        .onClick([] { g_aboutOpen = false; })
        .build();

    ui.stack("about.dialog")
        .position(dlgX, dlgY)
        .size(dlgW, dlgH)
        .zIndex(101)
        .content([&] {
            ui.rect("about.dialog.bg")
                .position(0, 0)
                .size(dlgW, dlgH)
                .color(theme.components.surface)
                .radius(S(10.0f))
                .border(1.0f,
                        components::theme::withOpacity(
                            theme.components.border, 0.6f))
                .onClick([] {})  // 吞掉弹窗内部空白点击，避免穿透到遮罩关闭弹窗
                .build();

            components::text(ui, "about.title")
                .position(S(16.0f), S(14.0f))
                .size(dlgW - S(32.0f), S(22.0f))
                .text("关于 TinyNext")
                .fontSize(S(15.0f))
                .lineHeight(S(22.0f))
                .color(theme.titleText)
                .build();

            struct AboutRow { const char* label; const char* value; };
            static const AboutRow kAboutRows[] = {
                {"应用名称", "TinyNext 下载器"},
                {"版本", "0.2.2"},
                {"界面框架", "EUI-NEO 0.5.3"},
                {"下载引擎", "aria2-next 2.5.5"},
                {"网络传输", "本地 socket（无外部依赖）"},
                {"构建工具", "mcpp（C++23）"},
            };
            float rowY = S(40.0f);
            for (const auto& row : kAboutRows) {
                components::text(ui, std::format("about.k{}.label", row.label))
                    .position(S(16.0f), rowY)
                    .size(S(90.0f), S(22.0f))
                    .text(row.label)
                    .fontSize(S(11.0f))
                    .lineHeight(S(22.0f))
                    .color(theme.metaText)
                    .build();
                components::text(ui, std::format("about.k{}.value", row.label))
                    .position(S(108.0f), rowY)
                    .size(dlgW - S(124.0f), S(22.0f))
                    .text(row.value)
                    .fontSize(S(11.0f))
                    .lineHeight(S(22.0f))
                    .color(theme.nameText)
                    .build();
                rowY += S(22.0f);
            }

            // ---- 项目主页 ----
            components::text(ui, "about.links.header")
                .position(S(16.0f), rowY + S(4.0f))
                .size(dlgW - S(32.0f), S(16.0f))
                .text("项目主页")
                .fontSize(S(11.0f))
                .lineHeight(S(16.0f))
                .color(theme.statusText)
                .build();

            struct LinkRow { const char* label; const char* url; };
            static const LinkRow kLinks[] = {
                {"TinyNext 下载器", "https://github.com/FarnaHerry/tinynext"},
                {"mcpp 构建工具", "https://github.com/mcpp-community/mcpp"},
                {"EUI-NEO 界面框架", "https://github.com/sudoevolve/EUI-NEO"},
            };
            float linkY = rowY + S(22.0f);
            for (const auto& link : kLinks) {
                components::button(ui, std::format("about.link.{}", link.label))
                    .position(S(16.0f), linkY)
                    .size(S(180.0f), S(24.0f))
                    .text(link.label)
                    .fontSize(S(11.0f))
                    .theme(theme.components, false)
                    .onClick([url = std::string(link.url)] { openUrl(url); })
                    .build();
                linkY += S(26.0f);
            }

            components::button(ui, "about.close")
                .position((dlgW - S(76.0f)) * 0.5f, dlgH - S(30.0f))
                .size(S(76.0f), S(24.0f))
                .text("关闭")
                .fontSize(S(12.0f))
                .theme(theme.components, true)
                .onClick([] { g_aboutOpen = false; })
                .build();
        })
        .build();
}
