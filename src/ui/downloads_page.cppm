// ui/downloads_page.cppm — 下载页：任务列表 + 状态筛选 + 工具栏 + 翻页 + 添加下载弹窗。
// 从 tinynext.ui.pages 拆出，独立成模块，避免单文件管理过多页面。
module;

#include "eui_ui.h"

export module tinynext.ui.downloads_page;

import std;
import tinynext.config;
import tinynext.download_engine;
import tinynext.ui.theme;
import tinynext.ui.utils;
import tinynext.ui.widgets;
import tinynext.ui.cards;
import tinynext.ui.state;
import tinynext.ui.platform;

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

    // ---- 布局尺寸（岛屿卡片风：内容区两张浮岛卡片）----
    // 图标栏占满左缘（整高竖条、不套卡片），状态子侧边栏 + 内容大卡两张浮岛
    // 紧贴图标栏右侧，上下各留 kIslandVInset 空隙（卡片感）；仅右侧留 kRightMargin。
    const float islandTop = kIslandVInset;
    const float islandH = screen.height - 2.0f * kIslandVInset;
    const float subX = kRailWidth;
    const float contentX = subX + kSubSidebarWidth + kIslandGap;
    const float contentW = screen.width - contentX - kRightMargin;

    // 内容大卡：内边距 kPanelPad，卡内依次是 工具栏 / 任务列表 / 状态消息 / 翻页。
    const float pad = kPanelPad;
    const float toolY = islandTop + pad;
    const float pagerY = islandTop + islandH - pad - kPagerHeight;
    const float listTop = toolY + kInputHeight + S(8.0f);
    const float listHeight = std::max(0.0f, pagerY - listTop - S(4.0f));
    const float listX = contentX + pad;
    const float listW = contentW - 2.0f * pad;

    drawPanel(ui, "dl.content.panel", contentX, islandTop, contentW, islandH, theme);

    // ---- 下载状态子侧边栏：所有 / 下载中 / 已完成（独立岛卡片）----
    ui.stack("sub.filter")
        .position(subX, islandTop)
        .size(kSubSidebarWidth, islandH)
        .zIndex(4)
        .content([&] {
            drawPanel(ui, "sub.filter.bg", 0, 0, kSubSidebarWidth, islandH, theme);

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

    // ---- 顶部工具栏（右对齐，收在内容大卡内）：全部暂停 / 全部继续 / 排序 / 添加 ----
    const float toolW = S(28.0f);
    const float toolGap = S(4.0f);
    const float toolRight = contentX + contentW - pad;
    const float addX = toolRight - toolW;
    const float sortX = addX - toolGap - toolW;
    const float startAllX = sortX - toolGap - toolW;
    const float pauseAllX = startAllX - toolGap - toolW;

    // 排序选择器：图标按钮 + 向下弹出列表（buildListPicker 不设位置，
    // 由外层 stack 绝对定位）。
    static const char* kSortLabels[] = {"最新在前", "状态优先", "文件名", "大小", "进度", "优先级"};
    ui.stack("tool.sort.wrap")
        .position(sortX, toolY)
        .size(toolW, toolW)
        .zIndex(30)
        .content([&] {
            buildListPicker(ui, "tool.sort", toolW, toolW, theme,
                            g_sortOpen, kSortLabels, 6,
                            static_cast<int>(g_sort), false,
                            PickerField::Icon,
                            [](int i) {
                                g_sort = static_cast<SortMode>(i);
                                g_page = 1;
                            },
                            S(96.0f));  // 字段是图标，弹层加宽容纳文字
        })
        .build();

    // 全部继续：恢复所有已暂停任务（正圆，默认无描边，hover 才浮现）。
    drawToolbarIconButton(ui, "tool.startAll", startAllX, toolY, toolW, toolW,
                          0xF04B, false, theme,
                          [] {
                              g_manager->resumeAll();
                              showStatus("已全部继续");
                          });

    // 全部暂停：暂停所有排队/进行中任务（正圆，默认无描边，hover 才浮现）。
    drawToolbarIconButton(ui, "tool.pauseAll", pauseAllX, toolY, toolW, toolW,
                          0xF04C, false, theme,
                          [] {
                              g_manager->pauseAll();
                              showStatus("已全部暂停");
                          });

    // 添加下载：右上角 ➕ 图标（正圆 + 一直主色填充），点击弹出对话框。
    drawToolbarIconButton(ui, "add.btn", addX, toolY, toolW, toolW,
                          0xF067, true, theme,
                          [] {
                              // 打开弹窗：恢复默认选项——连接数填配置的 split 值，下载目录填配置
                              // 的默认下载目录；其余清空。若剪贴板是 http(s)/magnet 链接则预填 URL。
                              g_urlText.clear();
                              const std::string clip = trimText(getClipboardText());
                              if (!clip.empty()) {
                                  if (clip.starts_with("http://") || clip.starts_with("https://") ||
                                      clip.starts_with("magnet:")) {
                                      g_urlText = clip;
                                  }
                              }
                              g_addConnectionsText = std::to_string(cfg::aria2Config().split);
                              g_addPriority = 0;
                              g_addPriorityOpen = false;
                              g_addRenameText.clear();
                              g_addLimitText.clear();
                              g_addDirText = cfg::downloadDir().string();
                              g_addOpen = true;
                          });

    // ---- 任务列表：卡片式布局（名称/进度/信息纵向排布）----
    if (totalCount == 0) {
        const char* hint =
            g_filter == Filter::All     ? "暂无下载任务 — 点击右上角 ➕ 添加下载"
            : g_filter == Filter::Active ? "暂无下载中的任务"
                                          : "暂无已完成的任务";
        components::text(ui, "empty.hint")
            .position(listX, listTop + S(16.0f))
            .size(listW, S(24.0f))
            .text(hint)
            .fontSize(S(12.0f))
            .lineHeight(S(24.0f))
            .color(theme.hintText)
            .build();
    } else {
        components::scrollView(ui, "task.list")
            .position(listX, listTop)
            .size(listW, listHeight)
            .gap(kCardGap)
            .theme(theme.components)
            .content([&](eui::Ui& sv, float width, float viewportHeight) {
                for (int index = start; index < end; ++index) {
                    drawTaskCard(sv, filtered[index], width);
                }
            })
            .build();
    }

    // ---- 翻页控件组：◀ 页码 ▶ [数字/页]，整组收进一张小卡片 ----
    // 简洁版：中间只显示当前页码数字（不再显示"第 X / Y 页"），分页大小是
    // 无边框的纯文本"数字/页"。整组控件包在一张圆角小卡里（岛内小岛）。
    constexpr float kChevWidth = S(18.0f);
    constexpr float kPageLabelWidth = S(28.0f);   // 仅当前页码
    constexpr float kPageSizeWidth = S(52.0f);    // "数字/页" 纯文本
    constexpr float kPagerGap = S(4.0f);
    constexpr float kPagerPadH = S(10.0f);        // 卡片水平内边距
    constexpr float kPagerPadV = S(3.0f);         // 卡片垂直内边距
    const float groupWidth = kChevWidth + kPagerGap + kPageLabelWidth +
                             kPagerGap + kChevWidth + kPagerGap +
                             kPageSizeWidth;
    const float pagerCardW = groupWidth + 2.0f * kPagerPadH;
    const float pagerCardH = kPagerHeight + 2.0f * kPagerPadV;
    const float pagerCardX = contentX + (contentW - pagerCardW) * 0.5f;
    const float pagerCardY = pagerY - kPagerPadV;

    ui.stack("pager.card")
        .position(pagerCardX, pagerCardY)
        .size(pagerCardW, pagerCardH)
        .zIndex(10)
        .content([&] {
            // 卡片底：圆角表面 + 细边框 + 柔和投影（与任务卡同风格）。
            ui.rect("pager.card.bg")
                .position(0, 0)
                .size(pagerCardW, pagerCardH)
                .color(theme.components.surface)
                .radius(S(8.0f))
                .border(1.0f, components::theme::withOpacity(theme.components.border, 0.55f))
                .shadow(S(8.0f), S(2.0f),
                        theme.components.dark
                            ? core::Color{0.0f, 0.0f, 0.0f, 0.18f}
                            : core::Color{0.10f, 0.14f, 0.22f, 0.08f})
                .build();

            ui.row("pager.group")
                .position(kPagerPadH, kPagerPadV)
                .size(groupWidth, kPagerHeight)
                .gap(kPagerGap)
                .alignItems(core::Align::CENTER)
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
                        .text(std::format("{}", g_page))
                        .fontSize(S(11.0f))
                        .lineHeight(kPagerHeight)
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .verticalAlign(core::VerticalAlign::Center)
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

                    // 分页大小（5/10/20/50/100，纯文本"数字/页"无边框），跟在下一页后面。
                    static const char* kPageLabels[] = {"5/页", "10/页", "20/页", "50/页", "100/页"};
                    buildListPicker(ui, "pager.pageSize", kPageSizeWidth,
                                    kPagerHeight, theme, g_pageSizeOpen,
                                    kPageLabels, 5, pageSizeIndex(), true,
                                    PickerField::Plain,
                                    [](int i) {
                                        g_pageSize = kPageSizes[i];
                                        g_page = 1;
                                    });
                })
                .build();
        })
        .build();

    // ---- 状态消息（短暂显示，翻页行上方）----
    if (g_statusTimer > 0.0f && !g_statusMessage.empty()) {
        components::text(ui, "status")
            .position(listX, pagerY - S(24.0f))
            .size(listW, S(18.0f))
            .text(g_statusMessage)
            .fontSize(S(12.0f))
            .lineHeight(S(18.0f))
            .color(theme.statusText)
            .build();
    }

    // ---- 添加下载弹窗（模态）：链接 + 每任务高级选项 ----
    if (g_addOpen) {
        const float dlgW = S(320.0f);
        const float dlgH = S(316.0f);
        const float dlgX = (screen.width - dlgW) * 0.5f;
        const float dlgY = (screen.height - dlgH) * 0.5f;
        const float labelX = S(16.0f);
        const float labelW = S(56.0f);
        const float inputX = S(74.0f);
        const float urlH = S(48.0f);      // URL 多行输入高度（长链接可见）
        const float splitY = S(96.0f);    // 分片数（独立一行）
        const float priorityY = S(132.0f); // 优先级（独立一行，普通字段）
        const float row3Y = S(168.0f);    // 重命名
        const float row4Y = S(204.0f);    // 限速
        const float row5Y = S(240.0f);    // 下载目录
        const float btnY = S(278.0f);

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
                    .onClick([] {})  // 吞掉弹窗内部空白点击，避免穿透到遮罩关闭弹窗
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
                    .size(dlgW - S(32.0f), urlH)
                    .multiline(true)  // 多行：长链接可完整看到
                    .placeholder("https://… 或 magnet:…")
                    .value(g_urlText)
                    .theme(theme.components)
                    .onChange([](const std::string& value) { g_urlText = value; })
                    .onEnter([] { if (addDownload()) g_addOpen = false; })
                    .build();

                // ---- 分片数（0=配置默认；仅 aria2 生效）----
                components::text(ui, "add.conn.label")
                    .position(labelX, splitY)
                    .size(labelW, S(28.0f))
                    .text("分片数")
                    .fontSize(S(12.0f))
                    .lineHeight(S(28.0f))
                    .color(theme.metaText)
                    .build();
                components::input(ui, "add.conn")
                    .position(inputX, splitY - S(2.0f))
                    .size(S(84.0f), S(28.0f))
                    .placeholder("0=默认")
                    .value(g_addConnectionsText)
                    .theme(theme.components)
                    .onChange([](const std::string& value) { g_addConnectionsText = value; })
                    .onEnter([] { if (addDownload()) g_addOpen = false; })
                    .build();

                // ---- 优先级：默认/高/中/低（独立一行，仅 aria2 生效）----
                // 注意 id 不能用 "add.priority.label"——buildListPicker(id="add.priority")
                // 内部字段标签正好也是该 id，会互相覆盖。
                components::text(ui, "add.priority.title")
                    .position(labelX, priorityY - S(2.0f))
                    .size(labelW, S(28.0f))
                    .text("优先级")
                    .fontSize(S(12.0f))
                    .lineHeight(S(28.0f))
                    .verticalAlign(core::VerticalAlign::Center)
                    .color(theme.metaText)
                    .build();
                ui.stack("add.priority.wrap")
                    .position(inputX, priorityY - S(2.0f))
                    .size(S(82.0f), S(28.0f))
                    .zIndex(32)
                    .content([&] {
                        static const char* kPriorityLabels[] = {"默认", "高", "中", "低"};
                        // 默认选"默认"（0），字段直接显示当前值。
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
