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
    static const char* kSortLabels[] = {"最新在前", "状态优先", "文件名", "大小", "进度"};
    ui.stack("tool.sort.wrap")
        .position(sortX, inputY)
        .size(toolW, kInputHeight)
        .zIndex(30)
        .content([&] {
            buildListPicker(ui, "tool.sort", toolW, kInputHeight, theme,
                            g_sortOpen, kSortLabels, 5,
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
            g_urlText.clear();
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

    // ---- 添加下载弹窗（模态）：链接输入 + 提交/取消 ----
    if (g_addOpen) {
        const float dlgW = S(280.0f);
        const float dlgH = S(166.0f);
        const float dlgX = (screen.width - dlgW) * 0.5f;
        const float dlgY = (screen.height - dlgH) * 0.5f;

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
                    .position(S(16.0f), S(12.0f))
                    .size(dlgW - S(32.0f), S(20.0f))
                    .text("添加下载")
                    .fontSize(S(14.0f))
                    .lineHeight(S(20.0f))
                    .color(theme.titleText)
                    .build();

                components::input(ui, "add.url")
                    .position(S(16.0f), S(40.0f))
                    .size(dlgW - S(32.0f), S(28.0f))
                    .placeholder("https://example.com/file.zip")
                    .value(g_urlText)
                    .theme(theme.components)
                    .onChange([](const std::string& value) { g_urlText = value; })
                    .onEnter([] { if (addDownload()) g_addOpen = false; })
                    .build();

                // 每任务连接数：默认=配置 split 值；填 0 或清空=配置默认。
                components::text(ui, "add.conn.label")
                    .position(S(16.0f), S(76.0f))
                    .size(S(70.0f), S(28.0f))
                    .text("连接数")
                    .fontSize(S(12.0f))
                    .lineHeight(S(28.0f))
                    .color(theme.metaText)
                    .build();
                components::input(ui, "add.conn")
                    .position(S(88.0f), S(74.0f))
                    .size(dlgW - S(104.0f), S(28.0f))
                    .placeholder("0=默认")
                    .value(g_addConnectionsText)
                    .theme(theme.components)
                    .onChange([](const std::string& value) { g_addConnectionsText = value; })
                    .onEnter([] { if (addDownload()) g_addOpen = false; })
                    .build();

                components::button(ui, "add.cancel")
                    .position(dlgW - S(16.0f) - S(76.0f) - S(8.0f) - S(76.0f), S(124.0f))
                    .size(S(76.0f), S(26.0f))
                    .text("取消")
                    .fontSize(S(12.0f))
                    .theme(theme.components, false)
                    .onClick([] { g_addOpen = false; })
                    .build();

                components::button(ui, "add.submit")
                    .position(dlgW - S(16.0f) - S(76.0f), S(124.0f))
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

    // ---- 主题设置：跟随系统 / 深色 / 浅色 ----
    const float labelW = S(90.0f);
    float nextY = S(58.0f);
    components::text(ui, "settings.theme.label")
        .position(infoX, nextY)
        .size(labelW, S(22.0f))
        .text("主题")
        .fontSize(S(12.0f))
        .lineHeight(S(22.0f))
        .color(theme.metaText)
        .build();

    struct ThemeChoice { const char* label; cfg::ThemeMode mode; };
    static const ThemeChoice kThemeChoices[] = {
        {"跟随系统", cfg::ThemeMode::System},
        {"深色", cfg::ThemeMode::Dark},
        {"浅色", cfg::ThemeMode::Light},
    };
    float themeBtnX = infoX + labelW + S(8.0f);
    for (std::size_t i = 0; i < 3; ++i) {
        const bool active = g_pendingTheme == kThemeChoices[i].mode;
        components::button(ui, std::format("settings.theme.{}", i))
            .position(themeBtnX, nextY - S(1.0f))
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
        themeBtnX += S(84.0f);
    }
    nextY += S(32.0f);

    // ---- 下载引擎：tinyhttps / aria2-next ----
    components::text(ui, "settings.engine.label")
        .position(infoX, nextY)
        .size(labelW, S(22.0f))
        .text("下载引擎")
        .fontSize(S(12.0f))
        .lineHeight(S(22.0f))
        .color(theme.metaText)
        .build();

    struct EngineChoice { const char* label; cfg::EngineChoice choice; };
    static const EngineChoice kEngineChoices[] = {
        {"tinyhttps", cfg::EngineChoice::TinyHttps},
        {"aria2-next", cfg::EngineChoice::Aria2Next},
    };
    float engineBtnX = infoX + labelW + S(8.0f);
    for (std::size_t i = 0; i < 2; ++i) {
        const bool active = g_pendingEngine == kEngineChoices[i].choice;
        components::button(ui, std::format("settings.engine.{}", i))
            .position(engineBtnX, nextY - S(1.0f))
            .size(S(90.0f), S(24.0f))
            .text(kEngineChoices[i].label)
            .fontSize(S(12.0f))
            .theme(theme.components, active)
            .onClick([choice = kEngineChoices[i].choice] {
                g_pendingEngine = choice;  // 「保存」时生效
            })
            .build();
        engineBtnX += S(98.0f);
    }
    nextY += S(32.0f);

    // ---- 下载路径：输入框 + 系统文件夹选择器 ----
    components::text(ui, "settings.path.label")
        .position(infoX, nextY)
        .size(labelW, S(22.0f))
        .text("下载路径")
        .fontSize(S(12.0f))
        .lineHeight(S(22.0f))
        .color(theme.metaText)
        .build();

    const float pathInputW = std::max(
        S(160.0f), contentWidth - S(16.0f) - labelW - S(8.0f) - S(60.0f));
    components::input(ui, "settings.path.input")
        .position(infoX + labelW, nextY - S(2.0f))
        .size(pathInputW, S(26.0f))
        .placeholder("下载保存目录")
        .value(g_downloadDirText)
        .theme(theme.components)
        .onChange([](const std::string& value) { g_downloadDirText = value; })
        .build();

    components::button(ui, "settings.path.browse")
        .position(infoX + labelW + pathInputW + S(8.0f), nextY - S(2.0f))
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
    nextY += S(34.0f);

    // ---- aria2 参数（仅 aria2-next 引擎相关）----
    if (g_pendingEngine == cfg::EngineChoice::Aria2Next) {
        nextY += S(2.0f);
        components::text(ui, "settings.aria2.header")
            .position(infoX, nextY)
            .size(contentWidth - S(16.0f), S(18.0f))
            .text("aria2 参数")
            .fontSize(S(11.0f))
            .lineHeight(S(18.0f))
            .color(theme.statusText)
            .build();
        nextY += S(22.0f);

        auto drawParamField =
            [&](const std::string& id, const char* label,
                float x, float y, float inputW, const std::string& value,
                const std::function<void(const std::string&)>& onChange) {
                components::text(ui, id + ".label")
                    .position(x, y)
                    .size(S(84.0f), S(22.0f))
                    .text(label)
                    .fontSize(S(11.0f))
                    .lineHeight(S(22.0f))
                    .color(theme.metaText)
                    .build();
                components::input(ui, id + ".input")
                    .position(x + S(86.0f), y - S(2.0f))
                    .size(inputW, S(26.0f))
                    .value(value)
                    .theme(theme.components)
                    .onChange(onChange)
                    .build();
            };

        const float col2X = infoX + S(86.0f) + S(90.0f) + S(20.0f);  // 第二列
        drawParamField("settings.aria2.split", "分片数",
                       infoX, nextY, S(90.0f), g_aria2SplitText,
                       [](const std::string& v) { g_aria2SplitText = v; });
        drawParamField("settings.aria2.conn", "每服务器连接",
                       col2X, nextY, S(90.0f), g_aria2ConnText,
                       [](const std::string& v) { g_aria2ConnText = v; });
        nextY += S(28.0f);
        drawParamField("settings.aria2.minsplit", "最小分片",
                       infoX, nextY, S(90.0f), g_aria2MinSplitText,
                       [](const std::string& v) { g_aria2MinSplitText = v; });
        drawParamField("settings.aria2.limit", "限速KB/s",
                       col2X, nextY, S(90.0f), g_aria2LimitText,
                       [](const std::string& v) { g_aria2LimitText = v; });
        nextY += S(32.0f);
    }

    // ---- 操作：恢复默认路径 / 保存全部设置 / 放弃修改 ----
    const float actionRowY = nextY;
    components::button(ui, "settings.path.reset")
        .position(infoX + labelW, actionRowY)
        .size(S(76.0f), S(26.0f))
        .text("恢复默认")
        .fontSize(S(12.0f))
        .theme(theme.components, false)
        .onClick([] {
            g_downloadDirText = cfg::defaultDownloadDir().string();
        })
        .build();

    components::button(ui, "settings.save")
        .position(infoX + labelW + S(8.0f) + S(76.0f), actionRowY)
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

            // 下载引擎：若变化且无进行中任务，立即切换生效。
            const bool engineChanged = cfg::engine() != g_pendingEngine;
            cfg::setEngine(g_pendingEngine);
            if (engineChanged) {
                if (g_manager->busy()) {
                    showStatus("有进行中的任务，下载引擎将在重启后生效");
                } else {
                    g_manager = createEngine();
                }
            }

            // aria2 参数（仅 aria2-next 引擎生效）：解析并夹取。
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
            cfg::setAria2Config(a2);
            g_aria2SplitText = std::to_string(a2.split);
            g_aria2ConnText = std::to_string(a2.maxConnectionPerServer);
            g_aria2MinSplitText = a2.minSplitSize;
            g_aria2LimitText =
                std::to_string(a2.maxDownloadLimit / 1024);

            // 下载路径。
            cfg::setDownloadDir(g_downloadDirText);
            showStatus("设置已保存");
        })
        .build();

    components::button(ui, "settings.discard")
        .position(infoX + labelW + S(8.0f) + S(76.0f) + S(8.0f) + S(76.0f), actionRowY)
        .size(S(76.0f), S(26.0f))
        .text("放弃")
        .fontSize(S(12.0f))
        .theme(theme.components, false)
        .onClick([] {
            // 回滚到已保存值。
            g_pendingTheme = cfg::themeMode();
            g_pendingEngine = cfg::engine();
            g_downloadDirText = cfg::downloadDir().string();
            g_themeMode = g_pendingTheme;
            g_dark = cfg::effectiveDark();
            const cfg::Aria2Config a2 = cfg::aria2Config();
            g_aria2SplitText = std::to_string(a2.split);
            g_aria2ConnText = std::to_string(a2.maxConnectionPerServer);
            g_aria2MinSplitText = a2.minSplitSize;
            g_aria2LimitText = std::to_string(a2.maxDownloadLimit / 1024);
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
                {"界面框架", "EUI-NEO 0.5.5"},
                {"下载引擎", "tinyhttps / aria2-next"},
                {"网络库", "tinyhttps 0.2.9"},
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

