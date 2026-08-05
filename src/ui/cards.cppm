// ui/cards.cppm — the download task card (filename + status, progress, info
// row with per-state icon actions).
module;

#include "eui_ui.h"

export module tinynext.ui.cards;

import std;
import tinynext.download_engine;
import tinynext.ui.theme;
import tinynext.ui.utils;
import tinynext.ui.widgets;
import tinynext.ui.state;
import tinynext.ui.platform;

export eui::Color stateColor(dl::State state) {
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
export std::string stateLabel(dl::State state) {
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
export std::string cardInfoText(const dl::TaskView& task) {
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
    if (task.connections > 0) {
        push(std::format("{} 连接", task.connections));
    }
    if (task.totalBytes > 0) {
        push(std::format("{} / {}", formatBytes(task.downloadedBytes),
                         formatBytes(task.totalBytes)));
    }
    // ETA：剩余字节 / 当前速度（需知道总量且有速度）。
    if (task.totalBytes > 0 && task.speedBps > 0.0) {
        const std::int64_t remaining = task.totalBytes - task.downloadedBytes;
        if (remaining > 0) {
            const std::int64_t seconds =
                static_cast<std::int64_t>(remaining / task.speedBps);
            if (seconds < 60) {
                push(std::format("剩余 {}s", seconds));
            } else if (seconds < 3600) {
                push(std::format("剩余 {}m {:02d}s", seconds / 60, seconds % 60));
            } else {
                push(std::format("剩余 {}h {:02d}m", seconds / 3600,
                                 (seconds % 3600) / 60));
            }
        }
    }
    return parts.empty() ? "下载中" : parts;
}

// 卡片式下载项：名称、进度、各种信息在卡片内纵向排布。
// 卡片作为 scrollview 纵向流的一个子项；卡片内部用绝对定位布局三行：
//   第 1 行  文件名（左）+ 状态标签（右）
//   第 2 行  进度条（横贯卡片）
//   第 3 行  信息文本（左）+ 操作按钮（右）
export void drawTaskCard(eui::Ui& ui, const dl::TaskView& task, float cardWidth) {
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
                .radius(S(8.0f))
                .border(1.0f, components::theme::withOpacity(theme.components.border, 0.55f))
                .build();

            // ---- 第 1 行：文件名 + 状态 ----
            const float stateW = S(46.0f);
            components::text(ui, fid + ".name")
                .position(kCardPad, S(9.0f))
                .size(inner - stateW - S(6.0f), S(15.0f))
                .text(fileNameFromUrl(task.url))
                .fontSize(S(13.0f))
                .lineHeight(S(15.0f))
                .maxWidth(inner - stateW - S(6.0f))
                .color(theme.nameText)
                .build();
            components::text(ui, fid + ".state")
                .position(cardWidth - kCardPad - stateW, S(9.0f))
                .size(stateW, S(15.0f))
                .text(stateLabel(task.state))
                .fontSize(S(10.0f))
                .lineHeight(S(15.0f))
                .horizontalAlign(core::HorizontalAlign::Right)
                .color(stateColor(task.state))
                .build();

            // ---- 第 2 行：进度条 ----
            ui.stack(fid + ".progress.slot")
                .position(kCardPad, S(28.0f))
                .size(inner, S(6.0f))
                .content([&] {
                    components::progress(ui, fid + ".progress")
                        .size(inner, S(6.0f))
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
            const bool showRetry = task.state == dl::State::Failed ||
                                   task.state == dl::State::Cancelled;
            const bool showOpen = task.state == dl::State::Done;
            const bool showOpenFolder = task.state == dl::State::Done;

            const int actionCount = 2 + (showPause || showResume ? 1 : 0) +
                                    (showCancel ? 1 : 0) +
                                    (showRetry ? 1 : 0) +
                                    (showOpen ? 1 : 0) + (showOpenFolder ? 1 : 0);
            const float iconsW = actionCount * kCardIconW +
                                 (actionCount > 0 ? (actionCount - 1) * kCardIconGap : 0.0f);

            components::text(ui, fid + ".info")
                .position(kCardPad, S(42.0f))
                .size(inner - iconsW, kCardIconW)
                .text(cardInfoText(task))
                .fontSize(S(10.0f))
                .lineHeight(kCardIconW)
                .maxWidth(inner - iconsW)
                .color(theme.metaText)
                .build();

            // 从右往左摆放：状态操作（打开所在文件夹/打开/取消/暂停）在右，
            // 通用操作（删除/复制链接）在左，阅读顺序为左→右。
            const float btnY = S(42.0f);
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
                      [id = task.id] { g_manager->cancel(id); });
            }
            if (showRetry) {
                place("retry", 0xF01E, true,  // fa-redo
                      [id = task.id] { g_manager->retry(id); });
            }
            if (showResume) {
                place("resume", 0xF04B, true,  // fa-play
                      [id = task.id] { g_manager->resume(id); });
            }
            if (showPause) {
                place("pause", 0xF04C, true,  // fa-pause
                      [id = task.id] { g_manager->pause(id); });
            }
            place("delete", 0xF1F8, false,  // fa-trash
                  [id = task.id] { g_manager->remove(id); });
            place("copy", 0xF0C5, false,  // fa-copy
                  [url = task.url] {
                      core::window::setClipboardText(url);
                      showStatus("已复制链接");
                  });
        })
        .build();
}
