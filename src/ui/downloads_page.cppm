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

// 镜像源管理弹窗（定义在文件末尾；drawDownloadsPage 调用它）。
void drawMirrorDialog(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme);

// ===================== 下载页 =====================
// 布局：左侧是任务列表子侧边栏，右侧是输入栏 + 卡片任务列表 + 翻页控件组。
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
                                 return taskDisplayName(a) < taskDisplayName(b);
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
    }
    const int totalCount = static_cast<int>(filtered.size());
    const int totalPages = std::max(1, (totalCount + g_pageSize - 1) / g_pageSize);
    g_page = std::clamp(g_page, 1, totalPages);
    const int start = (g_page - 1) * g_pageSize;
    const int end = std::min(totalCount, start + g_pageSize);

    // ---- 布局尺寸（岛屿卡片风：内容区两张浮岛卡片）----
    // 图标栏占满左缘（整高竖条、不套卡片），任务列表子侧边栏 + 内容大卡两张浮岛
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
    const float listTop = toolY + kInputHeight + 8.0f;
    const float listHeight = std::max(0.0f, pagerY - listTop - 4.0f);
    const float listX = contentX + pad;
    const float listW = contentW - 2.0f * pad;

    drawPanel(ui, "dl.content.panel", contentX, islandTop, contentW, islandH, theme);

    // ---- 任务列表子侧边栏：所有 / 下载中 / 已完成（独立岛卡片）----
    ui.stack("sub.filter")
        .position(subX, islandTop)
        .size(kSubSidebarWidth, islandH)
        .zIndex(4)
        .content([&] {
            drawPanel(ui, "sub.filter.bg", 0, 0, kSubSidebarWidth, islandH, theme);

            components::text(ui, "sub.filter.label")
                .position(9.0f, 10.0f)
                .size(kSubSidebarWidth - 18.0f, 18.0f)
                .text("任务列表")
                .fontSize(13.0f)
                .lineHeight(18.0f)
                .color(theme.titleText)
                .build();

            const float itemW = kSubSidebarWidth - 12.0f;
            float itemY = 28.0f;
            // 各筛选的任务数：所有 = 总数；下载中/已完成与列表筛选同一口径
            // （stateMatches），基于本帧 snapshot 统计。
            int activeCount = 0, doneCount = 0;
            for (const auto& task : tasks) {
                if (stateMatches(Filter::Active, task.state)) ++activeCount;
                else if (stateMatches(Filter::Done, task.state)) ++doneCount;
            }
            drawSidebarItem(ui, "filter.all", 6.0f, itemY, itemW, 22.0f,
                            "所有", 0xF03A, g_filter == Filter::All, theme,
                            [] { g_filter = Filter::All; g_page = 1; },
                            static_cast<int>(tasks.size()));
            itemY += 27.0f;
            drawSidebarItem(ui, "filter.active", 6.0f, itemY, itemW, 22.0f,
                            "下载中", 0xF019, g_filter == Filter::Active, theme,
                            [] { g_filter = Filter::Active; g_page = 1; },
                            activeCount);
            itemY += 27.0f;
            drawSidebarItem(ui, "filter.done", 6.0f, itemY, itemW, 22.0f,
                            "已完成", 0xF00C, g_filter == Filter::Done, theme,
                            [] { g_filter = Filter::Done; g_page = 1; },
                            doneCount);
        })
        .build();

    // ---- 顶部工具栏（右对齐，收在内容大卡内）：全部暂停 / 全部继续 / 排序 / 添加 ----
    const float toolW = 28.0f;
    const float toolGap = 4.0f;
    const float toolRight = contentX + contentW - pad;
    const float addX = toolRight - toolW;
    const float sortX = addX - toolGap - toolW;
    const float startAllX = sortX - toolGap - toolW;
    const float pauseAllX = startAllX - toolGap - toolW;

    // 排序选择器：图标按钮 + 向下弹出列表（buildListPicker 不设位置，
    // 由外层 stack 绝对定位）。
    static const char* kSortLabels[] = {"最新在前", "状态优先", "文件名", "大小", "进度"};
    ui.stack("tool.sort.wrap")
        .position(sortX, toolY)
        .size(toolW, toolW)
        .zIndex(30)
        .content([&] {
            buildListPicker(ui, "tool.sort", toolW, toolW, theme,
                            g_sortOpen, kSortLabels, 5,
                            static_cast<int>(g_sort), false,
                            PickerField::Icon,
                            [](int i) {
                                g_sort = static_cast<SortMode>(i);
                                g_page = 1;
                            },
                            96.0f);  // 字段是图标，弹层加宽容纳文字
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
                              // 的默认下载目录；其余清空。若剪贴板是可下载源则预填 URL。
                              g_urlText.clear();
                              g_addTorrentPath.clear();
                              g_addMirror = false;
                              g_addTab = AddTab::Direct;
                              const std::string clip = trimText(getClipboardText());
                              if (!clip.empty() && isDownloadableSource(clip)) {
                                  g_urlText = clip;
                              }
                              g_addConnectionsText = std::to_string(cfg::aria2Config().split);
                              g_addRenameText.clear();
                              g_addDirText = cfg::downloadDir().string();
                              g_addOpen = true;
                          });

    // ---- 任务列表：卡片式布局（名称/进度/信息纵向排布）----
    // 无任务时也画空 scrollView，不显示引导文案（界面更简洁，用户自会用）。
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

    // ---- 翻页控件组：◀ 页码 ▶ [数字/页]，整组收进一张小卡片 ----
    // 简洁版：中间只显示当前页码数字（不再显示"第 X / Y 页"），分页大小是
    // 无边框的纯文本"数字/页"。整组控件包在一张圆角小卡里（岛内小岛）。
    constexpr float kChevWidth = 20.0f;  // 正方形 → components::button 默认 radius 钳成纯圆
    constexpr float kPageLabelWidth = 28.0f;   // 仅当前页码
    constexpr float kPageSizeWidth = 52.0f;    // "数字/页" 纯文本
    constexpr float kPagerGap = 4.0f;
    constexpr float kPagerPadH = 10.0f;        // 卡片水平内边距
    constexpr float kPagerPadV = 3.0f;         // 卡片垂直内边距
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
                .radius(8.0f)
                .border(1.0f, components::theme::withOpacity(theme.components.border, 0.55f))
                .shadow(8.0f, 2.0f,
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
                        .size(kChevWidth, kChevWidth)
                        .icon(0xF053)  // chevron-left
                        .text("")
                        .iconSize(11.0f)
                        .theme(theme.components, false)
                        .disabled(g_page <= 1)
                        .onClick([] { if (g_page > 1) --g_page; })
                        .build();

                    components::text(ui, "pager.label")
                        .size(kPageLabelWidth, kPagerHeight)
                        .text(std::format("{}", g_page))
                        .fontSize(11.0f)
                        .lineHeight(kPagerHeight)
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .verticalAlign(core::VerticalAlign::Center)
                        .color(theme.metaText)
                        .build();

                    components::button(ui, "pager.next")
                        .size(kChevWidth, kChevWidth)
                        .icon(0xF054)  // chevron-right
                        .text("")
                        .iconSize(11.0f)
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
            .position(listX, pagerY - 24.0f)
            .size(listW, 18.0f)
            .text(g_statusMessage)
            .fontSize(12.0f)
            .lineHeight(18.0f)
            .color(theme.statusText)
            .build();
    }

    // ---- 添加下载弹窗（模态）：链接 + 每任务高级选项 ----
    if (g_addOpen) {
        const float dlgW = 320.0f;
        const float dlgH = 316.0f;
        const float dlgX = (screen.width - dlgW) * 0.5f;
        const float dlgY = (screen.height - dlgH) * 0.5f;
        const float labelX = 16.0f;
        const float labelW = 56.0f;
        const float inputX = 74.0f;
        const float tabY = 34.0f;      // 顶部切换（直链下载 / 种子）
        const float tabW = 88.0f;
        const float tabH = 26.0f;
        const float urlY = 66.0f;      // 直链：URL 多行输入（磁力 magnet: 也在这）
        const float urlH = 48.0f;
        const float splitY = 122.0f;   // 直链：分片数
        const float renameY = 156.0f;  // 直链：重命名
        const float dirY = 190.0f;     // 直链：下载目录
        const float mirrorY = 224.0f;  // 直链：镜像多源开关
        const float btnY = 276.0f;
        const float torY = 66.0f;      // 种子 tab：种子文件行
        const float torDirY = 102.0f;  // 种子 tab：下载目录行

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
                    .radius(10.0f)
                    .border(1.0f,
                            components::theme::withOpacity(
                                theme.components.border, 0.6f))
                    .onClick([] {})  // 吞掉弹窗内部空白点击，避免穿透到遮罩关闭弹窗
                    .build();

                components::text(ui, "add.dialog.title")
                    .position(labelX, 12.0f)
                    .size(dlgW - 32.0f, 20.0f)
                    .text("添加下载")
                    .fontSize(14.0f)
                    .lineHeight(20.0f)
                    .color(theme.titleText)
                    .build();

                // 顶部切换：直链下载 / 种子（种子与直连流程不同，字段各自独立）。
                components::button(ui, "add.tab.direct")
                    .position(labelX, tabY)
                    .size(tabW, tabH)
                    .text("直链下载")
                    .fontSize(12.0f)
                    .theme(theme.components, g_addTab == AddTab::Direct)
                    .onClick([] { g_addTab = AddTab::Direct; })
                    .build();
                components::button(ui, "add.tab.torrent")
                    .position(labelX + tabW + 8.0f, tabY)
                    .size(tabW, tabH)
                    .text("种子")
                    .fontSize(12.0f)
                    .theme(theme.components, g_addTab == AddTab::Torrent)
                    .onClick([] { g_addTab = AddTab::Torrent; })
                    .build();

                if (g_addTab == AddTab::Direct) {
                    // URL 多行（磁力 magnet: 也在这里填；直链默认不设 out 让 aria2
                    // 从 Content-Disposition 解析真实文件名）。
                    components::input(ui, "add.url")
                        .position(labelX, urlY)
                        .size(dlgW - 32.0f, urlH)
                        .multiline(true)  // 多行：长链接完整可见，滚轮可滚动
                        .placeholder("https://… / magnet:… / ftp://…")
                        .value(g_urlText)
                        .fontFamily("")  // 用应用字体（Noto Sans SC），不要 eui 默认的 Microsoft YaHei
                        .theme(theme.components)
                        .onChange([](const std::string& value) { g_urlText = value; })
                        .onEnter([] { if (addDownload()) g_addOpen = false; })
                        .build();

                    // ---- 镜像多源：勾选时 URL 框多行 → 首行为主 URL，其余为镜像源
                    //      （aria2 多源并发下载同一文件，源挂自动切换；实验性）----
                    components::text(ui, "add.mirror.label")
                        .position(labelX, mirrorY)
                        .size(dlgW - 16.0f - 60.0f, 28.0f)
                        .text("多行URL合并为镜像")
                        .fontSize(12.0f)
                        .lineHeight(28.0f)
                        .color(theme.metaText)
                        .build();
                    components::button(ui, "add.mirror.toggle")
                        .position(dlgW - 16.0f - 48.0f, mirrorY)
                        .size(48.0f, 26.0f)
                        .text(g_addMirror ? "开" : "关")
                        .fontSize(11.0f)
                        .theme(theme.components, g_addMirror)
                        .onClick([] { g_addMirror = !g_addMirror; })
                        .build();

                    // ---- 分片数（0=配置默认；仅 aria2 生效）----
                    components::text(ui, "add.conn.label")
                        .position(labelX, splitY)
                        .size(labelW, 28.0f)
                        .text("分片数")
                        .fontSize(12.0f)
                        .lineHeight(28.0f)
                        .color(theme.metaText)
                        .build();
                    buildNumberStepper(ui, "add.conn", inputX, splitY - 2.0f,
                                       84.0f, 28.0f, theme, g_addConnectionsText,
                                       [](const std::string& v) { g_addConnectionsText = v; },
                                       0, 64, 1);  // 0=用配置默认

                    // ---- 重命名（可选，留空=URL 文件名）----
                    components::text(ui, "add.rename.label")
                        .position(labelX, renameY)
                        .size(labelW, 28.0f)
                        .text("重命名")
                        .fontSize(12.0f)
                        .lineHeight(28.0f)
                        .color(theme.metaText)
                        .build();
                    components::input(ui, "add.rename")
                        .position(inputX, renameY - 2.0f)
                        .size(dlgW - inputX - 16.0f, 28.0f)
                        .placeholder("可选")
                        .value(g_addRenameText)
                        .fontFamily("")  // 用应用字体（Noto Sans SC），不要 eui 默认的 Microsoft YaHei
                        .theme(theme.components)
                        .onChange([](const std::string& value) { g_addRenameText = value; })
                        .onEnter([] { if (addDownload()) g_addOpen = false; })
                        .build();

                    // ---- 下载目录（留空=全局）----
                    components::text(ui, "add.dir.label")
                        .position(labelX, dirY)
                        .size(labelW, 28.0f)
                        .text("下载目录")
                        .fontSize(12.0f)
                        .lineHeight(28.0f)
                        .color(theme.metaText)
                        .build();
                    components::input(ui, "add.dir")
                        .position(inputX, dirY - 2.0f)
                        .size(dlgW - inputX - 16.0f - 60.0f - 8.0f, 28.0f)
                        .placeholder("留空=全局")
                        .value(g_addDirText)
                        .fontFamily("")  // 用应用字体（Noto Sans SC），不要 eui 默认的 Microsoft YaHei
                        .theme(theme.components)
                        .onChange([](const std::string& value) { g_addDirText = value; })
                        .onEnter([] { if (addDownload()) g_addOpen = false; })
                        .build();
                    components::button(ui, "add.dir.browse")
                        .position(inputX + (dlgW - inputX - 16.0f - 60.0f - 8.0f) +
                                      8.0f,
                                  dirY - 2.0f)
                        .size(60.0f, 26.0f)
                        .text("浏览…")
                        .fontSize(12.0f)
                        .theme(theme.components, false)
                        .onClick([] {
                            const auto picked = pickDownloadFolder();
                            if (!picked.empty()) {
                                g_addDirText = picked.string();
                            }
                        })
                        .build();
                } else {
                    // ---- 种子 tab：独立小面板（无连接数/重命名——对 BT 无意义；
                    //      内容名由种子决定，README 已提示）----
                    components::text(ui, "add.torrent.label")
                        .position(labelX, torY)
                        .size(labelW, 28.0f)
                        .text("种子文件")
                        .fontSize(12.0f)
                        .lineHeight(28.0f)
                        .color(theme.metaText)
                        .build();
                    components::input(ui, "add.torrent")
                        .position(inputX, torY - 2.0f)
                        .size(dlgW - inputX - 16.0f - 60.0f - 8.0f, 28.0f)
                        .placeholder("选择本地 .torrent")
                        .value(g_addTorrentPath)
                        .fontFamily("")  // 用应用字体（Noto Sans SC），不要 eui 默认的 Microsoft YaHei
                        .theme(theme.components)
                        .onChange([](const std::string& value) { g_addTorrentPath = value; })
                        .build();
                    components::button(ui, "add.torrent.browse")
                        .position(inputX + (dlgW - inputX - 16.0f - 60.0f - 8.0f) + 8.0f,
                                  torY - 2.0f)
                        .size(60.0f, 26.0f)
                        .text("浏览…")
                        .fontSize(12.0f)
                        .theme(theme.components, false)
                        .onClick([] {
                            const auto picked = pickTorrentFile();
                            if (!picked.empty()) g_addTorrentPath = picked.string();
                        })
                        .build();

                    // 下载目录（种子内容名不由文件名决定，明确提示填写）
                    components::text(ui, "add.torrent.dir.label")
                        .position(labelX, torDirY)
                        .size(labelW, 28.0f)
                        .text("下载目录")
                        .fontSize(12.0f)
                        .lineHeight(28.0f)
                        .color(theme.metaText)
                        .build();
                    components::input(ui, "add.torrent.dir")
                        .position(inputX, torDirY - 2.0f)
                        .size(dlgW - inputX - 16.0f - 60.0f - 8.0f, 28.0f)
                        .placeholder("留空=全局")
                        .value(g_addDirText)
                        .fontFamily("")  // 用应用字体（Noto Sans SC），不要 eui 默认的 Microsoft YaHei
                        .theme(theme.components)
                        .onChange([](const std::string& value) { g_addDirText = value; })
                        .build();
                    components::button(ui, "add.torrent.dir.browse")
                        .position(inputX + (dlgW - inputX - 16.0f - 60.0f - 8.0f) + 8.0f,
                                  torDirY - 2.0f)
                        .size(60.0f, 26.0f)
                        .text("浏览…")
                        .fontSize(12.0f)
                        .theme(theme.components, false)
                        .onClick([] {
                            const auto picked = pickDownloadFolder();
                            if (!picked.empty()) g_addDirText = picked.string();
                        })
                        .build();

                    components::text(ui, "add.torrent.hint")
                        .position(labelX, 140.0f)
                        .size(dlgW - 32.0f, 36.0f)
                        .text("下载内容名由种子决定；磁力链接请切到「直链下载」粘贴")
                        .fontSize(11.0f)
                        .lineHeight(16.0f)
                        .color(theme.metaText)
                        .build();
                }

                components::button(ui, "add.cancel")
                    .position(dlgW - 16.0f - 76.0f - 8.0f - 76.0f, btnY)
                    .size(76.0f, 26.0f)
                    .text("取消")
                    .fontSize(12.0f)
                    .theme(theme.components, false)
                    .onClick([] { g_addOpen = false; })
                    .build();

                components::button(ui, "add.submit")
                    .position(dlgW - 16.0f - 76.0f, btnY)
                    .size(76.0f, 26.0f)
                    .text("提交")
                    .fontSize(12.0f)
                    .theme(theme.components, true)
                    .onClick([] { if (addDownload()) g_addOpen = false; })
                    .build();
            })
            .build();
    }

    // ---- 删除确认弹窗（已完成任务）：复选框决定是否同时删除源文件 ----
    // 只有已完成任务会走到这里（requestDelete 对未完成任务直接删记录）。勾选
    // 删除源文件时走系统回收站（默认可恢复），绝不永久删除。
    if (g_pendingDelete.has_value()) {
        const dl::TaskView& delTask = *g_pendingDelete;
        const std::string delName = taskDisplayName(delTask);
        const float dlgW = 340.0f;
        const float dlgH = 136.0f;
        const float dlgX = (screen.width - dlgW) * 0.5f;
        const float dlgY = (screen.height - dlgH) * 0.5f;

        // 半透明遮罩，点击空白处关闭（=取消）。
        ui.rect("del.backdrop")
            .position(0, 0)
            .size(screen.width, screen.height)
            .zIndex(100)
            .color({0.0f, 0.0f, 0.0f, 0.45f})
            .onClick([] { g_pendingDelete.reset(); })
            .build();

        ui.stack("del.dialog")
            .position(dlgX, dlgY)
            .size(dlgW, dlgH)
            .zIndex(101)
            .content([&] {
                ui.rect("del.dialog.bg")
                    .position(0, 0)
                    .size(dlgW, dlgH)
                    .color(theme.components.surface)
                    .radius(10.0f)
                    .border(1.0f,
                            components::theme::withOpacity(
                                theme.components.border, 0.6f))
                    .onClick([] {})  // 吞掉弹窗内部空白点击，避免穿透到遮罩关闭弹窗
                    .build();

                components::text(ui, "del.title")
                    .position(16.0f, 12.0f)
                    .size(dlgW - 32.0f, 20.0f)
                    .text("删除任务")
                    .fontSize(14.0f)
                    .lineHeight(20.0f)
                    .color(theme.titleText)
                    .build();

                components::text(ui, "del.name")
                    .position(16.0f, 34.0f)
                    .size(dlgW - 32.0f, 18.0f)
                    .text("删除「" + delName + "」？")
                    .fontSize(12.0f)
                    .lineHeight(18.0f)
                    .maxWidth(dlgW - 32.0f)
                    .color(theme.nameText)
                    .build();

                // 复选框：是否同时删除源文件。默认勾选（移到回收站，可恢复）。
                // checkbox builder 无定位，外包 stack 定位。
                ui.stack("del.checkbox.wrap")
                    .position(16.0f, 54.0f)
                    .size(dlgW - 32.0f, 20.0f)
                    .content([&] {
                        components::checkbox(ui, "del.checkbox")
                            .size(dlgW - 32.0f, 20.0f)
                            .checked(g_deleteIncludeFiles)
                            .text("同时删除源文件")
                            .fontSize(11.0f)
                            .theme(theme.components)
                            .onChange([](bool v) { g_deleteIncludeFiles = v; })
                            .build();
                    })
                    .build();

                // 底部按钮行：取消 | 删除（主按钮）。
                const float btnH = 26.0f;
                const float btnY = dlgH - 36.0f;
                const float wCancel = 64.0f;
                const float wDel = 76.0f;
                const float gap = 8.0f;
                const float delX = dlgW - 16.0f - wDel;
                const float cancelX = delX - gap - wCancel;

                components::button(ui, "del.cancel")
                    .position(cancelX, btnY)
                    .size(wCancel, btnH)
                    .text("取消")
                    .fontSize(11.0f)
                    .theme(theme.components, false)
                    .onClick([] { g_pendingDelete.reset(); })
                    .build();

                components::button(ui, "del.confirm")
                    .position(delX, btnY)
                    .size(wDel, btnH)
                    .text("删除")
                    .fontSize(11.0f)
                    .theme(theme.components, true)
                    .onClick([] {
                        const dl::TaskView task = *g_pendingDelete;
                        g_pendingDelete.reset();
                        deleteTaskRecord(task);
                        if (g_deleteIncludeFiles) {
                            std::error_code ec;
                            if (std::filesystem::exists(task.destPath, ec)) {
                                if (moveToTrash(task.destPath)) {
                                    showStatus("已删除记录，源文件已移到回收站");
                                } else {
                                    showStatus("已删除记录（移入回收站失败，文件保留）");
                                }
                            } else {
                                showStatus("已删除记录（源文件不存在）");
                            }
                        } else {
                            showStatus("已删除记录，保留源文件");
                        }
                    })
                    .build();
            })
            .build();
    }

    // ---- 镜像源管理弹窗 ----
    if (g_mirrorOpen) {
        drawMirrorDialog(ui, screen, theme);
    }
}

// 镜像源管理弹窗：查看实时源列表（aria2 uris 去重）+ 移除坏源 + 添加新源。
// 仅活动任务可增删（aria2.changeUri 对 active/waiting/paused 有效）。
void drawMirrorDialog(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    const float dlgW = 380.0f;
    const float dlgH = 340.0f;
    const float dlgX = (screen.width - dlgW) * 0.5f;
    const float dlgY = (screen.height - dlgH) * 0.5f;

    ui.rect("mirror.backdrop")
        .position(0, 0)
        .size(screen.width, screen.height)
        .zIndex(100)
        .color({0.0f, 0.0f, 0.0f, 0.45f})
        .onClick([] { g_mirrorOpen = false; })
        .build();

    ui.stack("mirror.dialog")
        .position(dlgX, dlgY)
        .size(dlgW, dlgH)
        .zIndex(101)
        .content([&] {
            ui.rect("mirror.dialog.bg")
                .position(0, 0)
                .size(dlgW, dlgH)
                .color(theme.components.surface)
                .radius(10.0f)
                .border(1.0f,
                        components::theme::withOpacity(
                            theme.components.border, 0.6f))
                .onClick([] {})  // 吞掉内部空白点击
                .build();

            components::text(ui, "mirror.title")
                .position(16.0f, 12.0f)
                .size(dlgW - 32.0f, 22.0f)
                .text("镜像源管理")
                .fontSize(14.0f)
                .lineHeight(22.0f)
                .color(theme.titleText)
                .build();

            // 当前任务（实时 snapshot；任务可能已被删除）。
            const auto tasks = g_manager->snapshot();
            const dl::TaskView* task = nullptr;
            for (const auto& t : tasks) {
                if (t.id == g_mirrorTaskId) {
                    task = &t;
                    break;
                }
            }
            const bool active = task &&
                (task->state == dl::State::Queued ||
                 task->state == dl::State::Downloading ||
                 task->state == dl::State::Paused);

            components::text(ui, "mirror.task")
                .position(16.0f, 36.0f)
                .size(dlgW - 32.0f, 18.0f)
                .text(task ? taskDisplayName(*task) : "任务已结束")
                .fontSize(11.0f)
                .lineHeight(18.0f)
                .color(theme.metaText)
                .build();

            // ---- 源列表（实时 uris 去重）----
            components::scrollView(ui, "mirror.list")
                .position(16.0f, 58.0f)
                .size(dlgW - 32.0f, 182.0f)
                .gap(4.0f)
                .theme(theme.components)
                .content([&](eui::Ui& sv, float w, float) {
                    const auto drawSource = [&](int idx, const std::string& uri,
                                                const std::string& status) {
                        const std::string rowId = "mirror.row." + std::to_string(idx);
                        sv.stack(rowId)
                            .width(w)
                            .height(26.0f)
                            .content([&] {
                                std::string shown = uri;
                                if (shown.size() > 40) shown = shown.substr(0, 40) + "…";
                                components::text(sv, rowId + ".uri")
                                    .position(6.0f, 0)
                                    .size(w - 96.0f, 26.0f)
                                    .text(shown)
                                    .fontSize(10.0f)
                                    .lineHeight(26.0f)
                                    .maxWidth(w - 96.0f)
                                    .color(theme.nameText)
                                    .build();
                                const char* st = status == "used" ? "在用"
                                                  : (status == "error" ? "失败" : "备用");
                                const eui::Color stc = status == "used" ? theme.done
                                                       : (status == "error" ? theme.failed
                                                                          : theme.metaText);
                                components::text(sv, rowId + ".st")
                                    .position(w - 88.0f, 0)
                                    .size(40.0f, 26.0f)
                                    .text(st)
                                    .fontSize(10.0f)
                                    .lineHeight(26.0f)
                                    .color(stc)
                                    .build();
                                if (active) {
                                    components::button(sv, rowId + ".rm")
                                        .position(w - 44.0f, 1.0f)
                                        .size(38.0f, 24.0f)
                                        .text("移除")
                                        .fontSize(10.0f)
                                        .theme(theme.components, false)
                                        .onClick([id = task->id, uri] {
                                            if (g_manager->removeMirror(id, uri)) {
                                                showStatus("已移除镜像源");
                                            } else {
                                                showStatus("移除失败（任务非活动或源不存在）");
                                            }
                                        })
                                        .build();
                                }
                            })
                            .build();
                    };

                    if (!task || task->mirrors.empty()) {
                        components::text(sv, "mirror.empty")
                            .position(6.0f, 0)
                            .size(w - 12.0f, 26.0f)
                            .text(task ? "无镜像源（可在下方添加）" : "任务无镜像源")
                            .fontSize(10.0f)
                            .lineHeight(26.0f)
                            .color(theme.metaText)
                            .build();
                    } else {
                        int i = 0;
                        for (const auto& m : task->mirrors) {
                            drawSource(i++, m.uri, m.status);
                        }
                    }
                })
                .build();

            // ---- 添加行 ----
            components::text(ui, "mirror.add.label")
                .position(16.0f, 250.0f)
                .size(60.0f, 26.0f)
                .text("添加源")
                .fontSize(11.0f)
                .lineHeight(26.0f)
                .color(theme.metaText)
                .build();
            components::input(ui, "mirror.add.input")
                .position(74.0f, 248.0f)
                .size(dlgW - 74.0f - 64.0f - 16.0f, 26.0f)
                .placeholder(active ? "镜像源 URL" : "仅活动任务可添加")
                .value(g_mirrorAddText)
                .fontFamily("")
                .theme(theme.components)
                .onChange([](const std::string& v) { g_mirrorAddText = v; })
                .build();
            if (active) {
                components::button(ui, "mirror.add.btn")
                    .position(dlgW - 16.0f - 56.0f, 248.0f)
                    .size(56.0f, 26.0f)
                    .text("添加")
                    .fontSize(11.0f)
                    .theme(theme.components, true)
                    .onClick([] {
                        std::string url = trimText(g_mirrorAddText);
                        if (url.empty()) {
                            showStatus("请输入镜像源地址");
                            return;
                        }
                        if (!isDownloadableSource(url) || url.starts_with("magnet:")) {
                            showStatus("镜像源须为 http(s)/ftp(s)/sftp 链接");
                            return;
                        }
                        if (g_manager->addMirror(g_mirrorTaskId, url)) {
                            showStatus("已添加镜像源");
                            g_mirrorAddText.clear();
                        } else {
                            showStatus("添加失败（任务非活动或地址无效）");
                        }
                    })
                    .build();
            }

            components::button(ui, "mirror.close")
                .position((dlgW - 76.0f) * 0.5f, dlgH - 34.0f)
                .size(76.0f, 26.0f)
                .text("关闭")
                .fontSize(12.0f)
                .theme(theme.components, true)
                .onClick([] { g_mirrorOpen = false; })
                .build();
        })
        .build();
}
