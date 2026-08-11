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
            // 卡片底：圆角表面 + 细边框 + 柔和投影（岛屿卡片风：与内容大卡分层）。
            ui.rect(fid + ".bg")
                .position(0, 0)
                .size(cardWidth, kCardHeight)
                .color(theme.components.surface)
                .radius(8.0f)
                .border(1.0f, components::theme::withOpacity(theme.components.border, 0.55f))
                .shadow(8.0f, 2.0f,
                        theme.components.dark
                            ? core::Color{0.0f, 0.0f, 0.0f, 0.18f}
                            : core::Color{0.10f, 0.14f, 0.22f, 0.08f})
                .build();

            // ---- 第 1 行：文件名 + 状态 ----
            const float stateW = 46.0f;
            components::text(ui, fid + ".name")
                .position(kCardPad, 9.0f)
                .size(inner - stateW - 6.0f, 15.0f)
                .text(taskDisplayName(task))
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
            // X（取消）：进行中（排队/下载/暂停）显示，用来取消任务。
            const bool showCancel = task.state == dl::State::Queued ||
                                    task.state == dl::State::Downloading ||
                                    task.state == dl::State::Paused;
            // 垃圾桶（删除）：任务结束后显示，删除下载好的文件或记录。
            // 与 showCancel 互斥（状态不重叠）。
            const bool showDelete = task.state == dl::State::Done ||
                                    task.state == dl::State::Failed ||
                                    task.state == dl::State::Cancelled;
            // 失败/已取消/已完成都提供重新下载（完成的再下走 auto-file-renaming 改名）。
            const bool showRetry = task.state == dl::State::Failed ||
                                   task.state == dl::State::Cancelled ||
                                   task.state == dl::State::Done;
            const bool showOpen = task.state == dl::State::Done;
            // 打开所在文件夹：任何状态都显示（未完成/失败时 openContainingFolder
            // 会回退到打开下载目录，不会报错）。
            const bool showOpenFolder = true;

            const int actionCount = (showOpen ? 1 : 0) + (showOpenFolder ? 1 : 0) +
                                    (showDelete ? 1 : 0) + 1 /* 复制链接恒显示 */ +
                                    (showCancel ? 1 : 0) + (showRetry ? 1 : 0) +
                                    (showPause || showResume ? 1 : 0);
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

            // 从右往左摆放（place 递减 bx，先调用的在最右）。左→右阅读顺序：
            // 进行中任务：开始/暂停在最左，接着所在文件夹/复制链接，最右是取消。
            // 已完成任务：打开文件/所在文件夹/复制链接成组，接着重新下载，最后删除。
            // 全部普通颜色（无主色），与同类按钮一致。
            const float btnY = 42.0f;
            float bx = cardWidth - kCardPad;
            const auto place = [&](const std::string& aid, unsigned int icon,
                                   bool primary, std::function<void()> cb) {
                bx -= kCardIconW;
                drawCardAction(ui, fid + "." + aid, bx, btnY, icon, primary, theme,
                               std::move(cb));
            };
            if (showDelete) {
                place("delete", 0xF1F8, false,  // fa-trash（仅任务结束后显示）
                      [task = task] { requestDelete(task); });
            }
            if (showRetry) {
                place("retry", 0xF01E, false,  // fa-redo（普通颜色，与同类一致）
                      [id = task.id] { g_manager->retry(id); });
            }
            if (showCancel) {
                // X 与垃圾桶同一效果：弹删除确认框（问是否删除任务 + 勾选删源文件）。
                place("cancel", 0xF00D, false,  // fa-times
                      [task = task] { requestDelete(task); });
            }
            place("copy", 0xF0C1, false,  // fa-link（复制链接；fa-copy 0xF0C5 像复制文件）
                  [url = task.url] {
                      core::window::setClipboardText(url);
                      showStatus("已复制链接");
                  });
            if (showOpenFolder) {
                place("openfolder", 0xF07C, false,  // fa-folder-open
                      [path = task.destPath] { openContainingFolder(path); });
            }
            if (showOpen) {
                // 放最后 → 最左：下载完成后的「打开文件」主入口。
                place("open", 0xF08E, false,  // fa-external-link
                      [path = task.destPath] { openFile(path); });
            }
            if (showResume) {
                // 放最后 → 最左：进行中任务的主操作；普通颜色与同类一致。
                place("resume", 0xF04B, false,  // fa-play
                      [id = task.id] { g_manager->resume(id); });
            }
            if (showPause) {
                place("pause", 0xF04C, false,  // fa-pause
                      [id = task.id] { g_manager->pause(id); });
            }
        })
        .build();
}
