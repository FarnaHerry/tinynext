// ui/pages.cppm — the two app pages (downloads / settings) + the about dialog.
// Each page computes its own data/layout from the shared state module.
module;

#include "eui_ui.h"

export module tinynext.ui.pages;

import std;
import tinynext.config;
import tinynext.cli;
import tinynext.download_engine;
import tinynext.ui.theme;
import tinynext.ui.utils;
import tinynext.ui.widgets;
import tinynext.ui.cards;
import tinynext.ui.state;
import tinynext.ui.platform;

namespace {

// 两个 Aria2Config 是否等价：用于判断保存后 daemon 级参数是否真的变了，
// 从而决定是否提示"重启后生效"。
bool sameAria2Config(const cfg::Aria2Config& x, const cfg::Aria2Config& y) {
    return x.split == y.split &&
           x.maxConnectionPerServer == y.maxConnectionPerServer &&
           x.minSplitSize == y.minSplitSize &&
           x.maxDownloadLimit == y.maxDownloadLimit &&
           x.proxy == y.proxy &&
           x.noProxy == y.noProxy &&
           x.maxTries == y.maxTries &&
           x.retryWait == y.retryWait &&
           x.maxConcurrentDownloads == y.maxConcurrentDownloads &&
           x.removeControlFile == y.removeControlFile &&
           x.onDownloadComplete == y.onDownloadComplete &&
           x.userAgent == y.userAgent &&
           x.referer == y.referer &&
           x.diskCache == y.diskCache;
}

} // namespace

// ===================== 下载页 =====================
// 布局：左侧是下载状态子侧边栏，右侧是输入栏 + 卡片任务列表 + 翻页控件组。
export void drawDownloadsPage(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    // ---- 数据：筛选 + 排序 + 分页切片（每帧重跑，跟随下载线程实时刷新）----
    const auto tasks = g_manager->snapshot();
    std::vector<dl::TaskView> filtered;
    filtered.reserve(tasks.size());
    for (const auto& task : tasks) {
        if (stateMatches(g_filter, task.state)) {
            filtered.push_back(task);
        }
    }
    // 排序（稳定排序；Newest 保持 snapshot 的"最新在前"顺序）。
    switch (g_sort) {
        case SortMode::Newest:
            break;
        case SortMode::State: {
            auto rank = [](dl::State s) {
                switch (s) {
                    case dl::State::Downloading: return 0;
                    case dl::State::Queued:      return 1;
                    case dl::State::Paused:      return 2;
                    default:                     return 3;
                }
            };
            std::stable_sort(filtered.begin(), filtered.end(),
                             [&](const auto& a, const auto& b) {
                                 return rank(a.state) < rank(b.state);
                             });
            break;
        }
        case SortMode::Name:
            std::stable_sort(filtered.begin(), filtered.end(),
                             [](const auto& a, const auto& b) {
                                 return fileNameFromUrl(a.url) <
                                        fileNameFromUrl(b.url);
                             });
            break;
        case SortMode::Size:
            std::stable_sort(filtered.begin(), filtered.end(),
                             [](const auto& a, const auto& b) {
                                 return a.totalBytes > b.totalBytes;  // 大→小
                             });
            break;
        case SortMode::Progress: {
            auto pct = [](const dl::TaskView& t) {
                return t.totalBytes > 0
                           ? static_cast<double>(t.downloadedBytes) / t.totalBytes
                           : 0.0;
            };
            std::stable_sort(filtered.begin(), filtered.end(),
                             [&](const auto& a, const auto& b) {
                                 return pct(a) > pct(b);
                             });
            break;
        }
        case SortMode::Priority:
            // 高优先在前（数值大 = 高优先，与 priorityValueFromPicker 一致）。
            std::stable_sort(filtered.begin(), filtered.end(),
                             [](const auto& a, const auto& b) {
                                 return a.priority > b.priority;
                             });
            break;
    }
    const int totalCount = static_cast<int>(filtered.size());
    const int totalPages = std::max(1, (totalCount + g_pageSize - 1) / g_pageSize);
    g_page = std::clamp(g_page, 1, totalPages);
    const int start = (g_page - 1) * g_pageSize;
    const int end = std::min(totalCount, start + g_pageSize);

    // ---- 布局尺寸 ----
    const float inputY = S(14.0f);
    const float listTop = inputY + kInputHeight + S(10.0f);
    const float pagerY = screen.height - kPagerBottomMargin - kPagerHeight;
    const float listHeight = std::max(0.0f, pagerY - listTop - S(4.0f));

    const float railRight = kRailWidth + kMargin;            // 图标栏右侧
    const float subX = railRight;                            // 状态子侧边栏
    const float contentX = subX + kSubSidebarWidth + S(10.0f);  // 列表区起点
    const float contentWidth = screen.width - contentX - kMargin;

    // ---- 下载状态子侧边栏：所有 / 下载中 / 已完成 ----
    const float subY = inputY;
    const float subHeight = pagerY - inputY;
    ui.stack("sub.filter")
        .position(subX, subY)
        .size(kSubSidebarWidth, subHeight)
        .zIndex(4)
        .content([&] {
            ui.rect("sub.filter.bg")
                .position(0, 0)
                .size(kSubSidebarWidth, subHeight)
                .color(theme.components.surface)
                .radius(S(8.0f))
                .build();

            components::text(ui, "sub.filter.label")
                .position(S(9.0f), S(10.0f))
                .size(kSubSidebarWidth - S(18.0f), S(14.0f))
                .text("下载状态")
                .fontSize(S(10.0f))
                .lineHeight(S(14.0f))
                .color(theme.metaText)
                .build();

            const float itemW = kSubSidebarWidth - S(12.0f);
            float itemY = S(28.0f);
            drawSidebarItem(ui, "filter.all", S(6.0f), itemY, itemW, S(22.0f),
                            "所有", 0xF03A, g_filter == Filter::All, theme,
                            [] { g_filter = Filter::All; g_page = 1; });
            itemY += S(27.0f);
            drawSidebarItem(ui, "filter.active", S(6.0f), itemY, itemW, S(22.0f),
                            "下载中", 0xF019, g_filter == Filter::Active, theme,
                            [] { g_filter = Filter::Active; g_page = 1; });
            itemY += S(27.0f);
            drawSidebarItem(ui, "filter.done", S(6.0f), itemY, itemW, S(22.0f),
                            "已完成", 0xF00C, g_filter == Filter::Done, theme,
                            [] { g_filter = Filter::Done; g_page = 1; });
        })
        .build();

    // ---- 顶部工具栏（右对齐）：全部暂停 / 全部继续 / 排序 / 添加 ----
    const float toolW = S(28.0f);
    const float toolGap = S(4.0f);
    const float toolRight = contentX + contentWidth;
    const float addX = toolRight - toolW;
    const float sortX = addX - toolGap - toolW;
    const float startAllX = sortX - toolGap - toolW;
    const float pauseAllX = startAllX - toolGap - toolW;

    // 排序选择器：图标按钮 + 向下弹出列表（buildListPicker 不设位置，
    // 由外层 stack 绝对定位）。
    static const char* kSortLabels[] = {"最新在前", "状态优先", "文件名", "大小", "进度", "优先级"};
    ui.stack("tool.sort.wrap")
        .position(sortX, inputY)
        .size(toolW, kInputHeight)
        .zIndex(30)
        .content([&] {
            buildListPicker(ui, "tool.sort", toolW, kInputHeight, theme,
                            g_sortOpen, kSortLabels, 6,
                            static_cast<int>(g_sort), false,
                            PickerField::Icon,
                            [](int i) {
                                g_sort = static_cast<SortMode>(i);
                                g_page = 1;
                            });
        })
        .build();

    // 全部继续：恢复所有已暂停任务。
    components::button(ui, "tool.startAll")
        .position(startAllX, inputY)
        .size(toolW, kInputHeight)
        .icon(0xF04B)  // fa-play
        .text("")
        .iconSize(S(13.0f))
        .theme(theme.components, false)
        .onClick([] {
            g_manager->resumeAll();
            showStatus("已全部继续");
        })
        .build();

    // 全部暂停：暂停所有排队/进行中任务。
    components::button(ui, "tool.pauseAll")
        .position(pauseAllX, inputY)
        .size(toolW, kInputHeight)
        .icon(0xF04C)  // fa-pause
        .text("")
        .iconSize(S(13.0f))
        .theme(theme.components, false)
        .onClick([] {
            g_manager->pauseAll();
            showStatus("已全部暂停");
        })
        .build();

    // 添加下载：右上角 ➕ 图标，点击弹出对话框。
    components::button(ui, "add.btn")
        .position(addX, inputY)
        .size(toolW, kInputHeight)
        .icon(0xF067)  // fa-plus
        .text("")
        .iconSize(S(13.0f))
        .theme(theme.components, true)
        .onClick([] {
            // 打开弹窗：恢复默认选项——连接数填配置的 split 值，下载目录填配置
            // 的默认下载目录；其余清空。
            g_urlText.clear();
            g_addConnectionsText = std::to_string(cfg::aria2Config().split);
            g_addPriority = 0;
            g_addPriorityOpen = false;
            g_addRenameText.clear();
            g_addLimitText.clear();
            g_addDirText = cfg::downloadDir().string();
            g_addOpen = true;
        })
        .build();

    // ---- 任务列表：卡片式布局（名称/进度/信息纵向排布）----
    if (totalCount == 0) {
        const char* hint =
            g_filter == Filter::All     ? "暂无下载任务 — 点击右上角 ➕ 添加下载"
            : g_filter == Filter::Active ? "暂无下载中的任务"
                                          : "暂无已完成的任务";
        components::text(ui, "empty.hint")
            .position(contentX, listTop + S(16.0f))
            .size(contentWidth, S(24.0f))
            .text(hint)
            .fontSize(S(12.0f))
            .lineHeight(S(24.0f))
            .color(theme.hintText)
            .build();
    } else {
        components::scrollView(ui, "task.list")
            .position(contentX, listTop)
            .size(contentWidth, listHeight)
            .gap(kCardGap)
            .theme(theme.components)
            .content([&](eui::Ui& sv, float width, float viewportHeight) {
                for (int index = start; index < end; ++index) {
                    drawTaskCard(sv, filtered[index], width);
                }
            })
            .build();
    }

    // ---- 翻页控件组：◀ 第 X / Y 页 ▶ [分页大小]，整合进一个容器 ----
    constexpr float kChevWidth = S(18.0f);
    constexpr float kPageLabelWidth = S(54.0f);
    constexpr float kPagerGap = S(4.0f);
    const float groupWidth = kChevWidth + kPagerGap + kPageLabelWidth +
                             kPagerGap + kChevWidth + kPagerGap +
                             kSizeDropdownWidth;
    const float groupX = contentX + (contentWidth - groupWidth) * 0.5f;

    ui.row("pager.group")
        .position(groupX, pagerY)
        .size(groupWidth, kPagerHeight)
        .gap(kPagerGap)
        .alignItems(core::Align::CENTER)
        .zIndex(10)
        .content([&] {
            components::button(ui, "pager.prev")
                .size(kChevWidth, S(20.0f))
                .icon(0xF053)  // chevron-left
                .text("")
                .iconSize(S(11.0f))
                .theme(theme.components, false)
                .disabled(g_page <= 1)
                .onClick([] { if (g_page > 1) --g_page; })
                .build();

            components::text(ui, "pager.label")
                .size(kPageLabelWidth, kPagerHeight)
                .text(std::format("第 {} / {} 页", g_page, totalPages))
                .fontSize(S(11.0f))
                .lineHeight(kPagerHeight)
                .horizontalAlign(core::HorizontalAlign::Center)
                .color(theme.metaText)
                .build();

            components::button(ui, "pager.next")
                .size(kChevWidth, S(20.0f))
                .icon(0xF054)  // chevron-right
                .text("")
                .iconSize(S(11.0f))
                .theme(theme.components, false)
                .disabled(g_page >= totalPages)
                .onClick([totalPages] { if (g_page < totalPages) ++g_page; })
                .build();

            // 分页大小（5/10/20/50/100 上拉选择器），跟在下一页后面。
            static const char* kPageLabels[] = {"5 条/页", "10 条/页", "20 条/页", "50 条/页", "100 条/页"};
            buildListPicker(ui, "pager.pageSize", kSizeDropdownWidth,
                            kPagerHeight, theme, g_pageSizeOpen,
                            kPageLabels, 5, pageSizeIndex(), true,
                            PickerField::Text,
                            [](int i) {
                                g_pageSize = kPageSizes[i];
                                g_page = 1;
                            });
        })
        .build();

    // ---- 状态消息（短暂显示，翻页行上方）----
    if (g_statusTimer > 0.0f && !g_statusMessage.empty()) {
        components::text(ui, "status")
            .position(contentX, pagerY - S(24.0f))
            .size(contentWidth, S(18.0f))
            .text(g_statusMessage)
            .fontSize(S(12.0f))
            .lineHeight(S(18.0f))
            .color(theme.statusText)
            .build();
    }

    // ---- 添加下载弹窗（模态）：链接 + 每任务高级选项 ----
    if (g_addOpen) {
        const float dlgW = S(320.0f);
        const float dlgH = S(262.0f);
        const float dlgX = (screen.width - dlgW) * 0.5f;
        const float dlgY = (screen.height - dlgH) * 0.5f;
        const float labelX = S(16.0f);
        const float labelW = S(56.0f);
        const float inputX = S(74.0f);
        const float row2Y = S(76.0f);   // 连接数 / 优先级
        const float row3Y = S(112.0f);  // 重命名
        const float row4Y = S(148.0f);  // 限速
        const float row5Y = S(184.0f);  // 下载目录
        const float btnY = S(222.0f);

        // 半透明遮罩，点击空白处关闭。zIndex 高于侧边栏/翻页，
        // 保证整个窗口都被盖住。
        ui.rect("add.backdrop")
            .position(0, 0)
            .size(screen.width, screen.height)
            .zIndex(100)
            .color({0.0f, 0.0f, 0.0f, 0.45f})
            .onClick([] { g_addOpen = false; })
            .build();

        ui.stack("add.dialog")
            .position(dlgX, dlgY)
            .size(dlgW, dlgH)
            .zIndex(101)
            .content([&] {
                ui.rect("add.dialog.bg")
                    .position(0, 0)
                    .size(dlgW, dlgH)
                    .color(theme.components.surface)
                    .radius(S(10.0f))
                    .border(1.0f,
                            components::theme::withOpacity(
                                theme.components.border, 0.6f))
                    .build();

                components::text(ui, "add.dialog.title")
                    .position(labelX, S(12.0f))
                    .size(dlgW - S(32.0f), S(20.0f))
                    .text("添加下载")
                    .fontSize(S(14.0f))
                    .lineHeight(S(20.0f))
                    .color(theme.titleText)
                    .build();

                components::input(ui, "add.url")
                    .position(labelX, S(40.0f))
                    .size(dlgW - S(32.0f), S(28.0f))
                    .placeholder("https://… 或 magnet:…")
                    .value(g_urlText)
                    .theme(theme.components)
                    .onChange([](const std::string& value) { g_urlText = value; })
                    .onEnter([] { if (addDownload()) g_addOpen = false; })
                    .build();

                // ---- 连接数（0=配置默认；仅 aria2 生效）----
                components::text(ui, "add.conn.label")
                    .position(labelX, row2Y)
                    .size(labelW, S(28.0f))
                    .text("连接数")
                    .fontSize(S(12.0f))
                    .lineHeight(S(28.0f))
                    .color(theme.metaText)
                    .build();
                components::input(ui, "add.conn")
                    .position(inputX, row2Y - S(2.0f))
                    .size(S(84.0f), S(28.0f))
                    .placeholder("0=默认")
                    .value(g_addConnectionsText)
                    .theme(theme.components)
                    .onChange([](const std::string& value) { g_addConnectionsText = value; })
                    .onEnter([] { if (addDownload()) g_addOpen = false; })
                    .build();

                // ---- 优先级：默认/高/中/低（仅 aria2 生效）----
                components::text(ui, "add.priority.label")
                    .position(S(166.0f), row2Y)
                    .size(S(52.0f), S(28.0f))
                    .text("优先级")
                    .fontSize(S(12.0f))
                    .lineHeight(S(28.0f))
                    .color(theme.metaText)
                    .build();
                ui.stack("add.priority.wrap")
                    .position(S(222.0f), row2Y - S(2.0f))
                    .size(S(82.0f), S(28.0f))
                    .zIndex(32)
                    .content([&] {
                        static const char* kPriorityLabels[] = {"默认", "高", "中", "低"};
                        buildListPicker(ui, "add.priority", S(82.0f), S(28.0f),
                                        theme, g_addPriorityOpen, kPriorityLabels,
                                        4, g_addPriority, false, PickerField::Text,
                                        [](int i) { g_addPriority = i; });
                    })
                    .build();

                // ---- 重命名（可选，留空=URL 文件名）----
                components::text(ui, "add.rename.label")
                    .position(labelX, row3Y)
                    .size(labelW, S(28.0f))
                    .text("重命名")
                    .fontSize(S(12.0f))
                    .lineHeight(S(28.0f))
                    .color(theme.metaText)
                    .build();
                components::input(ui, "add.rename")
                    .position(inputX, row3Y - S(2.0f))
                    .size(dlgW - inputX - S(16.0f), S(28.0f))
                    .placeholder("可选")
                    .value(g_addRenameText)
                    .theme(theme.components)
                    .onChange([](const std::string& value) { g_addRenameText = value; })
                    .onEnter([] { if (addDownload()) g_addOpen = false; })
                    .build();

                // ---- 每任务限速 KB/s（0=全局配置）----
                components::text(ui, "add.limit.label")
                    .position(labelX, row4Y)
                    .size(labelW, S(28.0f))
                    .text("限速KB/s")
                    .fontSize(S(12.0f))
                    .lineHeight(S(28.0f))
                    .color(theme.metaText)
                    .build();
                components::input(ui, "add.limit")
                    .position(inputX, row4Y - S(2.0f))
                    .size(dlgW - inputX - S(16.0f), S(28.0f))
                    .placeholder("0=全局")
                    .value(g_addLimitText)
                    .theme(theme.components)
                    .onChange([](const std::string& value) { g_addLimitText = value; })
                    .onEnter([] { if (addDownload()) g_addOpen = false; })
                    .build();

                // ---- 下载目录（留空=全局；磁力链接建议填写）----
                components::text(ui, "add.dir.label")
                    .position(labelX, row5Y)
                    .size(labelW, S(28.0f))
                    .text("下载目录")
                    .fontSize(S(12.0f))
                    .lineHeight(S(28.0f))
                    .color(theme.metaText)
                    .build();
                components::input(ui, "add.dir")
                    .position(inputX, row5Y - S(2.0f))
                    .size(dlgW - inputX - S(16.0f) - S(60.0f) - S(8.0f), S(28.0f))
                    .placeholder("留空=全局")
                    .value(g_addDirText)
                    .theme(theme.components)
                    .onChange([](const std::string& value) { g_addDirText = value; })
                    .onEnter([] { if (addDownload()) g_addOpen = false; })
                    .build();
                components::button(ui, "add.dir.browse")
                    .position(inputX + (dlgW - inputX - S(16.0f) - S(60.0f) - S(8.0f)) +
                                  S(8.0f),
                              row5Y - S(2.0f))
                    .size(S(60.0f), S(26.0f))
                    .text("浏览…")
                    .fontSize(S(12.0f))
                    .theme(theme.components, false)
                    .onClick([] {
                        const auto picked = pickDownloadFolder();
                        if (!picked.empty()) {
                            g_addDirText = picked.string();
                        }
                    })
                    .build();

                components::button(ui, "add.cancel")
                    .position(dlgW - S(16.0f) - S(76.0f) - S(8.0f) - S(76.0f), btnY)
                    .size(S(76.0f), S(26.0f))
                    .text("取消")
                    .fontSize(S(12.0f))
                    .theme(theme.components, false)
                    .onClick([] { g_addOpen = false; })
                    .build();

                components::button(ui, "add.submit")
                    .position(dlgW - S(16.0f) - S(76.0f), btnY)
                    .size(S(76.0f), S(26.0f))
                    .text("提交")
                    .fontSize(S(12.0f))
                    .theme(theme.components, true)
                    .onClick([] { if (addDownload()) g_addOpen = false; })
                    .build();
            })
            .build();
    }
}

// ===================== 设置页 =====================
// 设置页没有下载状态子侧边栏，内容区紧跟图标栏右侧。
export void drawSettingsPage(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    const float contentX = kRailWidth + kMargin;
    const float contentWidth = screen.width - contentX - kMargin;
    const float infoX = contentX + S(8.0f);
    components::text(ui, "settings.title")
        .position(infoX, S(16.0f))
        .size(contentWidth - S(16.0f), S(24.0f))
        .text("设置")
        .fontSize(S(17.0f))
        .lineHeight(S(24.0f))
        .color(theme.titleText)
        .build();

    components::text(ui, "settings.subtitle")
        .position(infoX, S(42.0f))
        .size(contentWidth - S(16.0f), S(16.0f))
        .text("下载默认保存到你的系统下载目录，可在此修改。")
        .fontSize(S(11.0f))
        .lineHeight(S(16.0f))
        .color(theme.hintText)
        .build();

    // ---- 布局常量 ----
    constexpr float kLabelW = S(90.0f);
    constexpr float kFieldH = S(26.0f);
    constexpr float kActionH = S(26.0f);
    const float actionY = screen.height - kPagerBottomMargin - kActionH - S(8.0f);
    const float scrollTop = S(62.0f);
    const float scrollHeight = std::max(0.0f, actionY - S(10.0f) - scrollTop);

    // 主题按钮用的固定选项表。
    struct ThemeChoice { const char* label; cfg::ThemeMode mode; };
    static const ThemeChoice kThemeChoices[] = {
        {"跟随系统", cfg::ThemeMode::System},
        {"深色", cfg::ThemeMode::Dark},
        {"浅色", cfg::ThemeMode::Light},
    };

    // 设置项较多，正文放进 scrollView（主题/路径/aria2 参数）；底部操作行
    // 固定在窗口底部，始终可见。
    components::scrollView(ui, "settings.scroll")
        .position(contentX, scrollTop)
        .size(contentWidth, scrollHeight)
        .gap(S(6.0f))
        .theme(theme.components)
        .content([&](eui::Ui& sv, float width, float) {
            const float rowW = width;

            // 一行 = 一个定高 stack 子项，由 scrollView 纵向自动排布。
            auto row = [&](const std::string& id, float height,
                           const std::function<void(eui::Ui&, float)>& draw) {
                sv.stack("st." + id)
                    .width(rowW)
                    .height(height)
                    .content([&] { draw(sv, rowW); })
                    .build();
            };

            // 行内"标签 + 输入框"：x 相对行内，标签在左、输入框在右。
            auto field = [&](eui::Ui& r, const std::string& id, const char* label,
                             float x, float inputW, const std::string& value,
                             const std::function<void(const std::string&)>& onChange,
                             const char* placeholder = "") {
                components::text(r, "st." + id + ".label")
                    .position(x, 0)
                    .size(kLabelW, kFieldH)
                    .text(label)
                    .fontSize(S(11.0f))
                    .lineHeight(kFieldH)
                    .color(theme.metaText)
                    .build();
                components::input(r, "st." + id + ".input")
                    .position(x + kLabelW, -S(2.0f))
                    .size(inputW, S(26.0f))
                    .placeholder(placeholder)
                    .value(value)
                    .theme(theme.components)
                    .onChange(onChange)
                    .build();
            };

            // ---- 主题：跟随系统 / 深色 / 浅色 ----
            row("theme", kFieldH, [&](eui::Ui& r, float) {
                components::text(r, "st.theme.label")
                    .position(0, 0)
                    .size(kLabelW, kFieldH)
                    .text("主题")
                    .fontSize(S(12.0f))
                    .lineHeight(kFieldH)
                    .color(theme.metaText)
                    .build();
                float bx = kLabelW + S(8.0f);
                for (std::size_t i = 0; i < 3; ++i) {
                    const bool active = g_pendingTheme == kThemeChoices[i].mode;
                    components::button(r, std::format("st.theme.{}", i))
                        .position(bx, -S(1.0f))
                        .size(S(76.0f), S(24.0f))
                        .text(kThemeChoices[i].label)
                        .fontSize(S(12.0f))
                        .theme(theme.components, active)
                        .onClick([mode = kThemeChoices[i].mode] {
                            // 选择即预览；「保存」才落盘。
                            g_pendingTheme = mode;
                            g_themeMode = mode;
                            switch (mode) {
                                case cfg::ThemeMode::Dark:   g_dark = true;  break;
                                case cfg::ThemeMode::Light:  g_dark = false; break;
                                case cfg::ThemeMode::System: g_dark = cfg::osDark(); break;
                            }
                        })
                        .build();
                    bx += S(84.0f);
                }
            });

            // ---- 下载路径：输入框 + 系统文件夹选择器 ----
            row("path", kFieldH, [&](eui::Ui& r, float w) {
                components::text(r, "st.path.label")
                    .position(0, 0)
                    .size(kLabelW, kFieldH)
                    .text("下载路径")
                    .fontSize(S(12.0f))
                    .lineHeight(kFieldH)
                    .color(theme.metaText)
                    .build();
                const float pathInputW =
                    std::max(S(160.0f), w - S(16.0f) - kLabelW - S(8.0f) - S(60.0f));
                components::input(r, "st.path.input")
                    .position(kLabelW, -S(2.0f))
                    .size(pathInputW, S(26.0f))
                    .placeholder("下载保存目录")
                    .value(g_downloadDirText)
                    .theme(theme.components)
                    .onChange([](const std::string& value) { g_downloadDirText = value; })
                    .build();
                components::button(r, "st.path.browse")
                    .position(kLabelW + pathInputW + S(8.0f), -S(2.0f))
                    .size(S(60.0f), S(26.0f))
                    .text("浏览…")
                    .fontSize(S(12.0f))
                    .theme(theme.components, false)
                    .onClick([] {
                        // 只填待提交值，点「保存」才写入配置。
                        const auto picked = pickDownloadFolder();
                        if (!picked.empty()) {
                            g_downloadDirText = picked.string();
                        }
                    })
                    .build();
            });

            // ---- aria2 参数 ----
            {
                constexpr float kCol2X = S(86.0f) + S(90.0f) + S(20.0f);  // 第二列起点
                constexpr float kInputW = S(90.0f);
                const float fullW =
                    std::max(S(160.0f), rowW - S(16.0f) - kLabelW - S(8.0f));

                row("aria2.header", S(18.0f), [&](eui::Ui& r, float w) {
                    components::text(r, "st.aria2.header")
                        .position(0, 0)
                        .size(w, S(18.0f))
                        .text("aria2 参数")
                        .fontSize(S(11.0f))
                        .lineHeight(S(18.0f))
                        .color(theme.statusText)
                        .build();
                });

                row("a.split", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.split", "分片数", 0, kInputW, g_aria2SplitText,
                          [](const std::string& v) { g_aria2SplitText = v; });
                    field(r, "a.conn", "每服务器连接", kCol2X, kInputW, g_aria2ConnText,
                          [](const std::string& v) { g_aria2ConnText = v; });
                });
                row("a.minsplit", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.minsplit", "最小分片", 0, kInputW, g_aria2MinSplitText,
                          [](const std::string& v) { g_aria2MinSplitText = v; });
                    field(r, "a.limit", "限速KB/s", kCol2X, kInputW, g_aria2LimitText,
                          [](const std::string& v) { g_aria2LimitText = v; });
                });
                row("a.proxy", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.proxy", "代理地址", 0, fullW, g_proxyText,
                          [](const std::string& v) { g_proxyText = v; },
                          "http://user:pass@host:port");
                });
                row("a.noproxy", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.noproxy", "不使用代理", 0, fullW, g_noProxyText,
                          [](const std::string& v) { g_noProxyText = v; },
                          "host1,host2（逗号分隔）");
                });
                row("a.retry", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.maxtries", "最大重试次数", 0, kInputW, g_maxTriesText,
                          [](const std::string& v) { g_maxTriesText = v; },
                          "0=无限");
                    field(r, "a.retrywait", "重试等待秒", kCol2X, kInputW, g_retryWaitText,
                          [](const std::string& v) { g_retryWaitText = v; });
                });
                row("a.concurrent", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.concurrent", "最大同时下载数", 0, fullW,
                          g_maxConcurrentText,
                          [](const std::string& v) { g_maxConcurrentText = v; });
                });
                row("a.remctrl", kFieldH, [&](eui::Ui& r, float) {
                    components::text(r, "st.a.remctrl.label")
                        .position(0, 0)
                        .size(kLabelW, kFieldH)
                        .text("移除控制文件")
                        .fontSize(S(11.0f))
                        .lineHeight(kFieldH)
                        .color(theme.metaText)
                        .build();
                    components::button(r, "st.a.remctrl.toggle")
                        .position(kLabelW, -S(1.0f))
                        .size(S(48.0f), S(24.0f))
                        .text(g_removeControlFile ? "开" : "关")
                        .fontSize(S(11.0f))
                        .theme(theme.components, g_removeControlFile)
                        .onClick([] { g_removeControlFile = !g_removeControlFile; })
                        .build();
                });
                row("a.oncomplete", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.oncomplete", "完成后命令", 0, fullW, g_onCompleteText,
                          [](const std::string& v) { g_onCompleteText = v; },
                          "命令 参数（aria2 追加 GID/文件数/路径）");
                });
                row("a.ua", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.ua", "User-Agent", 0, kInputW, g_userAgentText,
                          [](const std::string& v) { g_userAgentText = v; });
                    field(r, "a.referer", "Referer", kCol2X, kInputW, g_refererText,
                          [](const std::string& v) { g_refererText = v; });
                });
                row("a.diskcache", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.diskcache", "磁盘缓存", 0, fullW, g_diskCacheText,
                          [](const std::string& v) { g_diskCacheText = v; },
                          "如 16M，空=aria2 默认");
                });
            }
        })
        .build();

    // ---- 操作行（固定窗口底部）：恢复默认路径 / 保存全部设置 / 放弃修改 ----
    components::button(ui, "settings.path.reset")
        .position(infoX + kLabelW, actionY)
        .size(S(76.0f), S(26.0f))
        .text("恢复默认")
        .fontSize(S(12.0f))
        .theme(theme.components, false)
        .onClick([] { g_downloadDirText = cfg::defaultDownloadDir().string(); })
        .build();

    components::button(ui, "settings.save")
        .position(infoX + kLabelW + S(8.0f) + S(76.0f), actionY)
        .size(S(76.0f), S(26.0f))
        .text("保存")
        .fontSize(S(12.0f))
        .theme(theme.components, true)
        .onClick([] {
            const std::string t = trimText(g_downloadDirText);
            if (t.empty()) {
                showStatus("下载路径不能为空");
                return;
            }
            g_downloadDirText = t;

            // 主题：套用待提交值并落盘。
            g_themeMode = g_pendingTheme;
            switch (g_pendingTheme) {
                case cfg::ThemeMode::Dark:   g_dark = true;  break;
                case cfg::ThemeMode::Light:  g_dark = false; break;
                case cfg::ThemeMode::System: g_dark = cfg::osDark(); break;
            }
            cfg::setThemeMode(g_pendingTheme);

            // aria2 参数：解析并夹取后落盘，回写规范化文本。
            const cfg::Aria2Config cur = cfg::aria2Config();
            cfg::Aria2Config a2;
            a2.split = parseIntClamped(g_aria2SplitText, 1, 64, cur.split);
            a2.maxConnectionPerServer =
                parseIntClamped(g_aria2ConnText, 1, 64, cur.maxConnectionPerServer);
            a2.minSplitSize = "1M";
            if (parseSizeBytes(g_aria2MinSplitText) >= 1048576) {
                a2.minSplitSize = trimText(g_aria2MinSplitText);
            }
            a2.maxDownloadLimit =
                static_cast<std::int64_t>(
                    parseIntClamped(g_aria2LimitText, 0, 1000000, 0)) * 1024;
            a2.proxy = trimText(g_proxyText);
            a2.noProxy = trimText(g_noProxyText);
            a2.maxTries = parseIntClamped(g_maxTriesText, 0, 100, cur.maxTries);
            a2.retryWait = parseIntClamped(g_retryWaitText, 0, 600, cur.retryWait);
            a2.maxConcurrentDownloads =
                parseIntClamped(g_maxConcurrentText, 1, 64, cur.maxConcurrentDownloads);
            a2.removeControlFile = g_removeControlFile;
            a2.onDownloadComplete = trimText(g_onCompleteText);
            a2.userAgent = trimText(g_userAgentText);
            a2.referer = trimText(g_refererText);
            a2.diskCache = trimText(g_diskCacheText);

            const bool a2Changed = !sameAria2Config(cur, a2);
            cfg::setAria2Config(a2);
            g_aria2SplitText = std::to_string(a2.split);
            g_aria2ConnText = std::to_string(a2.maxConnectionPerServer);
            g_aria2MinSplitText = a2.minSplitSize;
            g_aria2LimitText = std::to_string(a2.maxDownloadLimit / 1024);
            g_proxyText = a2.proxy;
            g_noProxyText = a2.noProxy;
            g_maxTriesText = std::to_string(a2.maxTries);
            g_retryWaitText = std::to_string(a2.retryWait);
            g_maxConcurrentText = std::to_string(a2.maxConcurrentDownloads);
            g_onCompleteText = a2.onDownloadComplete;
            g_userAgentText = a2.userAgent;
            g_refererText = a2.referer;
            g_diskCacheText = a2.diskCache;

            // 下载路径。
            cfg::setDownloadDir(g_downloadDirText);

            // 汇总提示：aria2 daemon 已启动时，参数保存后需重启才生效。
            if (a2Changed && g_manager->engineActive()) {
                showStatus("aria2 参数将在重启后生效");
            } else {
                showStatus("设置已保存");
            }
        })
        .build();

    components::button(ui, "settings.discard")
        .position(infoX + kLabelW + S(8.0f) + S(76.0f) + S(8.0f) + S(76.0f), actionY)
        .size(S(76.0f), S(26.0f))
        .text("放弃")
        .fontSize(S(12.0f))
        .theme(theme.components, false)
        .onClick([] {
            // 回滚到已保存值。
            g_pendingTheme = cfg::themeMode();
            g_downloadDirText = cfg::downloadDir().string();
            g_themeMode = g_pendingTheme;
            g_dark = cfg::effectiveDark();
            const cfg::Aria2Config a2 = cfg::aria2Config();
            g_aria2SplitText = std::to_string(a2.split);
            g_aria2ConnText = std::to_string(a2.maxConnectionPerServer);
            g_aria2MinSplitText = a2.minSplitSize;
            g_aria2LimitText = std::to_string(a2.maxDownloadLimit / 1024);
            g_proxyText = a2.proxy;
            g_noProxyText = a2.noProxy;
            g_maxTriesText = std::to_string(a2.maxTries);
            g_retryWaitText = std::to_string(a2.retryWait);
            g_maxConcurrentText = std::to_string(a2.maxConcurrentDownloads);
            g_removeControlFile = a2.removeControlFile;
            g_onCompleteText = a2.onDownloadComplete;
            g_userAgentText = a2.userAgent;
            g_refererText = a2.referer;
            g_diskCacheText = a2.diskCache;
            showStatus("已放弃更改");
        })
        .build();
}

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
                {"版本", "0.1.0"},
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

