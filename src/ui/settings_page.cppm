// ui/settings_page.cppm — 设置页：主题 / 下载路径 / aria2 参数 + 底部操作行。
// 从 tinynext.ui.pages 拆出，独立成模块。
module;

#include "eui_ui.h"

export module tinynext.ui.settings_page;

import std;
import tinynext.config;
import tinynext.i18n;         // tr() + setLanguage（语言切换）
import tinynext.ui.theme;
import tinynext.ui.utils;
import tinynext.ui.widgets;   // drawPanel（设置页大卡背景）
import tinynext.store.tasks;  // g_tasks.engineActive（保存 daemon 参数时提示重启）
import tinynext.store.ui;     // showStatus
import tinynext.ui.platform;

// ---- 设置页私有待提交状态（本模块自用，store 化后不再全局导出）----
// 输入框草稿 / 下拉展开态 / 左侧分组选中项：都是「这个 UI 的实现细节」，点
// 「保存」时写入配置并生效，点「放弃」回滚到已保存值。主题相关的 pending
// （g_pendingTheme/g_dark/...）归位在 tinynext.ui.theme。

// 设置页左侧配置分组：每组一个独立"子页面"，避免全部参数挤在一屏滚动过长。
// 分组对齐 MotrixNext：通用 / 下载 / BitTorrent / ED2K / 网络 / 高级（MotrixNext
// 同为 aria2-next 引擎，其分组是此类下载器的标准布局）。
enum class SettingsTab { General, Download, Video, BitTorrent, Ed2k, Network, Advanced };
SettingsTab g_settingsTab = SettingsTab::General;
// 下载目录待提交值（默认保存目录；点「保存」才写入配置）。
std::string g_downloadDirText = cfg::downloadDir().string();
bool g_langOpen = false;  // 语言下拉是否展开
bool g_themeOpen = false;  // 主题下拉是否展开

// aria2 参数待提交值（设置页输入框的文本形式）。
std::string g_aria2SplitText = std::to_string(cfg::aria2Config().split);
std::string g_aria2ConnText = std::to_string(cfg::aria2Config().maxConnectionPerServer);
// 最小分片数值部分（初始从配置拆出纯数值，"1M" → "1"），单位单独存 g_aria2MinSplitUnit。
std::string g_aria2MinSplitText = [] {
    std::string value, unit;
    splitSizeUnit(cfg::aria2Config().minSplitSize, value, unit);
    return value;
}();
std::string g_aria2MinSplitUnit = [] {
    std::string value, unit;
    splitSizeUnit(cfg::aria2Config().minSplitSize, value, unit);
    return unit;
}();  // 最小分片单位（KB/MB/GB），随配置解析
bool g_minSplitUnitOpen = false;  // 最小分片单位下拉是否展开
std::string g_aria2LimitText =
    std::to_string(cfg::aria2Config().maxDownloadLimit / 1024);  // KB/s
// daemon 级参数待提交值。
std::string g_proxyText = cfg::aria2Config().proxy;
std::string g_noProxyText = cfg::aria2Config().noProxy;
std::string g_maxTriesText = std::to_string(cfg::aria2Config().maxTries);
std::string g_retryWaitText = std::to_string(cfg::aria2Config().retryWait);
std::string g_maxConcurrentText =
    std::to_string(cfg::aria2Config().maxConcurrentDownloads);
bool g_removeControlFile = cfg::aria2Config().removeControlFile;
std::string g_onCompleteText = cfg::aria2Config().onDownloadComplete;
std::string g_userAgentText = cfg::aria2Config().userAgent;
std::string g_refererText = cfg::aria2Config().referer;
std::string g_diskCacheText = cfg::aria2Config().diskCache;
// ---- 新增 aria2 配置项（daemon 级，待提交值；见 settings_page 的 BitTorrent /
//      HTTP / 下载行为 / 完整性校验 四组）----
std::string g_seedTimeText = std::to_string(cfg::aria2Config().seedTime);
std::string g_seedRatioText = [] {
    const double r = cfg::aria2Config().seedRatio;
    return r > 0.0 ? std::format("{}", r) : "";  // 空 = 0 = 不限
}();
std::string g_btMaxPeersText = std::to_string(cfg::aria2Config().btMaxPeers);
std::string g_listenPortText = cfg::aria2Config().listenPort;
bool g_btEnableLpd = cfg::aria2Config().btEnableLpd;
std::string g_btTrackerText = cfg::aria2Config().btTracker;
std::string g_headerText = cfg::aria2Config().header;
std::string g_loadCookiesText = cfg::aria2Config().loadCookies;
std::string g_saveCookiesText = cfg::aria2Config().saveCookies;
std::string g_overallLimitText =
    std::to_string(cfg::aria2Config().maxOverallDownloadLimit / 1024);  // KB/s
std::string g_fileAllocation = cfg::aria2Config().fileAllocation;  // ""/none/trunc/falloc
bool g_fileAllocationOpen = false;
bool g_autoFileRenaming = cfg::aria2Config().autoFileRenaming;
bool g_allowOverwrite = cfg::aria2Config().allowOverwrite;
bool g_checkIntegrity = cfg::aria2Config().checkIntegrity;
std::string g_checksumText = cfg::aria2Config().checksum;
// ---- ED2K 配置项（daemon 级，待提交值；aria2-next 原生支持电驴）----
std::string g_ed2kServersText = cfg::aria2Config().ed2kServers;
std::string g_ed2kListenPortText = cfg::aria2Config().ed2kListenPort;
std::string g_ed2kUdpPortText = cfg::aria2Config().ed2kUdpListenPort;
std::string g_ed2kUploadSlotsText =
    std::to_string(cfg::aria2Config().ed2kUploadSlots);
// ---- 视频解析配置项（VideoConfig，独立于 aria2 daemon，保存即生效）----
std::string g_videoCookieText = cfg::videoConfig().bilibiliCookie;
std::string g_videoQualityText = cfg::videoConfig().defaultQuality;
bool g_videoKeepParts = cfg::videoConfig().keepM4sParts;
std::string g_videoJsRuntimeText = cfg::videoConfig().jsRuntime;

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
           x.diskCache == y.diskCache &&
           x.seedTime == y.seedTime &&
           x.seedRatio == y.seedRatio &&
           x.btMaxPeers == y.btMaxPeers &&
           x.listenPort == y.listenPort &&
           x.btEnableLpd == y.btEnableLpd &&
           x.btTracker == y.btTracker &&
           x.header == y.header &&
           x.loadCookies == y.loadCookies &&
           x.saveCookies == y.saveCookies &&
           x.maxOverallDownloadLimit == y.maxOverallDownloadLimit &&
           x.fileAllocation == y.fileAllocation &&
           x.autoFileRenaming == y.autoFileRenaming &&
           x.allowOverwrite == y.allowOverwrite &&
           x.checkIntegrity == y.checkIntegrity &&
           x.checksum == y.checksum &&
           x.ed2kServers == y.ed2kServers &&
           x.ed2kListenPort == y.ed2kListenPort &&
           x.ed2kUdpListenPort == y.ed2kUdpListenPort &&
           x.ed2kUploadSlots == y.ed2kUploadSlots;
}

// 最小分片单位下拉的可选项（KB/MB；GB 对 min-split-size 过于大，不提供）。
// splitSizeUnit 已把旧配置的 G 后缀统一换算成 MB，所以这里单位恒为 KB/MB。
constexpr const char* kSizeUnits[] = {"KB", "MB"};
int sizeUnitIndex(const std::string& unit) {
    if (unit == "KB") return 0;
    return 1;  // MB（默认）
}

// 文件分配方式下拉：""（aria2 默认=none）/ none / trunc / falloc。
constexpr const char* kFileAllocValues[] = {"", "none", "trunc", "falloc"};
int fileAllocationIndex(const std::string& v) {
    for (int i = 0; i < 4; ++i) {
        if (v == kFileAllocValues[i]) return i;
    }
    return 0;  // 默认
}

// 最小分片数值输入的上限：按当前单位换算成字节后统一封顶 1GiB
// （MB→1024、KB→1048576），保证换单位后数值不超保存校验范围。
int minSplitMax(const std::string& unit) {
    return static_cast<int>(1024LL * 1024 * 1024 / sizeUnitMultiplier(unit));
}

// ---- 主题下拉的选项映射（跟随系统 / 深色 / 浅色，见 General tab）----
// 索引顺序与 kThemeLabels 一致：0 跟随系统 / 1 深色 / 2 浅色。
int themeIndex() {
    switch (g_pendingTheme) {
        case cfg::ThemeMode::Dark:  return 1;
        case cfg::ThemeMode::Light: return 2;
        default:                    return 0;  // System
    }
}

// 选择主题即预览（g_dark 立即生效），「保存」才落盘（g_pendingTheme）。
void applyThemeChoice(int i) {
    cfg::ThemeMode mode;
    switch (i) {
        case 1: mode = cfg::ThemeMode::Dark;  break;
        case 2: mode = cfg::ThemeMode::Light; break;
        default: mode = cfg::ThemeMode::System; break;
    }
    g_pendingTheme = mode;
    g_themeMode = mode;
    switch (mode) {
        case cfg::ThemeMode::Dark:   g_dark = true;  break;
        case cfg::ThemeMode::Light:  g_dark = false; break;
        case cfg::ThemeMode::System: g_dark = cfg::osDark(); break;
    }
}

} // namespace

// ===================== 设置页 =====================
// 岛屿卡片风：配置分组子侧边栏 + 内容大卡两张浮岛（镜像下载页的任务列表子侧边栏）。
// 每组配置单独一个"子页面"，避免全部参数挤在一屏滚动过长。底部操作行固定在大卡底部。
export void drawSettingsPage(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    const float islandTop = kIslandVInset;
    const float islandH = screen.height - 2.0f * kIslandVInset;
    const float subX = kRailWidth;
    const float contentX = subX + kSubSidebarWidth + kIslandGap;
    const float contentW = screen.width - contentX - kRightMargin;
    // 整页一张岛：侧边栏 + 内容区同卡，交界处竖线分隔（取代原两张岛卡）。
    const float islandW = contentX + contentW - subX;
    const float dividerX = subX + kSubSidebarWidth + kIslandGap * 0.5f;
    const float pad = kPanelPad;
    const float infoX = contentX + pad;
    const float innerW = contentW - 2.0f * pad;

    // 整页一张岛卡（取代原「settings.sub.bg + settings.panel」两张），竖线分隔。
    drawPanel(ui, "settings.island", subX, islandTop, islandW, islandH, theme);
    drawVDivider(ui, "settings.island.vdivider", dividerX, islandTop, islandH, theme);

    // ---- 配置分组子侧边栏（同一张岛卡，镜像下载页的任务列表子侧边栏）----
    ui.stack("settings.sub")
        .position(subX, islandTop)
        .size(kSubSidebarWidth, islandH)
        .zIndex(4)
        .content([&] {
            // 侧边栏底并入整页岛卡（drawPanel "settings.island"），这里不再单独画卡。

            components::text(ui, "settings.sub.label")
                .position(9.0f, 10.0f)
                .size(kSubSidebarWidth - 18.0f, 18.0f)
                .text(tr("settings.title"))
                .fontSize(13.0f)
                .lineHeight(18.0f)
                .color(theme.titleText)
                .build();

            struct TabItem { const char* label; const char* id; unsigned int icon; SettingsTab tab; };
            // 对齐 MotrixNext 的设置分组（ed2k 由 aria2-next 原生支持）。
            // label 走 tr()，id 独立于语言，保证 eui 元素 id 稳定唯一。
            const TabItem kTabs[] = {
                {tr("settings.tab.general"), "general", 0xF013, SettingsTab::General},
                {tr("settings.tab.download"), "download", 0xF0AC, SettingsTab::Download},
                {tr("settings.tab.video"), "video", 0xF03D, SettingsTab::Video},
                {tr("settings.tab.bittorrent"), "bittorrent", 0xF0E7, SettingsTab::BitTorrent},
                {tr("settings.tab.ed2k"), "ed2k", 0xF0C0, SettingsTab::Ed2k},
                {tr("settings.tab.network"), "network", 0xF0D7, SettingsTab::Network},
                {tr("settings.tab.advanced"), "advanced", 0xF085, SettingsTab::Advanced},
            };
            const float itemW = kSubSidebarWidth - 12.0f;
            float itemY = 28.0f;
            for (const auto& item : kTabs) {
                drawSidebarItem(ui, std::string("settings.tab.") + item.id,
                               6.0f, itemY, itemW, 22.0f, item.label, item.icon,
                               g_settingsTab == item.tab, theme,
                               [tab = item.tab] { g_settingsTab = tab; });
                itemY += 27.0f;
            }
        })
        .build();

    // 标题距卡片顶留足空间（避免被顶部圆角/窗口边缘截到第一行）。
    const float titleY = islandTop + 16.0f;
    components::text(ui, "settings.title")
        .position(infoX, titleY)
        .size(innerW, 24.0f)
        .text(tr("app.tab.settings"))
        .fontSize(17.0f)
        .lineHeight(24.0f)
        .color(theme.titleText)
        .build();

    // ---- 布局常量 ----
    constexpr float kLabelW = 90.0f;
    constexpr float kFieldH = 26.0f;
    constexpr float kActionH = 26.0f;
    const float actionY = islandTop + islandH - pad - kActionH;
    const float scrollTop = titleY + 24.0f + 12.0f;
    const float scrollHeight = std::max(0.0f, actionY - 10.0f - scrollTop);

    // 设置项较多，正文放进 scrollView（主题/路径/aria2 参数）；底部操作行
    // 固定在窗口底部，始终可见。
    components::scrollView(ui, "settings.scroll")
        .position(infoX, scrollTop)
        .size(innerW, scrollHeight)
        .gap(6.0f)
        .theme(theme.components)
        .content([&](eui::Ui& sv, float width, float) {
            const float rowW = width;
            // 全部单列布局（用户要求：不要横向双列）。各字段只留 kInputW 宽度。
            constexpr float kInputW = 90.0f;
            const float fullW =
                std::max(160.0f, rowW - 16.0f - kLabelW - 8.0f);

            // 顶部占位行：把第一个表单行往下推一点，避免其顶边贴住滚动区上缘被裁掉。
            sv.stack("st.top.pad")
                .width(rowW)
                .height(3.0f)
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
                    .fontSize(11.0f)
                    .lineHeight(kFieldH)
                    .color(theme.metaText)
                    .build();
                components::input(r, "st." + id + ".input")
                    .position(x + kLabelW, -2.0f)
                    .size(inputW, 26.0f)
                    .placeholder(placeholder)
                    .value(value)
                    .fontFamily("")  // 用应用字体（Noto Sans SC），不要 eui 默认的 Microsoft YaHei
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
                    .fontSize(11.0f)
                    .lineHeight(kFieldH)
                    .color(theme.metaText)
                    .build();
                buildNumberStepper(r, "st." + id + ".input", x + kLabelW, -2.0f,
                                   inputW, 26.0f, theme, value, onChange, min, max, step);
            };

            // ===== 通用：主题 / 关闭行为 / 语言 =====
            if (g_settingsTab == SettingsTab::General) {
            // ---- 主题（下拉，选择即预览、保存才落盘）----
            // 行 zIndex 用 200：高于下面语言行的 100——主题下拉弹出会向下盖过语言行，
            // 同 zIndex 时后画（语言行）会压在弹层之上。
            row("theme", kFieldH, [&](eui::Ui& r, float) {
                components::text(r, "st.theme.label")
                    .position(0, 0)
                    .size(kLabelW, kFieldH)
                    .text(tr("settings.theme"))
                    .fontSize(12.0f)
                    .lineHeight(kFieldH)
                    .color(theme.metaText)
                    .build();
                r.stack("st.theme.pick")
                    .position(kLabelW, -2.0f)
                    .size(90.0f, 26.0f)
                    .zIndex(30)
                    .content([&] {
                        const char* themeLabels[] = {
                            tr("settings.theme.system"),
                            tr("settings.theme.dark"),
                            tr("settings.theme.light"),
                        };
                        buildListPicker(r, "theme", 90.0f, 26.0f, theme,
                                        g_themeOpen, themeLabels, 3,
                                        themeIndex(), false, PickerField::Text,
                                        [](int i) { applyThemeChoice(i); });
                    })
                    .build();
            }, 200);  // 行 zIndex：高于语言行（100），弹出下拉不被盖

            // ---- 关闭时缩到托盘（Windows/macOS 生效；Linux 的 eui 托盘为 stub
            //      无效果）。改后重启生效（dslAppConfig 启动时读取）。----
            row("close.tray", kFieldH, [&](eui::Ui& r, float) {
                components::text(r, "st.close.tray.label")
                    .position(0, 0)
                    .size(kLabelW, kFieldH)
                    .text(tr("settings.close_to_tray"))
                    .fontSize(11.0f)
                    .lineHeight(kFieldH)
                    .color(theme.metaText)
                    .build();
                buildToggleSwitch(r, "st.close.tray.toggle", kLabelW, 3.0f,
                                  36.0f, 20.0f, g_closeToTray, theme,
                                  [](bool v) { g_closeToTray = v; });
            });

            // ---- 语言（下拉框，立即生效 + 持久化）----
            row("lang", kFieldH, [&](eui::Ui& r, float) {
                components::text(r, "st.lang.label")
                    .position(0, 0)
                    .size(kLabelW, kFieldH)
                    .text(tr("settings.language"))
                    .fontSize(12.0f)
                    .lineHeight(kFieldH)
                    .color(theme.metaText)
                    .build();
                r.stack("st.lang.pick")
                    .position(kLabelW, -2.0f)
                    .size(90.0f, 26.0f)
                    .zIndex(30)
                    .content([&] {
                        // 语言名按本族语言显示，不随 UI 语言翻译。
                        const char* langLabels[] = {"简体中文", "繁體中文", "English"};
                        buildListPicker(r, "lang", 90.0f, 26.0f, theme,
                                        g_langOpen, langLabels, 3,
                                        g_lang == cfg::Lang::ZhTW ? 1
                                            : g_lang == cfg::Lang::En ? 2 : 0,
                                        false, PickerField::Text,
                                        [](int i) {
                                            setLanguage(i == 2 ? cfg::Lang::En
                                                : i == 1 ? cfg::Lang::ZhTW : cfg::Lang::ZhCN);
                                        });
                    })
                    .build();
            }, 100);  // 行 zIndex：让弹出下拉盖过后面各行

            }  // 全局 tab 结束（主题 / 关闭行为 / 语言）

            // ============== 下载 tab（下载路径 / 分片 / 连接 / 限速 / 并发 / 重试 / 缓存）==============
            if (g_settingsTab == SettingsTab::Download) {
                // ---- 下载路径：默认保存目录（输入框 + 系统文件夹选择器）----
                row("path", kFieldH, [&](eui::Ui& r, float w) {
                    components::text(r, "st.path.label")
                        .position(0, 0)
                        .size(kLabelW, kFieldH)
                        .text(tr("settings.download_path"))
                        .fontSize(12.0f)
                        .lineHeight(kFieldH)
                        .color(theme.metaText)
                        .build();
                    const float pathInputW =
                        std::max(160.0f, w - 16.0f - kLabelW - 8.0f - 60.0f);
                    components::input(r, "st.path.input")
                        .position(kLabelW, -2.0f)
                        .size(pathInputW, 26.0f)
                        .placeholder(tr("settings.download_dir_placeholder"))
                        .value(g_downloadDirText)
                        .fontFamily("")
                        .theme(theme.components)
                        .onChange([](const std::string& value) { g_downloadDirText = value; })
                        .build();
                    components::button(r, "st.path.browse")
                        .position(kLabelW + pathInputW + 8.0f, -2.0f)
                        .size(60.0f, 26.0f)
                        .text(tr("dl.browse"))
                        .fontSize(12.0f)
                        .theme(theme.components, false)
                        .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
                        .onClick([] {
                            // 只填待提交值，点「保存」才写入配置。
                            const auto picked = pickDownloadFolder();
                            if (!picked.empty()) {
                                g_downloadDirText = picked.string();
                            }
                        })
                        .build();
                });
                row("a.split", kFieldH, [&](eui::Ui& r, float) {
                    numericField(r, "a.split", tr("dl.splits"), 0, kInputW, g_aria2SplitText,
                                 [](const std::string& v) { g_aria2SplitText = v; },
                                 1, 64, 1);
                });
                row("a.conn", kFieldH, [&](eui::Ui& r, float) {
                    numericField(r, "a.conn", tr("settings.conn_per_server"), 0, kInputW, g_aria2ConnText,
                                 [](const std::string& v) { g_aria2ConnText = v; },
                                 1, 64, 1);
                });
                row("a.minsplit", kFieldH, [&](eui::Ui& r, float) {
                    // 最小分片：数值输入 + 紧跟的单位下拉（KB/MB/GB）。
                    // aria2 的 --min-split-size 要求 ≥ 1M，保存时统一校验兜底。
                    components::text(r, "st.a.minsplit.label")
                        .position(0, 0)
                        .size(kLabelW, kFieldH)
                        .text(tr("settings.min_split"))
                        .fontSize(11.0f)
                        .lineHeight(kFieldH)
                        .color(theme.metaText)
                        .build();
                    buildNumberStepper(r, "st.a.minsplit.input", kLabelW, -2.0f,
                                       kInputW, 26.0f, theme, g_aria2MinSplitText,
                                       [](const std::string& v) { g_aria2MinSplitText = v; },
                                       1, minSplitMax(g_aria2MinSplitUnit), 1);
                    r.stack("st.a.minsplit.unit")
                        .position(kLabelW + kInputW + 8.0f, -2.0f)
                        .size(64.0f, 26.0f)
                        .zIndex(30)
                        .content([&] {
                            buildListPicker(r, "a.minsplit.unit", 64.0f, 26.0f,
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
                row("a.limit", kFieldH, [&](eui::Ui& r, float) {
                    numericField(r, "a.limit", tr("settings.per_task_limit_kbs"), 0, kInputW, g_aria2LimitText,
                                 [](const std::string& v) { g_aria2LimitText = v; },
                                 0, 1000000, 100);
                });
                // 全局限速 / 文件分配：各占一行（daemon 级，重启生效）。
                row("beh.overall", kFieldH, [&](eui::Ui& r, float) {
                    numericField(r, "beh.overall", tr("settings.global_limit_kbs"), 0, kInputW,
                                 g_overallLimitText,
                                 [](const std::string& v) { g_overallLimitText = v; },
                                 0, 1000000, 100);
                });
                row("beh.allocation", kFieldH, [&](eui::Ui& r, float) {
                    components::text(r, "st.beh.allocation.label")
                        .position(0, 0)
                        .size(kLabelW, kFieldH)
                        .text(tr("settings.file_allocation"))
                        .fontSize(11.0f)
                        .lineHeight(kFieldH)
                        .color(theme.metaText)
                        .build();
                    r.stack("st.beh.allocation.pick")
                        .position(kLabelW, -2.0f)
                        .size(84.0f, 26.0f)
                        .zIndex(30)
                        .content([&] {
                            const char* allocLabels[] = {
                                tr("settings.file_alloc_default"), "none", "trunc", "falloc"
                            };
                            buildListPicker(r, "beh.allocation", 84.0f, 26.0f, theme,
                                            g_fileAllocationOpen, allocLabels, 4,
                                            fileAllocationIndex(g_fileAllocation), false,
                                            PickerField::Text,
                                            [](int i) {
                                                g_fileAllocation = kFileAllocValues[i];
                                            });
                        })
                        .build();
                }, 100);  // 行 zIndex：文件分配下拉展开时盖过后面各行
                row("a.concurrent", kFieldH, [&](eui::Ui& r, float) {
                    numericField(r, "a.concurrent", tr("settings.max_concurrent"), 0, kInputW,
                                 g_maxConcurrentText,
                                 [](const std::string& v) { g_maxConcurrentText = v; },
                                 1, 64, 1);
                });
                row("a.maxtries", kFieldH, [&](eui::Ui& r, float) {
                    numericField(r, "a.maxtries", tr("settings.max_retries"), 0, kInputW, g_maxTriesText,
                                 [](const std::string& v) { g_maxTriesText = v; },
                                 0, 100, 1);
                });
                row("a.retrywait", kFieldH, [&](eui::Ui& r, float) {
                    numericField(r, "a.retrywait", tr("settings.retry_wait"), 0, kInputW, g_retryWaitText,
                                 [](const std::string& v) { g_retryWaitText = v; },
                                 0, 600, 1);
                });
                row("a.diskcache", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.diskcache", tr("settings.disk_cache"), 0, fullW, g_diskCacheText,
                          [](const std::string& v) { g_diskCacheText = v; },
                          tr("settings.disk_cache_hint"));
                });
            }  // 下载 tab 结束

            // ============== 视频 tab（解析 cookie / 默认画质 / 保留分片）==============
            if (g_settingsTab == SettingsTab::Video) {
                row("video.cookie", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "video.cookie", "SESSDATA", 0, fullW, g_videoCookieText,
                          [](const std::string& v) { g_videoCookieText = v; },
                          tr("settings.bili_cookie_hint"));
                });
                row("video.cookie.hint", 18.0f, [&](eui::Ui& r, float w) {
                    components::text(r, "st.video.cookie.hint")
                        .position(kLabelW, 0)
                        .size(std::max(160.0f, w - 16.0f - kLabelW), 18.0f)
                        .text(tr("settings.bili_cookie_how"))
                        .fontSize(10.0f)
                        .lineHeight(18.0f)
                        .color(theme.metaText)
                        .build();
                });
                row("video.quality", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "video.quality", tr("settings.default_quality"), 0, fullW,
                          g_videoQualityText,
                          [](const std::string& v) { g_videoQualityText = v; },
                          tr("settings.quality_hint"));
                });
                row("video.keep", kFieldH, [&](eui::Ui& r, float) {
                    components::text(r, "st.video.keep.label")
                        .position(0, 0)
                        .size(kLabelW, kFieldH)
                        .text(tr("settings.keep_m4s"))
                        .fontSize(11.0f)
                        .lineHeight(kFieldH)
                        .color(theme.metaText)
                        .build();
                    buildToggleSwitch(r, "st.video.keep.toggle", kLabelW, 3.0f,
                                      36.0f, 20.0f, g_videoKeepParts, theme,
                                      [](bool v) { g_videoKeepParts = v; });
                });
                row("video.jsruntime", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "video.jsruntime", tr("settings.js_runtime"), 0, fullW,
                          g_videoJsRuntimeText,
                          [](const std::string& v) { g_videoJsRuntimeText = v; },
                          tr("settings.js_runtime_hint"));
                });
            }  // 视频 tab 结束

            // ============== 网络 tab（代理 / UA / Referer / 请求头 / Cookie）==============
            if (g_settingsTab == SettingsTab::Network) {
                row("a.proxy", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.proxy", tr("settings.proxy"), 0, fullW, g_proxyText,
                          [](const std::string& v) { g_proxyText = v; },
                          "http://user:pass@host:port");
                });
                row("a.noproxy", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.noproxy", tr("settings.no_proxy"), 0, fullW, g_noProxyText,
                          [](const std::string& v) { g_noProxyText = v; },
                          tr("settings.no_proxy_hint"));
                });
                row("a.ua", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.ua", "User-Agent", 0, fullW, g_userAgentText,
                          [](const std::string& v) { g_userAgentText = v; });
                });
                row("a.referer", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.referer", "Referer", 0, fullW, g_refererText,
                          [](const std::string& v) { g_refererText = v; });
                });
                // 请求头：多行输入（每行一个 --header），行高放大到 52 容纳两行。
                row("http.headers", 52.0f, [&](eui::Ui& r, float w) {
                    components::text(r, "st.http.headers.label")
                        .position(0, 0)
                        .size(kLabelW, 52.0f)
                        .text(tr("settings.headers"))
                        .fontSize(11.0f)
                        .lineHeight(52.0f)
                        .color(theme.metaText)
                        .build();
                    components::input(r, "st.http.headers.input")
                        .position(kLabelW, -2.0f)
                        .size(std::max(160.0f, w - 16.0f - kLabelW - 8.0f), 52.0f)
                        .multiline(true)
                        .placeholder(tr("settings.headers_hint"))
                        .value(g_headerText)
                        .fontFamily("")
                        .theme(theme.components)
                        .onChange([](const std::string& v) { g_headerText = v; })
                        .build();
                });
                row("http.loadcookie", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "http.loadcookie", tr("settings.cookie_file"), 0, fullW, g_loadCookiesText,
                          [](const std::string& v) { g_loadCookiesText = v; },
                          tr("settings.cookie_file_hint"));
                });
                row("http.savecookie", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "http.savecookie", tr("settings.save_cookies"), 0, fullW, g_saveCookiesText,
                          [](const std::string& v) { g_saveCookiesText = v; },
                          tr("settings.save_cookies_hint"));
                });
            }  // 网络 tab 结束

            // ============== BitTorrent tab ===============
            if (g_settingsTab == SettingsTab::BitTorrent) {
                row("bt.header", 18.0f, [&](eui::Ui& r, float w) {
                    components::text(r, "st.bt.header")
                        .position(0, 0)
                        .size(w, 18.0f)
                        .text("BitTorrent")
                        .fontSize(11.0f)
                        .lineHeight(18.0f)
                        .color(theme.statusText)
                        .build();
                });
                row("bt.seedtime", kFieldH, [&](eui::Ui& r, float) {
                    numericField(r, "bt.seedtime", tr("settings.seed_time"), 0, kInputW, g_seedTimeText,
                                 [](const std::string& v) { g_seedTimeText = v; },
                                 0, 100000, 60);
                });
                row("bt.maxpeers", kFieldH, [&](eui::Ui& r, float) {
                    numericField(r, "bt.maxpeers", tr("settings.max_peers"), 0, kInputW,
                                 g_btMaxPeersText,
                                 [](const std::string& v) { g_btMaxPeersText = v; },
                                 0, 10000, 10);
                });
                row("bt.seedratio", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "bt.seedratio", tr("settings.seed_ratio"), 0, fullW, g_seedRatioText,
                          [](const std::string& v) { g_seedRatioText = v; },
                          tr("settings.seed_ratio_hint"));
                });
                row("bt.port", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "bt.port", tr("settings.listen_port"), 0, fullW, g_listenPortText,
                          [](const std::string& v) { g_listenPortText = v; },
                          tr("settings.listen_port_hint"));
                });
                row("bt.lpd", kFieldH, [&](eui::Ui& r, float) {
                    components::text(r, "st.bt.lpd.label")
                        .position(0, 0)
                        .size(kLabelW, kFieldH)
                        .text(tr("settings.lpd"))
                        .fontSize(11.0f)
                        .lineHeight(kFieldH)
                        .color(theme.metaText)
                        .build();
                    buildToggleSwitch(r, "st.bt.lpd.toggle", kLabelW, 3.0f,
                                      36.0f, 20.0f, g_btEnableLpd, theme,
                                      [](bool v) { g_btEnableLpd = v; });
                });
                // ---- Tracker 服务器：额外 tracker 列表（多行，每行一个；也接受逗号分隔）----
                row("bt.tracker", 52.0f, [&](eui::Ui& r, float w) {
                    components::text(r, "st.bt.tracker.label")
                        .position(0, 0)
                        .size(kLabelW, 52.0f)
                        .text(tr("settings.tracker"))
                        .fontSize(11.0f)
                        .lineHeight(52.0f)
                        .color(theme.metaText)
                        .build();
                    components::input(r, "st.bt.tracker.input")
                        .position(kLabelW, -2.0f)
                        .size(std::max(160.0f, w - 16.0f - kLabelW - 8.0f), 52.0f)
                        .multiline(true)
                        .placeholder(tr("settings.tracker_hint"))
                        .value(g_btTrackerText)
                        .fontFamily("")
                        .theme(theme.components)
                        .onChange([](const std::string& v) { g_btTrackerText = v; })
                        .build();
                });
            }  // BitTorrent tab 结束

            // ============== ED2K tab（电驴；aria2-next 原生支持）==============
            if (g_settingsTab == SettingsTab::Ed2k) {
                row("ed2k.header", 18.0f, [&](eui::Ui& r, float w) {
                    components::text(r, "st.ed2k.header")
                        .position(0, 0)
                        .size(w, 18.0f)
                        .text(tr("settings.ed2k_section"))
                        .fontSize(11.0f)
                        .lineHeight(18.0f)
                        .color(theme.statusText)
                        .build();
                });
                row("ed2k.listen", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "ed2k.listen", tr("settings.ed2k_listen_tcp"), 0, kInputW, g_ed2kListenPortText,
                          [](const std::string& v) { g_ed2kListenPortText = v; },
                          tr("settings.ed2k_tcp_default"));
                });
                row("ed2k.udp", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "ed2k.udp", tr("settings.ed2k_udp_port"), 0, kInputW, g_ed2kUdpPortText,
                          [](const std::string& v) { g_ed2kUdpPortText = v; },
                          tr("settings.ed2k_udp_default"));
                });
                row("ed2k.servers", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "ed2k.servers", tr("settings.ed2k_servers"), 0, fullW, g_ed2kServersText,
                          [](const std::string& v) { g_ed2kServersText = v; },
                          tr("settings.ed2k_servers_hint"));
                });
                row("ed2k.slots", kFieldH, [&](eui::Ui& r, float) {
                    numericField(r, "ed2k.slots", tr("settings.ed2k_upload_slots"), 0, kInputW,
                                 g_ed2kUploadSlotsText,
                                 [](const std::string& v) { g_ed2kUploadSlotsText = v; },
                                 0, 100, 1);
                });
            }  // ED2K tab 结束

            // ============== 高级 tab（文件处理 / 完整性校验）==============
            if (g_settingsTab == SettingsTab::Advanced) {
                row("file.header", 18.0f, [&](eui::Ui& r, float w) {
                    components::text(r, "st.file.header")
                        .position(0, 0)
                        .size(w, 18.0f)
                        .text(tr("settings.file_handling"))
                        .fontSize(11.0f)
                        .lineHeight(18.0f)
                        .color(theme.statusText)
                        .build();
                });
                row("beh.rename", kFieldH, [&](eui::Ui& r, float) {
                    components::text(r, "st.beh.rename.label")
                        .position(0, 0)
                        .size(kLabelW, kFieldH)
                        .text(tr("settings.auto_rename"))
                        .fontSize(11.0f)
                        .lineHeight(kFieldH)
                        .color(theme.metaText)
                        .build();
                    buildToggleSwitch(r, "st.beh.rename.toggle", kLabelW, 3.0f,
                                      36.0f, 20.0f, g_autoFileRenaming, theme,
                                      [](bool v) { g_autoFileRenaming = v; });
                });
                row("beh.overwrite", kFieldH, [&](eui::Ui& r, float) {
                    components::text(r, "st.beh.overwrite.label")
                        .position(0, 0)
                        .size(kLabelW, kFieldH)
                        .text(tr("settings.allow_overwrite"))
                        .fontSize(11.0f)
                        .lineHeight(kFieldH)
                        .color(theme.metaText)
                        .build();
                    buildToggleSwitch(r, "st.beh.overwrite.toggle", kLabelW, 3.0f,
                                      36.0f, 20.0f, g_allowOverwrite, theme,
                                      [](bool v) { g_allowOverwrite = v; });
                });
                row("a.remctrl", kFieldH, [&](eui::Ui& r, float) {
                    components::text(r, "st.a.remctrl.label")
                        .position(0, 0)
                        .size(kLabelW, kFieldH)
                        .text(tr("settings.remove_control_file"))
                        .fontSize(11.0f)
                        .lineHeight(kFieldH)
                        .color(theme.metaText)
                        .build();
                    buildToggleSwitch(r, "st.a.remctrl.toggle", kLabelW, 3.0f,
                                      36.0f, 20.0f, g_removeControlFile, theme,
                                      [](bool v) { g_removeControlFile = v; });
                });
                row("a.oncomplete", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "a.oncomplete", tr("settings.on_complete"), 0, fullW, g_onCompleteText,
                          [](const std::string& v) { g_onCompleteText = v; },
                          tr("settings.on_complete_args"));
                });

                row("chk.header", 18.0f, [&](eui::Ui& r, float w) {
                    components::text(r, "st.chk.header")
                        .position(0, 0)
                        .size(w, 18.0f)
                        .text(tr("settings.integrity_check"))
                        .fontSize(11.0f)
                        .lineHeight(18.0f)
                        .color(theme.statusText)
                        .build();
                });
                row("chk.integrity", kFieldH, [&](eui::Ui& r, float) {
                    components::text(r, "st.chk.integrity.label")
                        .position(0, 0)
                        .size(kLabelW, kFieldH)
                        .text(tr("settings.check_integrity"))
                        .fontSize(11.0f)
                        .lineHeight(kFieldH)
                        .color(theme.metaText)
                        .build();
                    buildToggleSwitch(r, "st.chk.integrity.toggle", kLabelW, 3.0f,
                                      36.0f, 20.0f, g_checkIntegrity, theme,
                                      [](bool v) { g_checkIntegrity = v; });
                });
                row("chk.checksum", kFieldH, [&](eui::Ui& r, float) {
                    field(r, "chk.checksum", tr("settings.checksum"), 0, fullW, g_checksumText,
                          [](const std::string& v) { g_checksumText = v; },
                          tr("settings.checksum_hint"));
                });
            }  // 高级 tab 结束
        })
        .build();

    // ---- 操作行（固定窗口底部）：恢复默认路径 / 保存全部设置 / 放弃修改 ----
    components::button(ui, "settings.path.reset")
        .position(infoX + kLabelW, actionY)
        .size(76.0f, 26.0f)
        .text(tr("settings.reset"))
        .fontSize(12.0f)
        .theme(theme.components, false)
        .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
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
            g_seedTimeText = std::to_string(d.seedTime);
            g_seedRatioText = "";
            g_btMaxPeersText = std::to_string(d.btMaxPeers);
            g_listenPortText = d.listenPort;
            g_btEnableLpd = d.btEnableLpd;
            g_btTrackerText = d.btTracker;
            g_headerText = d.header;
            g_loadCookiesText = d.loadCookies;
            g_saveCookiesText = d.saveCookies;
            g_overallLimitText = std::to_string(d.maxOverallDownloadLimit / 1024);
            g_fileAllocation = d.fileAllocation;
            g_autoFileRenaming = d.autoFileRenaming;
            g_allowOverwrite = d.allowOverwrite;
            g_checkIntegrity = d.checkIntegrity;
            g_checksumText = d.checksum;
            g_ed2kServersText = d.ed2kServers;
            g_ed2kListenPortText = d.ed2kListenPort;
            g_ed2kUdpPortText = d.ed2kUdpListenPort;
            g_ed2kUploadSlotsText = std::to_string(d.ed2kUploadSlots);
            const cfg::VideoConfig vd;  // 视频配置回默认
            g_videoCookieText = vd.bilibiliCookie;
            g_videoQualityText = vd.defaultQuality;
            g_videoKeepParts = vd.keepM4sParts;
            g_videoJsRuntimeText = vd.jsRuntime;
            showStatus(tr("settings.reset_done"));
        })
        .build();

    components::button(ui, "settings.save")
        .position(infoX + kLabelW + 8.0f + 76.0f, actionY)
        .size(76.0f, 26.0f)
        .text(tr("settings.save"))
        .fontSize(12.0f)
        .theme(theme.components, true)
        .textColor(onPrimaryColor(theme))
        .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([] {
            const std::string t = trimText(g_downloadDirText);
            if (t.empty()) {
                showStatus(tr("settings.download_path_empty"));
                return;
            }
            g_downloadDirText = t;

            // aria2 参数：先整体校验（空/非整数/越界 → 报错并中止保存，不落任何值），
            // 全部通过才应用主题 + 落盘。
            auto validateInt = [&](const std::string& text, int lo, int hi,
                                   const char* label, int& out) -> bool {
                const std::string s = trimText(text);
                if (s.empty()) {
                    showStatus(trf("settings.field_empty", label));
                    return false;
                }
                int v = 0;
                try {
                    v = std::stoi(s);
                } catch (...) {
                    showStatus(trf("settings.field_not_int", label));
                    return false;
                }
                if (v < lo || v > hi) {
                    showStatus(trf("settings.field_range", label, lo, hi));
                    return false;
                }
                out = v;
                return true;
            };

            int splitV = 0, connV = 0, minSplitV = 0, limitV = 0,
                maxTriesV = 0, retryWaitV = 0, concurrentV = 0;
            if (!validateInt(g_aria2SplitText, 1, 64, tr("dl.splits"), splitV)) return;
            if (!validateInt(g_aria2ConnText, 1, 64, tr("settings.conn_per_server"), connV)) return;
            if (!validateInt(g_aria2MinSplitText, 1, minSplitMax(g_aria2MinSplitUnit),
                             tr("settings.min_split"), minSplitV)) return;
            if (!validateInt(g_aria2LimitText, 0, 1000000, tr("settings.limit_kbs"), limitV)) return;
            if (!validateInt(g_maxTriesText, 0, 100, tr("settings.max_retries"), maxTriesV)) return;
            if (!validateInt(g_retryWaitText, 0, 600, tr("settings.retry_wait"), retryWaitV)) return;
            if (!validateInt(g_maxConcurrentText, 1, 64, tr("settings.max_concurrent"), concurrentV)) return;
            // 新增项校验：做种时间/最大Peers/全局限速 整数范围；做种比率 浮点。
            int seedTimeV = 0, btMaxPeersV = 0, overallLimitV = 0, ed2kSlotsV = 0;
            if (!validateInt(g_seedTimeText, 0, 100000, tr("settings.seed_time"), seedTimeV)) return;
            if (!validateInt(g_btMaxPeersText, 0, 10000, tr("settings.max_peers"), btMaxPeersV)) return;
            if (!validateInt(g_overallLimitText, 0, 1000000, tr("settings.global_limit_kbs"), overallLimitV)) return;
            if (!validateInt(g_ed2kUploadSlotsText, 0, 100, tr("settings.ed2k_upload_slots_field"), ed2kSlotsV)) return;
            double seedRatioV = 0.0;
            const std::string seedRatioStr = trimText(g_seedRatioText);
            if (!seedRatioStr.empty()) {
                try {
                    seedRatioV = std::stod(seedRatioStr);
                } catch (...) {
                    showStatus(tr("settings.seed_ratio_not_number"));
                    return;
                }
                if (seedRatioV < 0.0) {
                    showStatus(tr("settings.seed_ratio_negative"));
                    return;
                }
            }

            // 最小分片：数值 + 单位合成后必须 ≥ 1M（aria2 下限）。
            const std::string minSplitCombined =
                joinSizeUnit(std::to_string(minSplitV), g_aria2MinSplitUnit);
            if (parseSizeBytes(minSplitCombined) < 1048576) {
                showStatus(tr("settings.min_split_too_small"));
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

            // 关闭行为：落盘（重启后生效，dslAppConfig 启动时读取）。
            const bool trayChanged = cfg::closeToTray() != g_closeToTray;
            cfg::setCloseToTray(g_closeToTray);

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
            a2.seedTime = seedTimeV;
            a2.seedRatio = seedRatioV;
            a2.btMaxPeers = btMaxPeersV;
            a2.listenPort = trimText(g_listenPortText);
            a2.btEnableLpd = g_btEnableLpd;
            a2.btTracker = trimText(g_btTrackerText);
            a2.header = trimText(g_headerText);
            a2.loadCookies = trimText(g_loadCookiesText);
            a2.saveCookies = trimText(g_saveCookiesText);
            a2.maxOverallDownloadLimit = static_cast<std::int64_t>(overallLimitV) * 1024;
            a2.fileAllocation = g_fileAllocation;
            a2.autoFileRenaming = g_autoFileRenaming;
            a2.allowOverwrite = g_allowOverwrite;
            a2.checkIntegrity = g_checkIntegrity;
            a2.checksum = trimText(g_checksumText);
            a2.ed2kServers = trimText(g_ed2kServersText);
            a2.ed2kListenPort = trimText(g_ed2kListenPortText);
            a2.ed2kUdpListenPort = trimText(g_ed2kUdpPortText);
            a2.ed2kUploadSlots = ed2kSlotsV;

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
            g_seedTimeText = std::to_string(a2.seedTime);
            g_seedRatioText = a2.seedRatio > 0.0 ? std::format("{}", a2.seedRatio) : "";
            g_btMaxPeersText = std::to_string(a2.btMaxPeers);
            g_listenPortText = a2.listenPort;
            g_btEnableLpd = a2.btEnableLpd;
            g_btTrackerText = a2.btTracker;
            g_headerText = a2.header;
            g_loadCookiesText = a2.loadCookies;
            g_saveCookiesText = a2.saveCookies;
            g_overallLimitText = std::to_string(a2.maxOverallDownloadLimit / 1024);
            g_fileAllocation = a2.fileAllocation;
            g_autoFileRenaming = a2.autoFileRenaming;
            g_allowOverwrite = a2.allowOverwrite;
            g_checkIntegrity = a2.checkIntegrity;
            g_checksumText = a2.checksum;
            g_ed2kServersText = a2.ed2kServers;
            g_ed2kListenPortText = a2.ed2kListenPort;
            g_ed2kUdpPortText = a2.ed2kUdpListenPort;
            g_ed2kUploadSlotsText = std::to_string(a2.ed2kUploadSlots);

            // 下载路径：落盘（立即生效，不用重启 daemon）。
            cfg::setDownloadDir(g_downloadDirText);

            // 视频解析配置：独立落盘（与 aria2 daemon 无关，保存即生效）。
            cfg::VideoConfig vc;
            vc.bilibiliCookie = trimText(g_videoCookieText);
            vc.defaultQuality = trimText(g_videoQualityText);
            vc.keepM4sParts = g_videoKeepParts;
            vc.jsRuntime = trimText(g_videoJsRuntimeText);
            cfg::setVideoConfig(vc);
            g_videoCookieText = vc.bilibiliCookie;
            g_videoQualityText = vc.defaultQuality;
            g_videoJsRuntimeText = vc.jsRuntime;

            // 汇总提示：aria2 daemon 已启动时，参数保存后需重启才生效。
            if (trayChanged) {
                showStatus(a2Changed && g_tasks.engineActive()
                    ? tr("settings.saved_restart")
                    : tr("settings.close_behavior_restart"));
            } else if (a2Changed && g_tasks.engineActive()) {
                showStatus(tr("settings.aria2_restart"));
            } else {
                showStatus(tr("settings.saved"));
            }
        })
        .build();

    components::button(ui, "settings.discard")
        .position(infoX + kLabelW + 8.0f + 76.0f + 8.0f + 76.0f, actionY)
        .size(76.0f, 26.0f)
        .text(tr("settings.discard"))
        .fontSize(12.0f)
        .theme(theme.components, false)
        .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([] {
            // 回滚到已保存值。
            g_pendingTheme = cfg::themeMode();
            g_downloadDirText = cfg::downloadDir().string();
            g_themeMode = g_pendingTheme;
            g_dark = cfg::effectiveDark();
            g_closeToTray = cfg::closeToTray();
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
            g_seedTimeText = std::to_string(a2.seedTime);
            g_seedRatioText = a2.seedRatio > 0.0 ? std::format("{}", a2.seedRatio) : "";
            g_btMaxPeersText = std::to_string(a2.btMaxPeers);
            g_listenPortText = a2.listenPort;
            g_btEnableLpd = a2.btEnableLpd;
            g_btTrackerText = a2.btTracker;
            g_headerText = a2.header;
            g_loadCookiesText = a2.loadCookies;
            g_saveCookiesText = a2.saveCookies;
            g_overallLimitText = std::to_string(a2.maxOverallDownloadLimit / 1024);
            g_fileAllocation = a2.fileAllocation;
            g_autoFileRenaming = a2.autoFileRenaming;
            g_allowOverwrite = a2.allowOverwrite;
            g_checkIntegrity = a2.checkIntegrity;
            g_checksumText = a2.checksum;
            g_ed2kServersText = a2.ed2kServers;
            g_ed2kListenPortText = a2.ed2kListenPort;
            g_ed2kUdpPortText = a2.ed2kUdpListenPort;
            g_ed2kUploadSlotsText = std::to_string(a2.ed2kUploadSlots);
            const cfg::VideoConfig vc = cfg::videoConfig();
            g_videoCookieText = vc.bilibiliCookie;
            g_videoQualityText = vc.defaultQuality;
            g_videoKeepParts = vc.keepM4sParts;
            g_videoJsRuntimeText = vc.jsRuntime;
            showStatus(tr("settings.changes_discarded"));
        })
        .build();
}
