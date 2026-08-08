// ui/settings_page.cppm — 设置页：主题 / 下载路径 / aria2 参数 + 底部操作行。
// 从 tinynext.ui.pages 拆出，独立成模块。
module;

#include "eui_ui.h"

export module tinynext.ui.settings_page;

import std;
import tinynext.config;
import tinynext.ui.theme;
import tinynext.ui.utils;
import tinynext.ui.widgets;   // drawPanel（设置页大卡背景）
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

// 最小分片单位下拉的可选项（KB/MB；GB 对 min-split-size 过于大，不提供）。
// splitSizeUnit 已把旧配置的 G 后缀统一换算成 MB，所以这里单位恒为 KB/MB。
constexpr const char* kSizeUnits[] = {"KB", "MB"};
int sizeUnitIndex(const std::string& unit) {
    if (unit == "KB") return 0;
    return 1;  // MB（默认）
}

// 最小分片数值输入的上限：按当前单位换算成字节后统一封顶 1GiB
// （MB→1024、KB→1048576），保证换单位后数值不超保存校验范围。
int minSplitMax(const std::string& unit) {
    return static_cast<int>(1024LL * 1024 * 1024 / sizeUnitMultiplier(unit));
}

} // namespace

// ===================== 设置页 =====================
// 设置页没有任务列表子侧边栏，内容区紧跟图标栏右侧。
export void drawSettingsPage(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    // 岛屿卡片风：设置页整块是一张悬浮圆角大卡（标题 + 表单 + 底部操作行都收在卡内）。
    // 图标栏占满左缘（不套卡片），这张卡紧贴它右侧起排（无间隙）；上下各留
    // kIslandVInset 空隙（卡片感）。
    const float islandTop = kIslandVInset;
    const float islandH = screen.height - 2.0f * kIslandVInset;
    const float contentX = kRailWidth;
    const float contentW = screen.width - contentX - kRightMargin;
    const float pad = kPanelPad;
    const float infoX = contentX + pad;
    const float innerW = contentW - 2.0f * pad;

    drawPanel(ui, "settings.panel", contentX, islandTop, contentW, islandH, theme);

    // 标题距卡片顶留足空间（避免被顶部圆角/窗口边缘截到第一行）。
    const float titleY = islandTop + S(16.0f);
    components::text(ui, "settings.title")
        .position(infoX, titleY)
        .size(innerW, S(24.0f))
        .text("设置")
        .fontSize(S(17.0f))
        .lineHeight(S(24.0f))
        .color(theme.titleText)
        .build();

    // ---- 布局常量 ----
    constexpr float kLabelW = S(90.0f);
    constexpr float kFieldH = S(26.0f);
    constexpr float kActionH = S(26.0f);
    const float actionY = islandTop + islandH - pad - kActionH;
    const float scrollTop = titleY + S(24.0f) + S(12.0f);
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
        .position(infoX, scrollTop)
        .size(innerW, scrollHeight)
        .gap(S(6.0f))
        .theme(theme.components)
        .content([&](eui::Ui& sv, float width, float) {
            const float rowW = width;

            // 顶部占位行：把第一个表单行往下推一点，避免其顶边贴住滚动区上缘被裁掉。
            sv.stack("st.top.pad")
                .width(rowW)
                .height(S(3.0f))
                .content([&] {})
                .build();

            // 一行 = 一个定高 stack 子项，由 scrollView 纵向自动排布。
            // zIndex 用于把含弹出下拉的行抬到其它行之上（eui 按 zIndex 稳定排序
            // 直接子元素），否则弹层会被后面几行盖住。
            auto row = [&](const std::string& id, float height,
                           const std::function<void(eui::Ui&, float)>& draw,
                           int zIndex = 0) {
                sv.stack("st." + id)
                    .width(rowW)
                    .height(height)
                    .zIndex(zIndex)
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

            // 行内"标签 + 数字步进输入"：与 field 同布局，但输入框带 -/+ 步进按钮。
            // 只接受数字的字段用这个，既能手输又能点加减。
            auto numericField = [&](eui::Ui& r, const std::string& id, const char* label,
                                    float x, float inputW, const std::string& value,
                                    const std::function<void(const std::string&)>& onChange,
                                    int min, int max, int step) {
                components::text(r, "st." + id + ".label")
                    .position(x, 0)
                    .size(kLabelW, kFieldH)
                    .text(label)
                    .fontSize(S(11.0f))
                    .lineHeight(kFieldH)
                    .color(theme.metaText)
                    .build();
                buildNumberStepper(r, "st." + id + ".input", x + kLabelW, -S(2.0f),
                                   inputW, S(26.0f), theme, value, onChange, min, max, step);
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
                    numericField(r, "a.split", "分片数", 0, kInputW, g_aria2SplitText,
                                 [](const std::string& v) { g_aria2SplitText = v; },
                                 1, 64, 1);
                    numericField(r, "a.conn", "每服务器连接", kCol2X, kInputW, g_aria2ConnText,
                                 [](const std::string& v) { g_aria2ConnText = v; },
                                 1, 64, 1);
                });
                row("a.minsplit", kFieldH, [&](eui::Ui& r, float) {
                    // 最小分片：数值输入 + 紧跟的单位下拉（KB/MB/GB）。
                    // aria2 的 --min-split-size 要求 ≥ 1M，保存时统一校验兜底。
                    components::text(r, "st.a.minsplit.label")
                        .position(0, 0)
                        .size(kLabelW, kFieldH)
                        .text("最小分片")
                        .fontSize(S(11.0f))
                        .lineHeight(kFieldH)
                        .color(theme.metaText)
                        .build();
                    buildNumberStepper(r, "st.a.minsplit.input", kLabelW, -S(2.0f),
                                       kInputW, S(26.0f), theme, g_aria2MinSplitText,
                                       [](const std::string& v) { g_aria2MinSplitText = v; },
                                       1, minSplitMax(g_aria2MinSplitUnit), 1);
                    r.stack("st.a.minsplit.unit")
                        .position(kLabelW + kInputW + S(8.0f), -S(2.0f))
                        .size(S(64.0f), S(26.0f))
                        .zIndex(30)
                        .content([&] {
                            buildListPicker(r, "a.minsplit.unit", S(64.0f), S(26.0f),
                                            theme, g_minSplitUnitOpen, kSizeUnits, 2,
                                            sizeUnitIndex(g_aria2MinSplitUnit), false,
                                            PickerField::Text,
                                            [](int i) {
                                                // 换单位按 1024 进制换算数值（字节量不变），
                                                // 而不是只改单位标签。
                                                const std::string newUnit = kSizeUnits[i];
                                                if (newUnit != g_aria2MinSplitUnit) {
                                                    g_aria2MinSplitText = convertSizeUnit(
                                                        g_aria2MinSplitText,
                                                        g_aria2MinSplitUnit, newUnit);
                                                    g_aria2MinSplitUnit = newUnit;
                                                }
                                            });
                        })
                        .build();
                }, 100);  // 行 zIndex：让弹出下拉盖过后面各行
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
                    numericField(r, "a.maxtries", "最大重试次数", 0, kInputW, g_maxTriesText,
                                 [](const std::string& v) { g_maxTriesText = v; },
                                 0, 100, 1);
                    numericField(r, "a.retrywait", "重试等待秒", kCol2X, kInputW, g_retryWaitText,
                                 [](const std::string& v) { g_retryWaitText = v; },
                                 0, 600, 1);
                });
                row("a.limit", kFieldH, [&](eui::Ui& r, float) {
                    numericField(r, "a.limit", "限速KB/s", 0, kInputW, g_aria2LimitText,
                                 [](const std::string& v) { g_aria2LimitText = v; },
                                 0, 1000000, 100);
                    numericField(r, "a.concurrent", "最大同时下载数", kCol2X, kInputW,
                                 g_maxConcurrentText,
                                 [](const std::string& v) { g_maxConcurrentText = v; },
                                 1, 64, 1);
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
        .onClick([] {
            // 全部设置回默认：主题回「跟随系统」（并即时预览）、路径回系统下载目录、
            // aria2 参数回默认值。仍需点「保存」才落盘（与放弃/保存语义一致）。
            g_pendingTheme = cfg::ThemeMode::System;
            g_themeMode = cfg::ThemeMode::System;
            g_dark = cfg::osDark();
            g_downloadDirText = cfg::defaultDownloadDir().string();
            const cfg::Aria2Config d;  // 默认构造 = 各字段默认值
            g_aria2SplitText = std::to_string(d.split);
            g_aria2ConnText = std::to_string(d.maxConnectionPerServer);
            splitSizeUnit(d.minSplitSize, g_aria2MinSplitText, g_aria2MinSplitUnit);
            g_aria2LimitText = std::to_string(d.maxDownloadLimit / 1024);
            g_proxyText = d.proxy;
            g_noProxyText = d.noProxy;
            g_maxTriesText = std::to_string(d.maxTries);
            g_retryWaitText = std::to_string(d.retryWait);
            g_maxConcurrentText = std::to_string(d.maxConcurrentDownloads);
            g_removeControlFile = d.removeControlFile;
            g_onCompleteText = d.onDownloadComplete;
            g_userAgentText = d.userAgent;
            g_refererText = d.referer;
            g_diskCacheText = d.diskCache;
            showStatus("已恢复默认（点「保存」生效）");
        })
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

            // aria2 参数：先整体校验（空/非整数/越界 → 报错并中止保存，不落任何值），
            // 全部通过才应用主题 + 落盘。
            auto validateInt = [&](const std::string& text, int lo, int hi,
                                   const char* label, int& out) -> bool {
                const std::string s = trimText(text);
                if (s.empty()) {
                    showStatus(std::format("{} 不能为空", label));
                    return false;
                }
                int v = 0;
                try {
                    v = std::stoi(s);
                } catch (...) {
                    showStatus(std::format("{} 必须是整数", label));
                    return false;
                }
                if (v < lo || v > hi) {
                    showStatus(std::format("{} 需在 {}~{} 之间", label, lo, hi));
                    return false;
                }
                out = v;
                return true;
            };

            int splitV = 0, connV = 0, minSplitV = 0, limitV = 0,
                maxTriesV = 0, retryWaitV = 0, concurrentV = 0;
            if (!validateInt(g_aria2SplitText, 1, 64, "分片数", splitV)) return;
            if (!validateInt(g_aria2ConnText, 1, 64, "每服务器连接", connV)) return;
            if (!validateInt(g_aria2MinSplitText, 1, minSplitMax(g_aria2MinSplitUnit),
                             "最小分片", minSplitV)) return;
            if (!validateInt(g_aria2LimitText, 0, 1000000, "限速KB/s", limitV)) return;
            if (!validateInt(g_maxTriesText, 0, 100, "最大重试次数", maxTriesV)) return;
            if (!validateInt(g_retryWaitText, 0, 600, "重试等待秒", retryWaitV)) return;
            if (!validateInt(g_maxConcurrentText, 1, 64, "最大同时下载数", concurrentV)) return;

            // 最小分片：数值 + 单位合成后必须 ≥ 1M（aria2 下限）。
            const std::string minSplitCombined =
                joinSizeUnit(std::to_string(minSplitV), g_aria2MinSplitUnit);
            if (parseSizeBytes(minSplitCombined) < 1048576) {
                showStatus("最小分片不能小于 1M");
                return;
            }

            // 主题：套用待提交值并落盘。
            g_themeMode = g_pendingTheme;
            switch (g_pendingTheme) {
                case cfg::ThemeMode::Dark:   g_dark = true;  break;
                case cfg::ThemeMode::Light:  g_dark = false; break;
                case cfg::ThemeMode::System: g_dark = cfg::osDark(); break;
            }
            cfg::setThemeMode(g_pendingTheme);

            // aria2 参数：用已校验值落盘。
            const cfg::Aria2Config cur = cfg::aria2Config();
            cfg::Aria2Config a2;
            a2.split = splitV;
            a2.maxConnectionPerServer = connV;
            a2.minSplitSize = minSplitCombined;
            a2.maxDownloadLimit = static_cast<std::int64_t>(limitV) * 1024;
            a2.proxy = trimText(g_proxyText);
            a2.noProxy = trimText(g_noProxyText);
            a2.maxTries = maxTriesV;
            a2.retryWait = retryWaitV;
            a2.maxConcurrentDownloads = concurrentV;
            a2.removeControlFile = g_removeControlFile;
            a2.onDownloadComplete = trimText(g_onCompleteText);
            a2.userAgent = trimText(g_userAgentText);
            a2.referer = trimText(g_refererText);
            a2.diskCache = trimText(g_diskCacheText);

            const bool a2Changed = !sameAria2Config(cur, a2);
            cfg::setAria2Config(a2);
            g_aria2SplitText = std::to_string(a2.split);
            g_aria2ConnText = std::to_string(a2.maxConnectionPerServer);
            splitSizeUnit(a2.minSplitSize, g_aria2MinSplitText, g_aria2MinSplitUnit);
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
            splitSizeUnit(a2.minSplitSize, g_aria2MinSplitText, g_aria2MinSplitUnit);
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
