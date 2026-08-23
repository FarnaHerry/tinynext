// ui/about_dialog.cppm — 关于弹窗：软件信息 + 项目链接。
// 从 tinynext.ui.pages 拆出，独立成模块。
module;

#include "eui_ui.h"

export module tinynext.ui.about_dialog;

import std;
import tinynext.config;
import tinynext.i18n;   // tr（关于弹窗文案）
import tinynext.ui.theme;
import tinynext.ui.utils;
import tinynext.ui.widgets;     // glassFill（毛玻璃填充色）
import tinynext.store.dialogs;  // g_aboutOpen
import tinynext.store.tasks;    // g_tasks.health()（引擎真实版本）
import tinynext.video_resolver; // yt-dlp/ffmpeg 真实版本
import tinynext.ui.platform;

// ---- 关于弹窗：软件信息（所有页面可见）----
export void drawAboutDialog(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    if (!g_aboutOpen) return;
    const float dlgW = 300.0f;
    const float dlgH = 350.0f;
    const float dlgX = (screen.width - dlgW) * 0.5f;
    const float dlgY = (screen.height - dlgH) * 0.5f;

    ui.rect("about.backdrop")
        .position(0, 0)
        .size(screen.width, screen.height)
        .zIndex(100)
        .color({0.0f, 0.0f, 0.0f, 0.32f})
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
                .blur(10.0f)
                .color(glassFill(theme, 0.52f))
                .radius(10.0f)
                .border(1.0f,
                        components::theme::withOpacity(
                            theme.components.border, 0.6f))
                .onClick([] {})  // 吞掉弹窗内部空白点击，避免穿透到遮罩关闭弹窗
                .build();

            components::text(ui, "about.title")
                .position(16.0f, 14.0f)
                .size(dlgW - 32.0f, 22.0f)
                .text(tr("about.title"))
                .fontSize(15.0f)
                .lineHeight(22.0f)
                .color(theme.titleText)
                .build();

            // 版本全部来自实际运行中的组件，不硬编码：应用/eui 版本由
            // mcpp.toml 生成（versions.generated.h → config.cppm）；aria2-next
            // 版本来自 daemon 的 getVersion RPC（预热时缓存进 health）；
            // yt-dlp/ffmpeg 版本来自预热线程对二进制的 --version 探测。
            // 探测完成前/守护未起时返回空串，降级只显示组件名。
            const std::string appVersion(cfg::kAppVersion);
            const std::string uiVersion =
                std::string("EUI-NEO ") + std::string(cfg::kEuiVersion);
            const auto toolRow = [](const char* name, const std::string& ver) {
                return ver.empty() ? std::string(name) : std::string(name) + " " + ver;
            };
            struct AboutRow { const char* label; std::string value; };
            const AboutRow kAboutRows[] = {
                {tr("about.app_name"), tr("about.app_name_value")},
                {tr("about.version"), appVersion},
                {tr("about.ui_framework"), uiVersion},
                {tr("about.engine"), toolRow("aria2-next", g_tasks.health().version)},
                {tr("about.video_tool"), toolRow("yt-dlp", video::ytDlpVersion())},
                {tr("about.merge_tool"), toolRow("ffmpeg", video::ffmpegVersion())},
                {tr("about.transport"), tr("about.transport_value")},
                {tr("about.build_tool"), "mcpp（C++23）"},
            };
            float rowY = 40.0f;
            for (const auto& row : kAboutRows) {
                components::text(ui, std::format("about.k{}.label", row.label))
                    .position(16.0f, rowY)
                    .size(90.0f, 22.0f)
                    .text(row.label)
                    .fontSize(11.0f)
                    .lineHeight(22.0f)
                    .color(theme.metaText)
                    .build();
                components::text(ui, std::format("about.k{}.value", row.label))
                    .position(108.0f, rowY)
                    .size(dlgW - 124.0f, 22.0f)
                    .text(row.value)
                    .fontSize(11.0f)
                    .lineHeight(22.0f)
                    .color(theme.nameText)
                    .build();
                rowY += 22.0f;
            }

            // ---- 项目主页 ----
            components::text(ui, "about.links.header")
                .position(16.0f, rowY + 4.0f)
                .size(dlgW - 32.0f, 16.0f)
                .text(tr("about.project_home"))
                .fontSize(11.0f)
                .lineHeight(16.0f)
                .color(theme.statusText)
                .build();

            struct LinkRow { const char* label; const char* url; };
            const LinkRow kLinks[] = {
                {tr("about.app_name_value"), "https://github.com/FarnaHerry/tinynext"},
                {tr("about.build_tool_value"), "https://github.com/mcpp-community/mcpp"},
                {tr("about.ui_framework_value"), "https://github.com/sudoevolve/EUI-NEO"},
            };
            float linkY = rowY + 22.0f;
            for (const auto& link : kLinks) {
                components::button(ui, std::format("about.link.{}", link.label))
                    .position(16.0f, linkY)
                    .size(180.0f, 24.0f)
                    .text(link.label)
                    .fontSize(11.0f)
                    .theme(theme.components, false)
                    .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
                    .onClick([url = std::string(link.url)] { openUrl(url); })
                    .build();
                linkY += 26.0f;
            }

            components::button(ui, "about.close")
                .position((dlgW - 76.0f) * 0.5f, dlgH - 30.0f)
                .size(76.0f, 24.0f)
                .text(tr("about.close"))
                .fontSize(12.0f)
                .theme(theme.components, true)
                .textColor(onPrimaryColor(theme))
                .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .onClick([] { g_aboutOpen = false; })
                .build();
        })
        .build();
}
