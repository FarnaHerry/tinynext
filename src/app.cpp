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

#include "download_manager.hpp"

namespace {

constexpr float kMargin = 20.0f;
constexpr float kInputY = 56.0f;
constexpr float kInputHeight = 40.0f;
constexpr float kAddButtonWidth = 100.0f;
constexpr float kListTop = 118.0f;
constexpr float kRowHeight = 58.0f;
// Row columns (x offsets are relative to kMargin; row content is 800px wide).
constexpr float kNameWidth = 180.0f;
constexpr float kProgressX = 190.0f;
constexpr float kProgressWidth = 300.0f;
constexpr float kPctX = 498.0f;
constexpr float kInfoX = 558.0f;
constexpr float kPauseButtonX = 668.0f;
constexpr float kPauseButtonWidth = 60.0f;
constexpr float kCancelButtonX = 732.0f;
constexpr float kCancelButtonWidth = 68.0f;

dl::DownloadManager g_manager;
std::string g_urlText;
std::string g_statusMessage;
float g_statusTimer = 0.0f;

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

void openInExplorer(const std::filesystem::path& path) {
#ifdef _WIN32
    std::system(("explorer /select,\"" + path.string() + "\"").c_str());
#else
    (void)path;
#endif
}

// ----------------------------------------------------------- UI callbacks --

void addDownload() {
    std::string url = g_urlText;
    const std::size_t first = url.find_first_not_of(" \t\r\n");
    const std::size_t last = url.find_last_not_of(" \t\r\n");
    url = first == std::string::npos ? "" : url.substr(first, last - first + 1);
    if (url.empty()) {
        showStatus("请输入下载地址");
        return;
    }
    if (!url.starts_with("https://")) {
        if (url.starts_with("http://")) {
            // tinyhttps only speaks HTTPS; best-effort upgrade.
            url = "https://" + url.substr(7);
        } else {
            showStatus("仅支持 HTTPS 下载链接");
            return;
        }
    }

    const std::filesystem::path dest =
        std::filesystem::path("downloads") / fileNameFromUrl(url);
    const std::uint64_t id = g_manager.start(url, dest);
    showStatus(std::format("已开始下载 #{} — {}", id, dest.filename().string()));
}

// --------------------------------------------------------------- rendering --

eui::Color infoColor(dl::State state) {
    switch (state) {
        case dl::State::Downloading: return {0.58f, 0.72f, 0.95f, 1.0f};
        case dl::State::Paused:      return {0.95f, 0.72f, 0.30f, 1.0f};
        case dl::State::Done:        return {0.35f, 0.80f, 0.45f, 1.0f};
        case dl::State::Failed:      return {0.92f, 0.40f, 0.38f, 1.0f};
        case dl::State::Queued:
        case dl::State::Cancelled:   return {0.55f, 0.58f, 0.62f, 1.0f};
    }
    return {0.55f, 0.58f, 0.62f, 1.0f};
}

std::string infoText(const dl::TaskView& task) {
    switch (task.state) {
        case dl::State::Queued:      return "等待中";
        case dl::State::Downloading: return formatSpeed(task.speedBps);
        case dl::State::Paused:      return "已暂停";
        case dl::State::Done:        return "完成";
        case dl::State::Cancelled:   return "已取消";
        case dl::State::Failed: {
            std::string error = task.error;
            if (error.size() > 12) {
                error = error.substr(0, 12) + "…";
            }
            return error;
        }
    }
    return "";
}

void drawTaskRow(eui::Ui& ui, const dl::TaskView& task, float y) {
    const std::string fid = "task." + std::to_string(task.id);
    const float textY = y + 4.0f;
    const float textHeight = kRowHeight - 8.0f;

    // Filename (left, ellipsized by maxWidth).
    components::text(ui, fid + ".name")
        .position(kMargin, textY)
        .size(kNameWidth, textHeight)
        .text(fileNameFromUrl(task.url))
        .fontSize(15.0f)
        .lineHeight(20.0f)
        .maxWidth(kNameWidth)
        .color({0.88f, 0.92f, 1.0f, 1.0f})
        .build();

    // Progress bar — the component itself has no position, so park it in a
    // positioned slot stack.
    float progress = 0.0f;
    if (task.state == dl::State::Done) {
        progress = 1.0f;
    } else if (task.totalBytes > 0) {
        progress = static_cast<float>(
            static_cast<double>(task.downloadedBytes) / static_cast<double>(task.totalBytes));
        progress = std::clamp(progress, 0.0f, 1.0f);
    }
    ui.stack(fid + ".progress.slot")
        .position(kMargin + kProgressX, y + (kRowHeight - 8.0f) * 0.5f - 4.0f)
        .size(kProgressWidth, 8.0f)
        .content([&] {
            components::progress(ui, fid + ".progress")
                .size(kProgressWidth, 8.0f)
                .value(progress)
                .build();
        })
        .build();

    // Percent.
    std::string percent = "—";
    if (task.totalBytes > 0) {
        percent = std::format("{:.0f}%", progress * 100.0);
    } else if (task.downloadedBytes > 0) {
        percent = formatBytes(task.downloadedBytes);
    }
    components::text(ui, fid + ".pct")
        .position(kMargin + kPctX, textY)
        .size(52.0f, textHeight)
        .text(percent)
        .fontSize(14.0f)
        .lineHeight(20.0f)
        .color({0.62f, 0.70f, 0.82f, 1.0f})
        .build();

    // Info column: speed while downloading, otherwise state/error.
    components::text(ui, fid + ".info")
        .position(kMargin + kInfoX, textY)
        .size(100.0f, textHeight)
        .text(infoText(task))
        .fontSize(14.0f)
        .lineHeight(20.0f)
        .color(infoColor(task.state))
        .build();

    const float buttonY = y + (kRowHeight - 26.0f) * 0.5f;

    // Pause / resume button — the primary toggle for active tasks.
    if (task.state == dl::State::Downloading) {
        components::button(ui, fid + ".pause")
            .position(kMargin + kPauseButtonX, buttonY)
            .size(kPauseButtonWidth, 26.0f)
            .text("暂停")
            .fontSize(13.0f)
            .theme(components::theme::dark(), false)
            .onClick([id = task.id] { g_manager.pause(id); })
            .build();
    } else if (task.state == dl::State::Paused) {
        components::button(ui, fid + ".resume")
            .position(kMargin + kPauseButtonX, buttonY)
            .size(kPauseButtonWidth, 26.0f)
            .text("继续")
            .fontSize(13.0f)
            .theme(components::theme::dark(), true)
            .onClick([id = task.id] { g_manager.resume(id); })
            .build();
    }

    // Cancel / open button.
    if (task.state == dl::State::Queued || task.state == dl::State::Downloading ||
        task.state == dl::State::Paused) {
        components::button(ui, fid + ".cancel")
            .position(kMargin + kCancelButtonX, buttonY)
            .size(kCancelButtonWidth, 26.0f)
            .text("取消")
            .fontSize(13.0f)
            .theme(components::theme::dark(), false)
            .onClick([id = task.id] { g_manager.cancel(id); })
            .build();
    } else if (task.state == dl::State::Done) {
        components::button(ui, fid + ".open")
            .position(kMargin + kCancelButtonX, buttonY)
            .size(kCancelButtonWidth, 26.0f)
            .text("打开")
            .fontSize(13.0f)
            .theme(components::theme::dark(), false)
            .onClick([path = task.destPath] { openInExplorer(path); })
            .build();
    }
}

} // namespace

namespace app {

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("TinyNext 下载器")
        .pageId("tinynext")
        .clearColor({0.075f, 0.085f, 0.105f, 1.0f})
        .windowSize(840, 600)
        .fps(90.0)
        .showDebugStatsInTitle(false)
        .textFont("JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf")
        .iconFont("Font Awesome 7 Free-Solid-900.otf");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    // A node carrying onFrame() keeps the runtime animating, which makes the
    // loop render continuously and re-compose every frame — that is what lets
    // the progress bars track the download threads in real time.
    ui.stack("root")
        .size(screen.width, screen.height)
        .onFrame([](float deltaSeconds) {
            if (g_statusTimer > 0.0f) {
                g_statusTimer -= deltaSeconds;
            }
        })
        .content([&] {
            components::text(ui, "title")
                .position(kMargin, 12.0f)
                .size(500.0f, 30.0f)
                .text("TinyNext 下载器")
                .fontSize(22.0f)
                .lineHeight(30.0f)
                .color({0.94f, 0.97f, 1.0f, 1.0f})
                .build();

            const float inputWidth =
                screen.width - kMargin * 2.0f - kAddButtonWidth - 12.0f;

            components::input(ui, "url.input")
                .position(kMargin, kInputY)
                .size(inputWidth, kInputHeight)
                .placeholder("https://example.com/file.zip（仅支持 HTTPS）")
                .value(g_urlText)
                .onChange([](const std::string& value) { g_urlText = value; })
                .onEnter([] { addDownload(); })
                .build();

            components::button(ui, "add.btn")
                .position(kMargin + inputWidth + 12.0f, kInputY)
                .size(kAddButtonWidth, kInputHeight)
                .text("下载")
                .onClick([] { addDownload(); })
                .build();

            const auto tasks = g_manager.snapshot();
            if (tasks.empty()) {
                components::text(ui, "empty.hint")
                    .position(kMargin, kListTop + 26.0f)
                    .size(screen.width - kMargin * 2.0f, 30.0f)
                    .text("暂无下载任务 — 粘贴链接后点击「下载」")
                    .fontSize(15.0f)
                    .lineHeight(30.0f)
                    .color({0.42f, 0.47f, 0.55f, 1.0f})
                    .build();
            } else {
                float y = kListTop;
                for (const auto& task : tasks) {
                    drawTaskRow(ui, task, y);
                    y += kRowHeight;
                }
            }

            if (g_statusTimer > 0.0f && !g_statusMessage.empty()) {
                components::text(ui, "status")
                    .position(kMargin, screen.height - 34.0f)
                    .size(screen.width - kMargin * 2.0f, 24.0f)
                    .text(g_statusMessage)
                    .fontSize(14.0f)
                    .lineHeight(24.0f)
                    .color({0.72f, 0.83f, 0.97f, 1.0f})
                    .build();
            }
        })
        .build();
}

} // namespace app
