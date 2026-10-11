// ui/cards.cppm — the download task card (filename, progress, info
// row with per-state icon actions).
module;

#include "eui_ui.h"

export module tinynext.ui.cards;

import std;
import tinynext.download_engine;
import tinynext.i18n;           // tr / trf（状态标签/信息行）
import tinynext.ui.theme;
import tinynext.ui.utils;
import tinynext.ui.widgets;
import tinynext.store.tasks;    // g_tasks 命令（pause/resume/retry）+ taskDisplayName
import tinynext.store.dialogs;  // requestDelete / 镜像弹窗状态
import tinynext.store.ui;       // showStatus
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

// 任务状态短标签（任务信息弹窗使用）。
export std::string stateLabel(dl::State state) {
    switch (state) {
        case dl::State::Queued:      return tr("card.state.queued");
        case dl::State::Downloading: return tr("card.state.downloading");
        case dl::State::Paused:      return tr("card.state.paused");
        case dl::State::Done:        return tr("card.state.done");
        case dl::State::Cancelled:   return tr("card.state.cancelled");
        case dl::State::Failed:      return tr("card.state.failed");
    }
    return "";
}

namespace {
std::string mediaPhaseLabel(std::string_view phase) {
    if (phase == "waiting") return tr("card.media.waiting");
    if (phase == "probing") return tr("card.media.probing");
    if (phase == "awaiting-selection") return tr("card.media.awaiting_selection");
    if (phase == "downloading") return tr("card.media.downloading");
    if (phase == "recording") return tr("card.media.recording");
    if (phase == "finalizing") return tr("card.media.finalizing");
    if (phase == "paused") return tr("card.media.paused");
    if (phase == "complete") return tr("card.media.complete");
    if (phase == "error") return tr("card.media.error");
    if (phase == "removed") return tr("card.media.removed");
    return tr("card.state.downloading");
}

std::string mediaTime(std::int64_t milliseconds) {
    const std::int64_t total = std::max<std::int64_t>(0, milliseconds / 1000);
    const std::int64_t hours = total / 3600;
    const std::int64_t minutes = (total % 3600) / 60;
    const std::int64_t seconds = total % 60;
    if (hours > 0) return std::format("{:02}:{:02}:{:02}", hours, minutes, seconds);
    return std::format("{:02}:{:02}", minutes, seconds);
}

bool hasSelectableMediaTracks(const dl::TaskView& task) {
    return std::ranges::any_of(task.mediaTracks, [](const dl::MediaTrackView& track) {
        // 类型白名单统一在 dl::isSelectableMediaTrackType（与选择弹窗、CLI 一致）。
        return !track.id.empty() && dl::isSelectableMediaTrackType(track.type);
    });
}
}

// 卡片传输信息：速度 · 已下载/总大小；百分比在状态行独立展示。
export std::string cardInfoText(const dl::TaskView& task) {
    if (task.awaitingTorrentFileSelection) return tr("card.bt.awaiting_files");
    if (task.awaitingMediaTrackSelection) {
        return g_tasks.health().supportsMediaTrackSelection && hasSelectableMediaTracks(task)
            ? tr("card.media.awaiting_selection") : tr("common.unsupported");
    }
    switch (task.state) {
        case dl::State::Queued: return tr("card.state.wait_queue");
        case dl::State::Paused: return tr("card.state.paused");
        case dl::State::Cancelled: return tr("card.state.cancelled");
        case dl::State::Done:
            return task.totalBytes > 0
                ? std::string(tr("card.state.done")) + " · " + formatBytes(task.totalBytes)
                : tr("card.state.done");
        case dl::State::Failed: {
            return task.error.empty() ? tr("card.state.failed") : task.error;
        }
        case dl::State::Downloading:
            break;
    }

    if (task.isMedia) {
        std::string parts;
        const auto push = [&](std::string_view part) {
            if (part.empty()) return;
            if (!parts.empty()) parts += "  ·  ";
            parts += part;
        };
        if (!task.mediaProtocol.empty()) push(task.mediaProtocol);
        push(mediaPhaseLabel(task.mediaState));
        if (task.mediaLive) {
            if (task.mediaDownloadedBytes > 0) push(formatBytes(task.mediaDownloadedBytes));
        } else if (task.mediaDurationMs > 0) {
            push(std::format("{} / {}", mediaTime(task.mediaCompletedDurationMs),
                             mediaTime(task.mediaDurationMs)));
        }
        const std::string speed = formatSpeed(task.speedBps);
        if (!speed.empty()) push(speed);
        return parts.empty() ? tr("card.state.downloading") : parts;
    }

    std::string parts;
    const auto push = [&](std::string_view part) {
        if (!parts.empty()) {
            parts += "  ·  ";
        }
        parts += part;
    };
    if (task.totalBytes <= 0 && task.downloadedBytes > 0) {
        push(formatBytes(task.downloadedBytes));
    }
    const std::string speed = formatSpeed(task.speedBps);
    if (!speed.empty()) push(speed);
    // 源状态告警：镜像任务的源全部失败（aria2 uris 全 error）时提示失效。
    if (!task.mirrors.empty()) {
        bool allFailed = true;
        for (const auto& m : task.mirrors) {
            if (m.status != "error") { allFailed = false; break; }
        }
        if (allFailed) {
            push(tr("card.mirrors_all_failed"));
        }
    }
    if (task.totalBytes > 0) {
        push(std::format("{} / {}", formatBytes(task.downloadedBytes),
                         formatBytes(task.totalBytes)));
    }
    return parts.empty() ? tr("card.state.downloading") : parts;
}

// 紧凑任务卡：名称、状态与百分比、横向进度、传输信息、底部操作。
// 列表仍按任务 id 保留身份；菜单与领域命令沿用原来的状态条件。
export void drawTaskCard(eui::Ui& ui, const dl::TaskView& task, float cardWidth) {
    const AppTheme& theme = currentTheme();
    const std::string fid = "task." + std::to_string(task.id);
    const float nameW = std::max(0.0f, cardWidth - 2.0f * kCardPad);

    // 进度值：已完成视为 1，其余按已下载/总量计算。
    float progress = 0.0f;
    if (task.state == dl::State::Done) {
        progress = 1.0f;
    } else if (task.isMedia) {
        progress = task.mediaLive ? 0.0f : static_cast<float>(task.mediaProgress);
    } else if (task.totalBytes > 0) {
        progress = std::clamp(
            static_cast<float>(static_cast<double>(task.downloadedBytes) /
                               static_cast<double>(task.totalBytes)),
            0.0f, 1.0f);
    }

    progress = std::isfinite(progress) ? std::clamp(progress, 0.0f, 1.0f) : 0.0f;
    const bool knownProgress = task.state == dl::State::Done ||
        (task.isMedia ? !task.mediaLive && task.mediaDurationMs > 0 : task.totalBytes > 0);
    const std::string progressText = task.isMedia && task.mediaLive && task.state != dl::State::Done
        ? "LIVE" : knownProgress ? std::format("{:.0f}%", progress * 100.0f) : "--";
    const auto progressColor = task.state == dl::State::Done ? theme.done :
        task.state == dl::State::Failed ? theme.failed :
        task.state == dl::State::Paused || task.state == dl::State::Cancelled ? theme.idle : theme.primary;

    ui.stack(fid)
        .width(cardWidth)
        .height(kCardHeight)
        .content([&] {
            // 扁平卡：不透明 cardBg + 小圆角 + 1px hairline 描边，无投影
            // （层次靠描边表达）。不做 backdrop blur。
            ui.rect(fid + ".bg")
                .position(0, 0)
                .size(cardWidth, kCardHeight)
                .color(theme.cardBg)
                .radius(kCardRadius)
                .border(kHairline, theme.outline)
                .build();

            // 名称在单行内精确截断；完整名称、链接及错误通过信息入口查看。
            components::text(ui, fid + ".name")
                .position(kCardPad, 14.0f)
                .size(nameW, 20.0f)
                .text(ellipsizeText(taskDisplayName(task), nameW, 13.0f))
                .fontSize(13.0f)
                .lineHeight(20.0f)
                .color(theme.nameText)
                .build();
            components::text(ui, fid + ".state")
                .position(kCardPad, 42.0f)
                .size(nameW - 56.0f, 16.0f)
                .text(stateLabel(task.state))
                .fontSize(11.0f)
                .lineHeight(16.0f)
                .color(stateColor(task.state))
                .build();
            components::text(ui, fid + ".percentage")
                .position(cardWidth - kCardPad - 52.0f, 42.0f)
                .size(52.0f, 16.0f)
                .text(progressText)
                .fontFamily(kMonoFont)
                .fontSize(11.0f)
                .lineHeight(16.0f)
                .horizontalAlign(core::HorizontalAlign::Right)
                .color(theme.metaText)
                .build();
            ui.rect(fid + ".progress.track")
                .position(kCardPad, 68.0f)
                .size(nameW, 3.0f)
                .color(theme.surfaceContainerHighest)
                .radius(1.5f)
                .build();
            ui.rect(fid + ".progress.fill")
                .position(kCardPad, 68.0f)
                .size(nameW * progress, 3.0f)
                .color(progressColor)
                .radius(1.5f)
                .transition(core::Transition::make(0.18f, core::Ease::OutCubic))
                .animate(core::AnimProperty::Frame | core::AnimProperty::Color)
                .build();
            std::string info = cardInfoText(task);
            if (task.state == dl::State::Done && task.totalBytes > 0) info = formatBytes(task.totalBytes);
            if (task.state == dl::State::Paused || task.state == dl::State::Cancelled) {
                // 待选择任务保留选择提示，普通暂停任务显示已下载量。
                if (!task.awaitingTorrentFileSelection && !task.awaitingMediaTrackSelection) {
                    info = task.totalBytes > 0
                        ? std::format("{} / {}", formatBytes(task.downloadedBytes), formatBytes(task.totalBytes))
                        : formatBytes(task.downloadedBytes);
                }
            }
            components::text(ui, fid + ".infotext")
                .position(kCardPad, 82.0f)
                .size(nameW, 18.0f)
                .text(ellipsizeText(info, nameW, 11.0f, kMonoFont))
                .fontSize(11.0f)
                .fontFamily(kMonoFont)
                .lineHeight(18.0f)
                .color(theme.metaText)
                .build();

            // 操作条件保持按当前任务状态决定，信息不再与按钮争用同一行。
            const bool showPause = task.state == dl::State::Downloading;
            const bool mediaTrackSelectionSupported = !task.awaitingMediaTrackSelection ||
                g_tasks.health().supportsMediaTrackSelection;
            const bool showResume = task.state == dl::State::Paused &&
                !task.awaitingTorrentFileSelection &&
                (!task.awaitingMediaTrackSelection || !mediaTrackSelectionSupported ||
                 !hasSelectableMediaTracks(task));
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
            // 镜像管理：有镜像源（含运行时 changeUri 增删）就显示，可查看源/移除坏源/
            // 添加镜像。
            const bool showMirror = task.mirrorCount > 0 || !task.mirrors.empty();
            const bool showSelectFiles = task.awaitingTorrentFileSelection;
            const bool showSelectMediaTracks = task.awaitingMediaTrackSelection &&
                mediaTrackSelectionSupported && hasSelectableMediaTracks(task);
            const bool showFinishMedia = task.isMedia && task.mediaLive &&
                (task.mediaState == "recording" || task.mediaState == "paused") &&
                (task.state == dl::State::Downloading || task.state == dl::State::Paused);

            // 从右往左摆放（place 递减 bx，先调用的在最右）。左→右阅读顺序：
            // 进行中任务：开始/暂停在最左，接着所在文件夹/复制链接，最右是取消。
            // 已完成任务：打开文件/所在文件夹/复制链接成组，接着重新下载，最后删除。
            // 全部普通颜色（无主色），与同类按钮一致。
            float btnY = kCardHeight - kCardPad - kCardIconW;
            float bx = cardWidth - kCardPad;
            const auto place = [&](const std::string& aid, unsigned int icon,
                                   bool primary, std::function<void()> cb) {
                if (bx - kCardIconW < kCardPad) {
                    bx = cardWidth - kCardPad;
                    btnY -= kCardIconW + kCardIconGap;
                }
                bx -= kCardIconW;
                drawCardAction(ui, fid + "." + aid, bx, btnY, icon, primary, theme,
                               std::move(cb));
                bx -= kCardIconGap;
            };
            if (showDelete) {
                place("delete", 0xF1F8, false,  // fa-trash（仅任务结束后显示）
                      [task = task] { requestDelete(task); });
            }
            if (showRetry) {
                place("retry", 0xF01E, false,  // fa-redo（普通颜色，与同类一致）
                      [id = task.id] { g_tasks.retry(id); });
            }
            if (showCancel) {
                // X 与垃圾桶同一效果：弹删除确认框（问是否删除任务 + 勾选删源文件）。
                place("cancel", 0xF00D, false,  // fa-times
                      [task = task] { requestDelete(task); });
            }
            place("copy", 0xF0C1, false,  // fa-link（复制链接；fa-copy 0xF0C5 像复制文件）
                  [url = task.url] {
                      core::window::setClipboardText(url);
                      showStatus(tr("card.link_copied"));
                  });
            place("info", 0xF05A, false,  // fa-info-circle（任务信息：完整 URL/报错/路径）
                  [task = task] { requestInfo(task); });
            if (showOpenFolder) {
                place("openfolder", 0xF07C, false,  // fa-folder-open
                      [path = task.destPath] { openContainingFolder(path); });
            }
            if (showOpen) {
                // 放最后 → 最左：下载完成后的「打开文件」主入口（主色图标）。
                place("open", 0xF08E, true,  // fa-external-link
                      [path = task.destPath] { openFile(path); });
            }
            if (showResume) {
                // 放最后 → 最左：进行中任务的主操作（反白主色图标，主行动强调）。
                place("resume", 0xF04B, true,  // fa-play
                      [id = task.id] { g_tasks.resume(id); });
            }
            if (showPause) {
                place("pause", 0xF04C, false,  // fa-pause
                      [id = task.id] { g_tasks.pause(id); });
            }
            if (showMirror) {
                // 放最后 → 最左：镜像源管理入口。
                place("mirror", 0xF0EC, false,  // fa-exchange（多源）
                      [id = task.id] {
                          g_mirrorTaskId = id;
                          g_mirrorAddText.clear();
                          g_mirrorOpen = true;
                      });
            }
            if (showSelectFiles) {
                place("selectfiles", 0xF03A, false,  // fa-list-ul
                      [task] { requestTorrentSelection(task); });
            }
            if (showSelectMediaTracks) {
                place("selectmediatracks", 0xF008, false,  // fa-film
                      [task] { requestMediaTrackSelection(task); });
            }
            if (showFinishMedia) {
                place("finishmedia", 0xF04D, false,  // fa-stop
                      [id = task.id] {
                          g_tasks.finishMedia(id, [](bool ok, std::string error) {
                              postStatus(ok ? tr("dl.media_finish_sent")
                                            : trf("dl.media_finish_failed", error));
                              core::platform::requestUiUpdate();
                          });
                      });
            }
        })
        .build();
}
