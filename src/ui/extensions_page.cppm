// ui/extensions_page.cppm — 扩展页：浏览器扩展（Chrome）+ MCP（AI 助手）接入。
//
// 设置页承载 8 个分组后已不适合再塞接入类内容，浏览器扩展与 MCP 拆到这一页：
// 状态行（宿主注册）、扩展 ID、安装步骤、扩展目录入口、MCP 客户端配置示例。
// 数据来源是 browser_host 的导出（宿主注册在 GUI 每次启动时自动执行，本页只读
// 状态 + 提供重新注册按钮）；页面本身无表单、无全局状态。
module;

#include "eui_ui.h"

export module tinynext.ui.extensions_page;

import std;
import tinynext.i18n;           // tr()
import tinynext.browser_host;   // chromeHostRegistered / chromeExtensionOrigin /
                                // chromeExtensionDir / registerChromeNativeHost
import tinynext.ui.theme;
import tinynext.ui.utils;       // pageContentX / kPageTop / kMonoFont 等布局令牌
import tinynext.ui.widgets;     // drawPanel / buttons / texts
import tinynext.ui.platform;    // openContainingFolder
import tinynext.store.ui;       // Page / g_page_view / showStatus

namespace {

// 面板内一行的通用排版常量（设计逻辑像素）。
constexpr float kExtPanelPad = 14.0f;
constexpr float kLabelW = 110.0f;
constexpr float kRowH = 30.0f;

// 面板内「标签 + 值」一行。值列从 labelW 起给满宽度；颜色/等宽由调用方显式给。
void drawRow(eui::Ui& sv, const std::string& id, float x, float y, float w,
             const char* label, const std::string& value, const AppTheme& theme,
             eui::Color valueColor, bool mono) {
    components::text(sv, id + ".label")
        .position(x + kExtPanelPad, y)
        .size(kLabelW, kRowH)
        .text(label)
        .fontSize(12.0f)
        .lineHeight(kRowH)
        .color(theme.metaText)
        .build();
    components::text(sv, id + ".value")
        .position(x + kExtPanelPad + kLabelW, y)
        .size(w - kExtPanelPad * 2.0f - kLabelW, kRowH)
        .text(value)
        .fontSize(11.5f)
        .lineHeight(kRowH)
        .fontFamily(mono ? kMonoFont : "")
        .color(valueColor)
        .build();
}

} // namespace

// ===================== 扩展页 =====================
// 与设置页同款骨架：右上角关闭回下载页，正文单列滚动，无底部操作区。
export void drawExtensionsPage(eui::Ui& ui, const eui::Screen& screen,
                               const AppTheme& theme) {
    const float contentX = pageContentX(screen.width);
    const float contentW = pageContentWidth(screen.width);
    const float innerW = std::min(contentW, kMaxSettingsFormWidth);
    // 关闭入口移除：左侧导航栏常驻后由侧栏切换页面。
   

    components::text(ui, "ext.title")
        .position(contentX, kPageTop)
        .size(innerW - kToolbarButtonSize - 8.0f, 22.0f)
        .text(tr("ext.title"))
        .fontSize(16.0f)
        .lineHeight(22.0f)
        .color(theme.titleText)
        .build();
    components::text(ui, "ext.subtitle")
        .position(contentX, kPageTop + 24.0f)
        .size(innerW, 18.0f)
        .text(tr("ext.subtitle"))
        .fontSize(12.0f)
        .lineHeight(18.0f)
        .color(theme.metaText)
        .build();

    const float bodyTop = kPageTop + 52.0f;
    const float bodyBottom = screen.height - kPageTop;
    const float bodyH = std::max(0.0f, bodyBottom - bodyTop);

    // ---- 正文内容高度（静态布局，与下方绘制严格对应）----
    // 面板 A：pad12 + 状态30 + 4 + ID30 + 6 + 步骤3×22 + 6 + 按钮28 + pad12
    const float panelA = 12.0f + 30.0f + 4.0f + 30.0f + 6.0f + 3 * 22.0f + 6.0f +
                         28.0f + 12.0f;
    // 面板 B：pad12 + 说明36 + 4 + 标签22 + 2 + mono 5×18 + 6 + 提示34 + pad12
    //（各段之间的空隙也算在内，少算会让 hint 文本溢出面板底边）。
    const float panelB = 12.0f + 36.0f + 4.0f + 22.0f + 2.0f + 5 * 18.0f + 6.0f +
                         34.0f + 12.0f;
    constexpr float secTitleH = 20.0f;
    constexpr float secGap = 18.0f;
    const float contentH = secTitleH + 6.0f + panelA + secGap + secTitleH + 6.0f +
                           panelB + 12.0f;

    components::scrollView(ui, "ext.scroll")
        .position(contentX, bodyTop)
        .size(contentW, bodyH)
        .scrollbarWidth(kScrollbarWidth)
        .scrollbarGap(kScrollbarGap)
        .theme(theme.components)
        .content([&](eui::Ui& sv, float fieldW, float) {
            sv.stack("ext.canvas")
                .size(fieldW, contentH)
                .content([&] {
                    // 滚动激活时 fieldW 已扣除滚动条宽度，居中夹紧不为负，
                    // 否则内容左移被视口裁掉首字符。
                    const float w = std::min(fieldW, innerW);
                    const float x = std::max(0.0f, (fieldW - w) / 2.0f);
                    float y = 0.0f;

                    // ---- 浏览器扩展（Chrome）----
                    components::text(sv, "ext.browser.title")
                        .position(x, y)
                        .size(w, secTitleH)
                        .text(tr("ext.browser_section"))
                        .fontSize(13.0f)
                        .lineHeight(secTitleH)
                        .color(theme.titleText)
                        .build();
                    y += secTitleH + 6.0f;
                    drawPanel(sv, "ext.browser.panel", x, y, w, panelA, theme);
                    {
                        const float py = y + 12.0f;
                        const bool registered = browser_host::chromeHostRegistered();
                        drawRow(sv, "ext.host.status", x, py, w, tr("ext.host_status"),
                                registered ? tr("ext.registered")
                                           : tr("ext.not_registered"),
                                theme, registered ? theme.done : theme.failed,
                                false);
                        components::button(sv, "ext.host.reregister")
                            .position(x + w - kExtPanelPad - 88.0f, py + 1.0f)
                            .size(88.0f, kButtonHeight)
                            .text(tr("ext.reregister"))
                            .fontSize(kButtonFontSize)
                            .theme(theme.components, false)
                            .radius(kButtonRadius)
                            .shadow(0.0f, 0.0f, 0.0f, core::Color{0, 0, 0, 0})
                            .onClick([] {
                                browser_host::registerChromeNativeHost();
                                showStatus(tr("ext.register_done"));
                            })
                            .build();

                        drawRow(sv, "ext.host.id", x, py + 34.0f, w,
                                tr("ext.extension_id"), browser_host::chromeExtensionOrigin(),
                                theme, theme.onSurface, true);

                        float sy = py + 70.0f;
                        const char* steps[] = {tr("ext.step1"), tr("ext.step2"),
                                               tr("ext.step3")};
                        for (int i = 0; i < 3; ++i) {
                            components::text(sv, "ext.step." + std::to_string(i))
                                .position(x + kExtPanelPad, sy)
                                .size(w - kExtPanelPad * 2.0f, 22.0f)
                                .text(steps[i])
                                .fontSize(11.5f)
                                .lineHeight(22.0f)
                                .maxWidth(w - kExtPanelPad * 2.0f)
                                .wrap(true)
                                .color(theme.metaText)
                                .build();
                            sy += 22.0f;
                        }

                        components::button(sv, "ext.host.opendir")
                            .position(x + kExtPanelPad, sy + 6.0f)
                            .size(120.0f, kButtonHeight)
                            .text(tr("ext.open_dir"))
                            .fontSize(kButtonFontSize)
                            .theme(theme.components, false)
                            .radius(kButtonRadius)
                            .shadow(0.0f, 0.0f, 0.0f, core::Color{0, 0, 0, 0})
                            .onClick([] {
                                const std::filesystem::path dir = browser_host::chromeExtensionDir();
                                if (dir.empty()) {
                                    showStatus(tr("ext.dir_missing"));
                                    return;
                                }
                                openContainingFolder(dir);
                            })
                            .build();
                        components::text(sv, "ext.host.edge")
                            .position(x + kExtPanelPad + 132.0f, sy + 12.0f)
                            .size(w - kExtPanelPad * 2.0f - 132.0f, 22.0f)
                            .text(tr("ext.edge_firefox"))
                            .fontSize(11.5f)
                            .lineHeight(22.0f)
                            .color(theme.metaText)
                            .build();
                    }
                    y += panelA + secGap;

                    // ---- MCP（AI 助手）----
                    components::text(sv, "ext.mcp.title")
                        .position(x, y)
                        .size(w, secTitleH)
                        .text(tr("ext.mcp_section"))
                        .fontSize(13.0f)
                        .lineHeight(secTitleH)
                        .color(theme.titleText)
                        .build();
                    y += secTitleH + 6.0f;
                    drawPanel(sv, "ext.mcp.panel", x, y, w, panelB, theme);
                    {
                        const float py = y + 12.0f;
                        components::text(sv, "ext.mcp.desc")
                            .position(x + kExtPanelPad, py)
                            .size(w - kExtPanelPad * 2.0f, 36.0f)
                            .text(tr("ext.mcp_desc"))
                            .fontSize(11.5f)
                            .lineHeight(18.0f)
                            .maxWidth(w - kExtPanelPad * 2.0f)
                            .wrap(true)
                            .color(theme.metaText)
                            .build();
                        components::text(sv, "ext.mcp.cfg.label")
                            .position(x + kExtPanelPad, py + 40.0f)
                            .size(w - kExtPanelPad * 2.0f, 22.0f)
                            .text(tr("ext.mcp_config_label"))
                            .fontSize(12.0f)
                            .lineHeight(22.0f)
                            .color(theme.onSurface)
                            .build();
                        const char* cfgLines[] = {
                            "{",
                            "  \"mcpServers\": {",
                            "    \"tinynext\": { \"command\": \"/path/to/tinynext\", \"args\": [\"--mcp\"] }",
                            "  }",
                            "}",
                        };
                        float cy = py + 64.0f;
                        for (int i = 0; i < 5; ++i) {
                            components::text(sv, "ext.mcp.cfg.l" + std::to_string(i))
                                .position(x + kExtPanelPad, cy)
                                .size(w - kExtPanelPad * 2.0f, 18.0f)
                                .text(cfgLines[i])
                                .fontSize(11.0f)
                                .lineHeight(18.0f)
                                .fontFamily(kMonoFont)
                                .color(theme.onSurface)
                                .build();
                            cy += 18.0f;
                        }
                        components::text(sv, "ext.mcp.hint")
                            .position(x + kExtPanelPad, cy + 6.0f)
                            .size(w - kExtPanelPad * 2.0f, 34.0f)
                            .text(tr("ext.mcp_hint"))
                            .fontSize(11.5f)
                            .lineHeight(17.0f)
                            .maxWidth(w - kExtPanelPad * 2.0f)
                            .wrap(true)
                            .color(theme.metaText)
                            .build();
                    }
                })
                .build();
        })
        .build();
}
