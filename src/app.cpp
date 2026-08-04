// app.cpp — TinyNext downloader UI built on compat.eui-neo.
//
// This project enables the `app-main` feature of compat.eui-neo, so there is
// deliberately NO main() in any translation unit here: the package supplies
// upstream's GLFW entry point (core/app/glfw_app_main.cpp), which owns the
// window and render loop and calls back into the two symbols every EUI
// application must define — app::dslAppConfig() and app::compose().

#include <eui_neo.h>

// eui_neo.h must stay above `import std;` (it pulls in the platform headers).
// After the import, no standard header may be #included again in this TU —
// the std module already declares them.
import std;
import tinynext.download_manager;

namespace {

// 所有布局尺寸都按“逻辑像素”设计。eui 的逻辑坐标空间 = 物理像素 / DPI 缩放：
// 窗口为 920x620 物理像素时，逻辑空间是 460x310（本机 content scale = 2.0）。
// 因此本文件的数值都按逻辑空间取值，并尽量用 screen.width/height 推算，
// 随窗口缩放自适应。
constexpr float kMargin = 12.0f;
constexpr float kInputHeight = 26.0f;
constexpr float kPagerHeight = 24.0f;    // 翻页行高
constexpr float kPagerBottomMargin = 10.0f;  // 翻页行距窗口底部
constexpr float kSizeDropdownWidth = 96.0f;  // 分页大小下拉框宽

// 卡片式下载项：高度、内边距与间距。
constexpr float kCardHeight = 68.0f;
constexpr float kCardPad = 10.0f;
constexpr float kCardGap = 6.0f;
constexpr float kCardIconW = 22.0f;  // 卡片操作图标按钮边长
constexpr float kCardIconGap = 4.0f;

// 布局：主侧边栏为纯图标栏；下载页内部再分一个下载状态子侧边栏。
constexpr float kRailWidth = 26.0f;         // 大侧边栏（图标栏）宽
constexpr float kSubSidebarWidth = 96.0f;   // 下载页内状态子侧边栏宽

// ---------------------------------------------------------------- themes --

// 日间/夜间双主题。clearColor 在 eui 初始化时固化、无法运行时修改，所以
// 主题由 compose 全权控制 —— 用一个全屏背景矩形盖住窗口底色。整个系统的
// 所有颜色（背景、表面、主色、文本、状态色）都从 currentTheme() 取：
//   - 控件统一走 `.theme(theme.components)`，一套 tokens 管按钮/输入框/
//     进度条的内部配色（fill/文字/边框/hover/focus）；
//   - 文本等裸颜色从 theme 字段取，深浅主题各自定义，保证对比度；
//   - 后续新增任何控件，只要同样从 currentTheme() 取色，就自动与现有
//     UI 保持一致。compose 每帧重跑，切换即时生效。
struct AppTheme {
    bool dark;
    eui::Color titleText;    // 大标题
    eui::Color nameText;     // 文件名
    eui::Color metaText;     // 百分比/速度等次要文本
    eui::Color hintText;     // 空态提示
    eui::Color statusText;   // 底部状态消息
    eui::Color downloading;  // 状态色
    eui::Color paused;
    eui::Color done;
    eui::Color failed;
    eui::Color idle;
    components::theme::ThemeColorTokens components;  // 传给组件的完整 tokens
};

// 主色不写死：夜间用亮蓝（暗底上醒目），日间用深蓝（浅底上对比好）。
// 两套主题共享同一套 Token 结构，只是颜色值不同。
const AppTheme kDarkTheme = {
    true,
    {0.94f, 0.97f, 1.0f, 1.0f},
    {0.88f, 0.92f, 1.0f, 1.0f},
    {0.62f, 0.70f, 0.82f, 1.0f},
    {0.42f, 0.47f, 0.55f, 1.0f},
    {0.72f, 0.83f, 0.97f, 1.0f},
    {0.58f, 0.72f, 0.95f, 1.0f},
    {0.95f, 0.72f, 0.30f, 1.0f},
    {0.35f, 0.80f, 0.45f, 1.0f},
    {0.92f, 0.40f, 0.38f, 1.0f},
    {0.55f, 0.58f, 0.62f, 1.0f},
    [] {
        auto tokens = components::theme::dark();
        tokens.primary = {0.90f, 0.32f, 0.18f, 1.0f};  // 深色模式主色：橘红
        return tokens;
    }(),
};

const AppTheme kLightTheme = {
    false,
    {0.16f, 0.20f, 0.30f, 1.0f},
    {0.10f, 0.13f, 0.20f, 1.0f},
    {0.42f, 0.48f, 0.58f, 1.0f},
    {0.55f, 0.60f, 0.68f, 1.0f},
    {0.18f, 0.38f, 0.62f, 1.0f},
    {0.18f, 0.42f, 0.82f, 1.0f},
    {0.72f, 0.48f, 0.05f, 1.0f},
    {0.10f, 0.55f, 0.25f, 1.0f},
    {0.78f, 0.20f, 0.14f, 1.0f},
    {0.48f, 0.52f, 0.58f, 1.0f},
    [] {
        auto tokens = components::theme::light();
        tokens.primary = {0.02f, 0.62f, 0.72f, 1.0f};  // 浅色模式主色：青色
        return tokens;
    }(),
};

// 初始主题默认夜间；`TINYNEXT_THEME=light` 可强制日间（供测试与偏好）。
bool g_dark = [] {
    if (const char* value = std::getenv("TINYNEXT_THEME")) {
        return std::string_view(value) != "light";
    }
    return true;
}();

// -------------------------------------------------- list filter / pagination --

enum class Filter { All, Active, Done };

// 下载列表：状态筛选 + 分页。snapshot() 最新在前，先按筛选收窄，再按
// 当前页切片。切换筛选或分页大小时回到第 1 页。
Filter g_filter = Filter::All;
int g_page = 1;
int g_pageSize = 5;
bool g_pageSizeOpen = false;  // 分页大小下拉是否展开
constexpr int kPageSizes[] = {5, 10, 20, 50, 100};

// "下载中" = 排队/进行/暂停；"已完成" = 完成/失败/已取消。
bool stateMatches(Filter filter, dl::State state) {
    switch (filter) {
        case Filter::All: return true;
        case Filter::Active:
            return state == dl::State::Queued || state == dl::State::Downloading ||
                   state == dl::State::Paused;
        case Filter::Done:
            return state == dl::State::Done || state == dl::State::Failed ||
                   state == dl::State::Cancelled;
    }
    return true;
}

int pageSizeIndex() {
    for (int i = 0; i < 5; ++i) {
        if (kPageSizes[i] == g_pageSize) return i;
    }
    return 0;  // 5
}

const AppTheme& currentTheme() {
    return g_dark ? kDarkTheme : kLightTheme;
}

dl::DownloadManager g_manager;
std::string g_urlText;
std::string g_statusMessage;
float g_statusTimer = 0.0f;
bool g_addOpen = false;  // “添加下载”弹窗是否打开

// ---------------------------------------------------------------- helpers --

std::string percentDecode(std::string s) {
    const auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexValue(s[i + 1]);
            const int lo = hexValue(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string fileNameFromUrl(const std::string& url) {
    const std::size_t cut = url.find_first_of("?#");
    const std::string base = cut == std::string::npos ? url : url.substr(0, cut);
    const std::size_t slash = base.find_last_of('/');
    std::string name = slash == std::string::npos ? base : base.substr(slash + 1);
    if (name.empty()) {
        name = "download";
    }
    return percentDecode(std::move(name));
}

std::string formatBytes(std::int64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return std::format("{} B", bytes);
    }
    return std::format("{:.1f} {}", value, kUnits[unit]);
}

std::string formatSpeed(double bytesPerSecond) {
    if (bytesPerSecond <= 0.0) {
        return "";
    }
    return formatBytes(static_cast<std::int64_t>(bytesPerSecond)) + "/s";
}

void showStatus(std::string message) {
    g_statusMessage = std::move(message);
    g_statusTimer = 4.0f;
}

// 用系统默认程序打开文件 / 打开所在文件夹（后台执行，避免阻塞 UI 线程）。
void openFile(const std::filesystem::path& path) {
#ifdef _WIN32
    std::system(("explorer \"" + path.string() + "\"").c_str());
#else
    std::system(("xdg-open \"" + path.string() + "\" >/dev/null 2>&1 &").c_str());
#endif
}

void openContainingFolder(const std::filesystem::path& path) {
#ifdef _WIN32
    std::system(("explorer /select,\"" + path.string() + "\"").c_str());
#else
    std::system(("xdg-open \"" + path.parent_path().string() + "\" >/dev/null 2>&1 &").c_str());
#endif
}

// ----------------------------------------------------------- UI callbacks --

// 校验并启动一个下载；返回是否成功（供“添加下载”弹窗决定是否关闭）。
bool addDownload() {
    std::string url = g_urlText;
    const std::size_t first = url.find_first_not_of(" \t\r\n");
    const std::size_t last = url.find_last_not_of(" \t\r\n");
    url = first == std::string::npos ? "" : url.substr(first, last - first + 1);
    if (url.empty()) {
        showStatus("请输入下载地址");
        return false;
    }
    if (!url.starts_with("https://")) {
        if (url.starts_with("http://")) {
            // tinyhttps only speaks HTTPS; best-effort upgrade.
            url = "https://" + url.substr(7);
        } else {
            showStatus("仅支持 HTTPS 下载链接");
            return false;
        }
    }

    const std::filesystem::path dest =
        std::filesystem::path("downloads") / fileNameFromUrl(url);
    const std::uint64_t id = g_manager.start(url, dest);
    showStatus(std::format("已开始下载 #{} — {}", id, dest.filename().string()));
    return true;
}

// --------------------------------------------------------------- rendering --

eui::Color stateColor(dl::State state) {
    const AppTheme& theme = currentTheme();
    switch (state) {
        case dl::State::Downloading: return theme.downloading;
        case dl::State::Paused:      return theme.paused;
        case dl::State::Done:        return theme.done;
        case dl::State::Failed:      return theme.failed;
        case dl::State::Queued:
        case dl::State::Cancelled:   return theme.idle;
    }
    return theme.idle;
}

// 卡片右上角的状态短标签。
std::string stateLabel(dl::State state) {
    switch (state) {
        case dl::State::Queued:      return "等待中";
        case dl::State::Downloading: return "下载中";
        case dl::State::Paused:      return "已暂停";
        case dl::State::Done:        return "已完成";
        case dl::State::Cancelled:   return "已取消";
        case dl::State::Failed:      return "失败";
    }
    return "";
}

// 卡片信息行：百分比 · 速度 · 已下载/总大小；非下载中则显示状态/错误。
std::string cardInfoText(const dl::TaskView& task) {
    switch (task.state) {
        case dl::State::Queued: return "等待队列中";
        case dl::State::Paused: return "已暂停";
        case dl::State::Cancelled: return "已取消";
        case dl::State::Done:
            return task.totalBytes > 0
                ? "已完成 · " + formatBytes(task.totalBytes)
                : "已完成";
        case dl::State::Failed: {
            std::string error = task.error;
            if (error.size() > 36) {
                error = error.substr(0, 36) + "…";
            }
            return error.empty() ? "失败" : error;
        }
        case dl::State::Downloading:
            break;
    }

    std::string parts;
    const auto push = [&](std::string_view part) {
        if (!parts.empty()) {
            parts += "  ·  ";
        }
        parts += part;
    };
    if (task.totalBytes > 0) {
        const double pct = std::clamp(
            100.0 * static_cast<double>(task.downloadedBytes) /
                static_cast<double>(task.totalBytes),
            0.0, 100.0);
        push(std::format("{:.0f}%", pct));
    } else if (task.downloadedBytes > 0) {
        push(formatBytes(task.downloadedBytes));
    }
    const std::string speed = formatSpeed(task.speedBps);
    if (!speed.empty()) {
        push(speed);
    }
    if (task.totalBytes > 0) {
        push(std::format("{} / {}", formatBytes(task.downloadedBytes),
                         formatBytes(task.totalBytes)));
    }
    return parts.empty() ? "下载中" : parts;
}

// 卡片内的小图标操作按钮：纯图标、无文字，hover/按下反馈走组件默认状态。
void drawCardAction(eui::Ui& ui, const std::string& id, float x, float y,
                    unsigned int icon, bool primary, const AppTheme& theme,
                    std::function<void()> onClick) {
    components::button(ui, id)
        .position(x, y)
        .size(kCardIconW, kCardIconW)
        .icon(icon)
        .text("")
        .iconSize(11.0f)
        .theme(theme.components, primary)
        .onClick(std::move(onClick))
        .build();
}

// 卡片式下载项：名称、进度、各种信息在卡片内纵向排布。
// 卡片作为 scrollview 纵向流的一个子项；卡片内部用绝对定位布局三行：
//   第 1 行  文件名（左）+ 状态标签（右）
//   第 2 行  进度条（横贯卡片）
//   第 3 行  信息文本（左）+ 操作按钮（右）
void drawTaskCard(eui::Ui& ui, const dl::TaskView& task, float cardWidth) {
    const AppTheme& theme = currentTheme();
    const std::string fid = "task." + std::to_string(task.id);
    const float inner = cardWidth - kCardPad * 2.0f;  // 卡片内可用宽度

    // 进度值：已完成视为 1，其余按已下载/总量计算。
    float progress = 0.0f;
    if (task.state == dl::State::Done) {
        progress = 1.0f;
    } else if (task.totalBytes > 0) {
        progress = std::clamp(
            static_cast<float>(static_cast<double>(task.downloadedBytes) /
                               static_cast<double>(task.totalBytes)),
            0.0f, 1.0f);
    }

    ui.stack(fid)
        .width(cardWidth)
        .height(kCardHeight)
        .content([&] {
            // 卡片底：圆角表面 + 细边框。
            ui.rect(fid + ".bg")
                .position(0, 0)
                .size(cardWidth, kCardHeight)
                .color(theme.components.surface)
                .radius(8.0f)
                .border(1.0f, components::theme::withOpacity(theme.components.border, 0.55f))
                .build();

            // ---- 第 1 行：文件名 + 状态 ----
            const float stateW = 46.0f;
            components::text(ui, fid + ".name")
                .position(kCardPad, 9.0f)
                .size(inner - stateW - 6.0f, 15.0f)
                .text(fileNameFromUrl(task.url))
                .fontSize(13.0f)
                .lineHeight(15.0f)
                .maxWidth(inner - stateW - 6.0f)
                .color(theme.nameText)
                .build();
            components::text(ui, fid + ".state")
                .position(cardWidth - kCardPad - stateW, 9.0f)
                .size(stateW, 15.0f)
                .text(stateLabel(task.state))
                .fontSize(10.0f)
                .lineHeight(15.0f)
                .horizontalAlign(core::HorizontalAlign::Right)
                .color(stateColor(task.state))
                .build();

            // ---- 第 2 行：进度条 ----
            ui.stack(fid + ".progress.slot")
                .position(kCardPad, 28.0f)
                .size(inner, 6.0f)
                .content([&] {
                    components::progress(ui, fid + ".progress")
                        .size(inner, 6.0f)
                        .value(progress)
                        .theme(theme.components)
                        .build();
                })
                .build();

            // ---- 第 3 行：信息 + 图标操作按钮（全部用图标，无文字）----
            // 各状态展示的操作：复制/删除始终有；下载中=暂停+取消，
            // 已暂停=继续+取消，已完成=打开+打开所在文件夹。
            const bool showPause = task.state == dl::State::Downloading;
            const bool showResume = task.state == dl::State::Paused;
            const bool showCancel = task.state == dl::State::Queued ||
                                    task.state == dl::State::Downloading ||
                                    task.state == dl::State::Paused;
            const bool showOpen = task.state == dl::State::Done;
            const bool showOpenFolder = task.state == dl::State::Done;

            const int actionCount = 2 + (showPause || showResume ? 1 : 0) +
                                    (showCancel ? 1 : 0) +
                                    (showOpen ? 1 : 0) + (showOpenFolder ? 1 : 0);
            const float iconsW = actionCount * kCardIconW +
                                 (actionCount > 0 ? (actionCount - 1) * kCardIconGap : 0.0f);

            components::text(ui, fid + ".info")
                .position(kCardPad, 42.0f)
                .size(inner - iconsW, kCardIconW)
                .text(cardInfoText(task))
                .fontSize(10.0f)
                .lineHeight(kCardIconW)
                .maxWidth(inner - iconsW)
                .color(theme.metaText)
                .build();

            // 从右往左摆放：状态操作（打开所在文件夹/打开/取消/暂停）在右，
            // 通用操作（删除/复制链接）在左，阅读顺序为左→右。
            const float btnY = 42.0f;
            float bx = cardWidth - kCardPad;
            const auto place = [&](const std::string& aid, unsigned int icon,
                                   bool primary, std::function<void()> cb) {
                bx -= kCardIconW;
                drawCardAction(ui, fid + "." + aid, bx, btnY, icon, primary, theme,
                               std::move(cb));
            };
            if (showOpenFolder) {
                place("openfolder", 0xF07C, false,  // fa-folder-open
                      [path = task.destPath] { openContainingFolder(path); });
            }
            if (showOpen) {
                place("open", 0xF08E, false,  // fa-external-link
                      [path = task.destPath] { openFile(path); });
            }
            if (showCancel) {
                place("cancel", 0xF00D, false,  // fa-times
                      [id = task.id] { g_manager.cancel(id); });
            }
            if (showResume) {
                place("resume", 0xF04B, true,  // fa-play
                      [id = task.id] { g_manager.resume(id); });
            }
            if (showPause) {
                place("pause", 0xF04C, true,  // fa-pause
                      [id = task.id] { g_manager.pause(id); });
            }
            place("delete", 0xF1F8, false,  // fa-trash
                  [id = task.id] { g_manager.remove(id); });
            place("copy", 0xF0C5, false,  // fa-copy
                  [url = task.url] {
                      core::window::setClipboardText(url);
                      showStatus("已复制链接");
                  });
        })
        .build();
}

// ---------------------------------------------------------- 应用页 / 布局 --

enum class Page { Downloads, Settings };
Page g_page_view = Page::Downloads;  // 默认打开下载列表

// 前向声明：分页大小选择器在 compose() 之后定义。
void buildPageSizePicker(eui::Ui& ui, const std::string& id, float width, float height,
                         const AppTheme& theme);

// --------------------------------------------------------- 上拉分页大小选择 --
//
// eui 的 components::dropdown 只会向下弹出，放在底部翻页行时弹层会超出窗口下缘。
// 这里按官方 SKILL 的“小 builder 复用”模式，做一个向上弹出的选择器：字段 +
// 向上展开的 popup，样式取自当前主题 tokens，与全局配色保持一致。

void buildPageSizePicker(eui::Ui& ui, const std::string& id, float width, float height,
                         const AppTheme& theme) {
    static const char* kLabels[] = {"5 条/页", "10 条/页", "20 条/页", "50 条/页", "100 条/页"};
    constexpr int kCount = 5;
    const float itemHeight = 22.0f;
    const float popupPad = 3.0f;
    const float popupGap = 3.0f;
    const float popupHeight = itemHeight * kCount + popupPad * 2.0f;
    const int selected = pageSizeIndex();
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);

    ui.stack(id)
        .size(width, height)
        .zIndex(30)
        .content([&] {
            // ---- 字段（点击切换展开/收起）----
            ui.rect(id + ".field")
                .size(width, height)
                .color(tokens.surface)
                .radius(8.0f)
                .border(1.0f, components::theme::withOpacity(tokens.border, 0.78f))
                .transition(transition)
                .onClick([] { g_pageSizeOpen = !g_pageSizeOpen; })
                .build();

            ui.text(id + ".label")
                .x(9.0f)
                .size(width - 30.0f, height)
                .text(kLabels[selected])
                .fontSize(11.0f)
                .lineHeight(height)
                .color(tokens.text)
                .verticalAlign(core::VerticalAlign::Center)
                .build();

            ui.text(id + ".chevron")
                .x(width - 20.0f)
                .size(14.0f, height)
                .icon(g_pageSizeOpen ? 0xF077 : 0xF078)  // chevron-up / chevron-down
                .fontSize(10.0f)
                .lineHeight(height)
                .color(tokens.primary)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center)
                .build();

            // ---- 向上弹出的列表（在字段上方展开）----
            if (g_pageSizeOpen) {
                ui.stack(id + ".popup")
                    .y(-(popupHeight + popupGap))
                    .size(width, popupHeight)
                    .zIndex(31)
                    .content([&] {
                        ui.rect(id + ".popup.bg")
                            .size(width, popupHeight)
                            .color(tokens.dark ? tokens.surfaceActive : tokens.surface)
                            .radius(8.0f)
                            .border(1.0f, components::theme::withOpacity(tokens.border, 0.78f))
                            .build();

                        for (int i = 0; i < kCount; ++i) {
                            const float itemY = popupPad + i * itemHeight;
                            ui.rect(id + ".item." + std::to_string(i))
                                .x(popupPad)
                                .y(itemY)
                                .size(width - popupPad * 2.0f, itemHeight)
                                .states({0.0f, 0.0f, 0.0f, 0.0f}, tokens.surfaceHover,
                                        tokens.surfaceActive)
                                .radius(5.0f)
                                .onClick([i] {
                                    g_pageSize = kPageSizes[i];
                                    g_page = 1;
                                    g_pageSizeOpen = false;
                                })
                                .build();

                            ui.text(id + ".item.label." + std::to_string(i))
                                .x(popupPad + 8.0f)
                                .y(itemY)
                                .size(width - popupPad * 2.0f - 16.0f, itemHeight)
                                .text(kLabels[i])
                                .fontSize(11.0f)
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
void drawSidebarItem(eui::Ui& ui, const std::string& id, float x, float y,
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
        .radius(8.0f)
        .transition(transition)
        .build();

    if (active) {
        ui.rect(id + ".bar")
            .position(x, y + 6.0f)
            .size(3.0f, height - 12.0f)
            .color(tokens.primary)
            .radius(1.5f)
            .build();
    }

    // 点击命中区（悬停反馈；激活项不再叠加 hover 底色）。
    ui.rect(id + ".hit")
        .position(x, y)
        .size(width, height)
        .states(idle, active ? idle : tokens.surfaceHover, tokens.surfaceActive)
        .radius(8.0f)
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    // 图标。
    ui.text(id + ".icon")
        .position(x + 8.0f, y)
        .size(16.0f, height)
        .icon(icon)
        .fontSize(11.0f)
        .lineHeight(height)
        .color(textColor)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // 文字。
    ui.text(id + ".label")
        .position(x + 30.0f, y)
        .size(width - 36.0f, height)
        .text(label)
        .fontSize(12.0f)
        .lineHeight(height)
        .color(textColor)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}

// 主侧边栏（图标栏）列表项：仅图标、无文字，激活时主色高亮 + 左侧竖条。
void drawRailItem(eui::Ui& ui, const std::string& id, float y, float railWidth,
                  unsigned int icon, bool active, const AppTheme& theme,
                  std::function<void()> onClick) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color idle = {0.0f, 0.0f, 0.0f, 0.0f};
    const core::Color activeFill =
        components::theme::withAlpha(tokens.primary, theme.dark ? 0.22f : 0.14f);
    const core::Color iconColor = active ? tokens.primary : tokens.text;
    const float itemW = railWidth - 8.0f;   // 两侧各留 4px
    const float x = (railWidth - itemW) * 0.5f;

    // 激活高亮底色。
    ui.rect(id + ".bg")
        .position(x, y)
        .size(itemW, 24.0f)
        .color(active ? activeFill : idle)
        .radius(7.0f)
        .transition(transition)
        .build();

    if (active) {
        ui.rect(id + ".bar")
            .position(0, y + 4.0f)
            .size(2.0f, 16.0f)
            .color(tokens.primary)
            .radius(1.0f)
            .build();
    }

    // 点击命中区（悬停反馈）。
    ui.rect(id + ".hit")
        .position(x, y)
        .size(itemW, 24.0f)
        .states(idle, active ? idle : tokens.surfaceHover, tokens.surfaceActive)
        .radius(7.0f)
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    // 图标（水平居中）。
    ui.text(id + ".icon")
        .position(0, y)
        .size(railWidth, 24.0f)
        .icon(icon)
        .fontSize(13.0f)
        .lineHeight(24.0f)
        .color(iconColor)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}

} // namespace

namespace app {

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("TinyNext 下载器")
        .pageId("tinynext")
        .clearColor({0.075f, 0.085f, 0.105f, 1.0f})
        .windowSize(920, 620)
        .fps(90.0)
        .showDebugStatsInTitle(false)
        .textFont("JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf")
        .iconFont("FontAwesome7.otf");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    const AppTheme& theme = currentTheme();

    // ---- 数据：筛选 + 分页切片（compose 每帧重跑，跟随下载线程实时刷新）----
    const auto tasks = g_manager.snapshot();
    std::vector<dl::TaskView> filtered;
    filtered.reserve(tasks.size());
    for (const auto& task : tasks) {
        if (stateMatches(g_filter, task.state)) {
            filtered.push_back(task);
        }
    }
    const int totalCount = static_cast<int>(filtered.size());
    const int totalPages = std::max(1, (totalCount + g_pageSize - 1) / g_pageSize);
    g_page = std::clamp(g_page, 1, totalPages);
    const int start = (g_page - 1) * g_pageSize;
    const int end = std::min(totalCount, start + g_pageSize);

    // ---- 布局尺寸：左侧图标栏 + 右侧内容区 ----
    // contentX/contentWidth 按页面不同在分支内计算：设置页紧跟图标栏；
    // 下载页在图标栏后还多一个下载状态子侧边栏。
    const float inputY = 14.0f;
    const float listTop = inputY + kInputHeight + 10.0f;
    const float pagerY = screen.height - kPagerBottomMargin - kPagerHeight;
    const float listHeight = std::max(0.0f, pagerY - listTop - 4.0f);

    // 根用 stack：底层铺满窗口的主题背景（clearColor 在初始化时固化、无法运行时
    // 修改，所以背景色由 compose 每帧重绘，主题切换即时生效），其余控件用
    // .position() 绝对定位 —— eui 的 flex 引擎会压缩/居中固定尺寸子项，导致分页
    // 大小下拉与翻页被裁切；绝对定位则完全可控、随窗口高度自适应。
    ui.stack("root")
        .size(screen.width, screen.height)
        .onFrame([](float deltaSeconds) {
            if (g_statusTimer > 0.0f) {
                g_statusTimer -= deltaSeconds;
            }
        })
        .content([&] {
            ui.rect("theme.background")
                .position(0, 0)
                .size(screen.width, screen.height)
                .color(theme.components.background)
                .build();

            // ===================== 主侧边栏（图标栏） =====================
            // 只放图标：左上角应用 logo（项目名称，特例做成圆角图标块），
            // 下面是各应用页图标，底部是主题切换按钮 —— 无文字，保持简洁。
            // 下载状态筛选不在主侧边栏，而是下载页内容区左侧自己的子侧边栏。
            ui.stack("sidebar")
                .position(0, 0)
                .size(kRailWidth, screen.height)
                .zIndex(5)
                .content([&] {
                    ui.rect("sidebar.bg")
                        .position(0, 0)
                        .size(kRailWidth, screen.height)
                        .color(theme.components.surface)
                        .build();

                    // 应用 logo：项目名缩写 "TN"（TinyNext），主色圆角块特例。
                    ui.rect("sidebar.logo.bg")
                        .position(4.0f, 10.0f)
                        .size(18.0f, 18.0f)
                        .color(theme.components.primary)
                        .radius(5.0f)
                        .build();
                    ui.text("sidebar.logo")
                        .position(0, 10.0f)
                        .size(kRailWidth, 18.0f)
                        .text("TN")
                        .fontSize(8.0f)
                        .lineHeight(18.0f)
                        .color(theme.dark ? theme.components.surface
                                          : theme.components.background)
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .verticalAlign(core::VerticalAlign::Center)
                        .build();

                    // 应用页导航：下载列表（默认第一页）/ 设置。
                    float railY = 40.0f;
                    drawRailItem(ui, "nav.downloads", railY, kRailWidth, 0xF03A,
                                 g_page_view == Page::Downloads, theme,
                                 [] { g_page_view = Page::Downloads; });
                    railY += 30.0f;
                    drawRailItem(ui, "nav.settings", railY, kRailWidth, 0xF013,
                                 g_page_view == Page::Settings, theme,
                                 [] { g_page_view = Page::Settings; });

                    // 主题切换：底部，仅图标（月亮/太阳）。
                    components::button(ui, "theme.toggle")
                        .position((kRailWidth - 22.0f) * 0.5f, screen.height - 28.0f)
                        .size(22.0f, 22.0f)
                        .icon(g_dark ? 0xF186 : 0xF185)  // moon / sun
                        .text("")
                        .iconSize(11.0f)
                        .theme(theme.components, false)
                        .onClick([] { g_dark = !g_dark; })
                        .build();
                })
                .build();

            // ===================== 内容区 =====================
            if (g_page_view == Page::Downloads) {
                // 下载页布局：左侧是下载状态子侧边栏（下载页内部布局，普通
                // 列表），右侧是输入栏 + 卡片任务列表 + 翻页控件组。
                const float railRight = kRailWidth + kMargin;            // 图标栏右侧
                const float subX = railRight;                            // 状态子侧边栏
                const float contentX = subX + kSubSidebarWidth + 10.0f;  // 列表区起点
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
                            .radius(8.0f)
                            .build();

                        components::text(ui, "sub.filter.label")
                            .position(9.0f, 10.0f)
                            .size(kSubSidebarWidth - 18.0f, 14.0f)
                            .text("下载状态")
                            .fontSize(10.0f)
                            .lineHeight(14.0f)
                            .color(theme.metaText)
                            .build();

                        const float itemW = kSubSidebarWidth - 12.0f;
                        float itemY = 28.0f;
                        drawSidebarItem(ui, "filter.all", 6.0f, itemY, itemW, 22.0f,
                                        "所有", 0xF03A, g_filter == Filter::All, theme,
                                        [] { g_filter = Filter::All; g_page = 1; });
                        itemY += 27.0f;
                        drawSidebarItem(ui, "filter.active", 6.0f, itemY, itemW, 22.0f,
                                        "下载中", 0xF019, g_filter == Filter::Active, theme,
                                        [] { g_filter = Filter::Active; g_page = 1; });
                        itemY += 27.0f;
                        drawSidebarItem(ui, "filter.done", 6.0f, itemY, itemW, 22.0f,
                                        "已完成", 0xF00C, g_filter == Filter::Done, theme,
                                        [] { g_filter = Filter::Done; g_page = 1; });
                    })
                    .build();

                // ---- 添加下载：右上角一个 ➕ 图标，点击弹出对话框 ----
                components::button(ui, "add.btn")
                    .position(contentX + contentWidth - 28.0f, inputY)
                    .size(28.0f, kInputHeight)
                    .icon(0xF067)  // fa-plus
                    .text("")
                    .iconSize(13.0f)
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
                        .position(contentX, listTop + 16.0f)
                        .size(contentWidth, 24.0f)
                        .text(hint)
                        .fontSize(12.0f)
                        .lineHeight(24.0f)
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
                constexpr float kChevWidth = 18.0f;
                constexpr float kPageLabelWidth = 54.0f;
                constexpr float kPagerGap = 4.0f;
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
                            .size(kChevWidth, 20.0f)
                            .icon(0xF053)  // chevron-left
                            .text("")
                            .iconSize(11.0f)
                            .theme(theme.components, false)
                            .disabled(g_page <= 1)
                            .onClick([] { if (g_page > 1) --g_page; })
                            .build();

                        components::text(ui, "pager.label")
                            .size(kPageLabelWidth, kPagerHeight)
                            .text(std::format("第 {} / {} 页", g_page, totalPages))
                            .fontSize(11.0f)
                            .lineHeight(kPagerHeight)
                            .horizontalAlign(core::HorizontalAlign::Center)
                            .color(theme.metaText)
                            .build();

                        components::button(ui, "pager.next")
                            .size(kChevWidth, 20.0f)
                            .icon(0xF054)  // chevron-right
                            .text("")
                            .iconSize(11.0f)
                            .theme(theme.components, false)
                            .disabled(g_page >= totalPages)
                            .onClick([totalPages] { if (g_page < totalPages) ++g_page; })
                            .build();

                        // 分页大小（5/10/20/50/100 上拉选择器），跟在下一页后面。
                        buildPageSizePicker(ui, "pager.pageSize", kSizeDropdownWidth,
                                            kPagerHeight, theme);
                    })
                    .build();

                // ---- 状态消息（短暂显示，翻页行上方）----
                if (g_statusTimer > 0.0f && !g_statusMessage.empty()) {
                    components::text(ui, "status")
                        .position(contentX, pagerY - 24.0f)
                        .size(contentWidth, 18.0f)
                        .text(g_statusMessage)
                        .fontSize(12.0f)
                        .lineHeight(18.0f)
                        .color(theme.statusText)
                        .build();
                }

                // ---- 添加下载弹窗（模态）：链接输入 + 提交/取消 ----
                if (g_addOpen) {
                    const float dlgW = 280.0f;
                    const float dlgH = 150.0f;
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
                                .radius(10.0f)
                                .border(1.0f,
                                        components::theme::withOpacity(
                                            theme.components.border, 0.6f))
                                .build();

                            components::text(ui, "add.dialog.title")
                                .position(16.0f, 12.0f)
                                .size(dlgW - 32.0f, 20.0f)
                                .text("添加下载")
                                .fontSize(14.0f)
                                .lineHeight(20.0f)
                                .color(theme.titleText)
                                .build();

                            components::input(ui, "add.url")
                                .position(16.0f, 40.0f)
                                .size(dlgW - 32.0f, 28.0f)
                                .placeholder("https://example.com/file.zip")
                                .value(g_urlText)
                                .theme(theme.components)
                                .onChange([](const std::string& value) { g_urlText = value; })
                                .onEnter([] { if (addDownload()) g_addOpen = false; })
                                .build();

                            components::button(ui, "add.cancel")
                                .position(dlgW - 16.0f - 76.0f - 8.0f - 76.0f, 108.0f)
                                .size(76.0f, 26.0f)
                                .text("取消")
                                .fontSize(12.0f)
                                .theme(theme.components, false)
                                .onClick([] { g_addOpen = false; })
                                .build();

                            components::button(ui, "add.submit")
                                .position(dlgW - 16.0f - 76.0f, 108.0f)
                                .size(76.0f, 26.0f)
                                .text("提交")
                                .fontSize(12.0f)
                                .theme(theme.components, true)
                                .onClick([] { if (addDownload()) g_addOpen = false; })
                                .build();
                        })
                        .build();
                }
            } else {
                // ===================== 设置页：基础信息 =====================
                // 设置页没有下载状态子侧边栏，内容区紧跟图标栏右侧。
                const float contentX = kRailWidth + kMargin;
                const float contentWidth = screen.width - contentX - kMargin;
                const float infoX = contentX + 8.0f;
                components::text(ui, "settings.title")
                    .position(infoX, 16.0f)
                    .size(contentWidth - 16.0f, 24.0f)
                    .text("设置")
                    .fontSize(17.0f)
                    .lineHeight(24.0f)
                    .color(theme.titleText)
                    .build();

                components::text(ui, "settings.subtitle")
                    .position(infoX, 42.0f)
                    .size(contentWidth - 16.0f, 16.0f)
                    .text("目前暂无需要设置的选项，以下为应用基本信息。")
                    .fontSize(11.0f)
                    .lineHeight(16.0f)
                    .color(theme.hintText)
                    .build();

                struct InfoRow { const char* label; const char* value; };
                static const InfoRow kInfoRows[] = {
                    {"应用名称", "TinyNext 下载器"},
                    {"版本", "0.1.0"},
                    {"界面框架", "EUI-NEO 0.5.3"},
                    {"网络库", "tinyhttps 0.2.9"},
                    {"构建工具", "mcpp（C++23）"},
                    {"默认保存目录", "downloads/"},
                };
                const float labelW = 90.0f;
                float rowY = 66.0f;
                for (const auto& row : kInfoRows) {
                    components::text(ui, std::format("settings.k{}.label", row.label))
                        .position(infoX, rowY)
                        .size(labelW, 22.0f)
                        .text(row.label)
                        .fontSize(12.0f)
                        .lineHeight(22.0f)
                        .color(theme.metaText)
                        .build();
                    components::text(ui, std::format("settings.k{}.value", row.label))
                        .position(infoX + labelW, rowY)
                        .size(contentWidth - 16.0f - labelW, 22.0f)
                        .text(row.value)
                        .fontSize(12.0f)
                        .lineHeight(22.0f)
                        .color(theme.nameText)
                        .build();
                    rowY += 26.0f;
                }
            }
        })
        .build();
}

} // namespace app
