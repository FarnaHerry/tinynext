// ui/engine_page.cppm — Settings 内的 aria2-next 引擎监控标签内容。
//
// 引擎二进制 / 守护进程 / RPC / WS / 版本 / RPC 端点逐行体检 + 全局统计卡，
// 以及立即检测 / 重启引擎 / 打开日志操作行。
//
// 健康数据源：g_tasks.health() 是纯读缓存（UI 线程每帧读，不发 RPC）；内容由
// g_tasks.refreshHealth() 在后台命令线程刷新（housekeep 在引擎标签打开时 ~2s 一次，
// 「立即检测」按钮手动触发）。重启走 g_tasks.restartEngine()：保存会话 → 优雅退出
// → 重新拉起，进行中的下载经 .aria2 控制文件续传、不丢。
module;

#include "eui_ui.h"

export module tinynext.ui.engine_page;

import std;
import tinynext.config;          // cfg::configDir（打开引擎日志）
import tinynext.download_engine; // dl::HealthInfo
import tinynext.i18n;            // tr（监控页文案）
import tinynext.ui.theme;
import tinynext.ui.utils;        // formatBytes/formatSpeed（转发自 tinynext.utils）
import tinynext.ui.widgets;      // onPrimaryColor
import tinynext.store.tasks;     // g_tasks.health/refreshHealth/restartEngine
import tinynext.store.ui;        // postStatus（后台回调 → UI 状态条）
import tinynext.ui.platform;     // openFile（打开引擎日志）

namespace {

// 重启 / 检测的瞬时状态：后台回调写原子，UI 线程读。
// 0 空闲 / 1 重启中 / 2 成功 / 3 失败（2/3 只用于区分，按钮回到「重启引擎」）。
std::atomic<int> g_restartState{0};
std::atomic<bool> g_checking{false};  // 「立即检测」进行中

} // namespace

export void drawEnginePage(eui::Ui& ui, const AppTheme& theme, float infoX,
                           float innerW, float scrollTop, float actionY) {
    constexpr float kLabelW = 96.0f;
    // ---- 滚动内容区（标题 + 状态 + 体检 + 统计 + 参数）----
    const float scrollH = std::max(0.0f, actionY - scrollTop - 10.0f);

    components::scrollView(ui, "engine.scroll")
        .position(infoX, scrollTop)
        .size(innerW, scrollH)
        .theme(theme.components)
        .step(52.0f)
        .scrollbarWidth(kScrollbarWidth)
        .scrollbarGap(kScrollbarGap)
        .content([&](eui::Ui& sv, float contentWidth, float) {
            const float cvW = contentWidth;
            const dl::HealthInfo h = g_tasks.health();
            const int statCols = cvW >= 1000.0f ? 5 : cvW >= 700.0f ? 3 : 2;
            const int statRows = (5 + statCols - 1) / statCols;
            const float statGap = 8.0f;
            const float statRowGap = 6.0f;
            const float statAreaH = statRows * 46.0f + (statRows - 1) * statRowGap;
            const int paramCols = cvW < 560.0f ? 1 : 2;
            const int paramRows = (6 + paramCols - 1) / paramCols;
            float rowY = 30.0f + 6.0f * 22.0f;
            if (!h.error.empty()) rowY += 24.0f;
            const float statTop = rowY + 10.0f;
            const float paramsTop = statTop + statAreaH + 14.0f;
            const float canvasH = paramsTop + 18.0f + paramRows * 20.0f + 20.0f;
            sv.stack("engine.canvas")
                .width(contentWidth)
                .height(canvasH)
                .content([&] {
                    // canvas 内坐标相对（0, 0）
                    const float cvX = 0.0f;

                    components::text(sv, "engine.title")
                        .position(cvX, 0.0f)
                        .size(cvW, 24.0f)
                        .text(tr("app.tab.monitor"))
                        .fontSize(20.0f)
                        .lineHeight(24.0f)
                        .color(theme.titleText)
                        .build();

                    // ---- 健康快照 + 顶部状态（圆点 + 标签）----
                    const char* statusLabel = tr("eng.checking");
                    eui::Color statusColor = theme.metaText;
                    if (h.checked) {
                        if (!h.binaryFound) {
                            statusLabel = tr("eng.missing");
                            statusColor = theme.failed;
                        } else if (h.rpcReachable) {
                            statusLabel = tr("eng.healthy");
                            statusColor = theme.done;
                        } else if (h.daemonSpawned) {
                            statusLabel = tr("eng.service_error");
                            statusColor = theme.failed;
                        } else {
                            statusLabel = tr("eng.not_running");
                            statusColor = theme.metaText;
                        }
                    }
                    const float dotX = cvW - 140.0f;
                    sv.rect("engine.status.dot")
                        .position(dotX, 7.0f)
                        .size(10.0f, 10.0f)
                        .color(statusColor)
                        .radius(5.0f)
                        .build();
                    components::text(sv, "engine.status.label")
                        .position(dotX + 16.0f, 0.0f)
                        .size(124.0f, 24.0f)
                        .text(statusLabel)
                        .fontSize(12.0f)
                        .lineHeight(24.0f)
                        .color(statusColor)
                        .verticalAlign(core::VerticalAlign::Center)
                        .build();

                    // ---- 体检行：标签 + 值 ----
                    rowY = 30.0f;
                    const auto statusRow = [&](const std::string& id, const char* label,
                                               const std::string& value, const eui::Color& color) {
                        components::text(sv, id + ".label")
                            .position(cvX, rowY)
                            .size(kLabelW, 22.0f)
                            .text(label)
                            .fontSize(11.0f)
                            .lineHeight(22.0f)
                            .color(theme.metaText)
                            .build();
                        components::text(sv, id + ".value")
                            .position(cvX + kLabelW, rowY)
                            .size(cvW - kLabelW, 22.0f)
                            .text(value)
                            .fontSize(11.0f)
                            .lineHeight(22.0f)
                            .color(color)
                            .build();
                        rowY += 22.0f;
                    };

                    const eui::Color okColor = theme.done;
                    const eui::Color badColor = theme.failed;
                    const eui::Color idleColor = theme.metaText;

                    if (!h.checked) {
                        statusRow("engine.bin", tr("eng.binary"),
                                  tr("eng.checking"), idleColor);
                    } else if (h.binaryFound) {
                        statusRow("engine.bin", tr("eng.binary"),
                                  tr("eng.binary_found"), okColor);
                    } else {
                        statusRow("engine.bin", tr("eng.binary"),
                                  tr("eng.binary_not_found"), badColor);
                    }

                    if (h.daemonAlive) {
                        statusRow("engine.daemon", tr("eng.daemon"),
                                  tr("eng.running"), okColor);
                    } else if (h.daemonSpawned) {
                        statusRow("engine.daemon", tr("eng.daemon"),
                                  tr("eng.process_exited"), badColor);
                    } else if (!h.checked) {
                        statusRow("engine.daemon", tr("eng.daemon"),
                                  tr("eng.checking"), idleColor);
                    } else {
                        statusRow("engine.daemon", tr("eng.daemon"),
                                  tr("eng.not_running"), idleColor);
                    }

                    if (h.rpcReachable) {
                        statusRow("engine.rpc", tr("eng.rpc_service"),
                                  tr("eng.ok"), okColor);
                    } else if (h.daemonSpawned) {
                        statusRow("engine.rpc", tr("eng.rpc_service"),
                                  tr("eng.no_response"), badColor);
                    } else if (!h.checked) {
                        statusRow("engine.rpc", tr("eng.rpc_service"),
                                  tr("eng.checking"), idleColor);
                    } else {
                        statusRow("engine.rpc", tr("eng.rpc_service"),
                                  tr("eng.not_running"), idleColor);
                    }

                    if (h.wsConnected) {
                        statusRow("engine.ws", tr("eng.ws_push"),
                                  tr("eng.connected"), okColor);
                    } else {
                        statusRow("engine.ws", tr("eng.ws_push"),
                                  tr("eng.disconnected_poll"), idleColor);
                    }

                    statusRow("engine.ver", tr("eng.version"),
                              h.version.empty() ? tr("eng.unknown") : h.version, theme.nameText);

                    statusRow("engine.port", tr("eng.rpc_endpoint"),
                              h.rpcPort > 0 ? "127.0.0.1:" + std::to_string(h.rpcPort)
                                            : tr("eng.dash"),
                              theme.nameText);

                    // ---- 最近错误（红字，仅当有）----
                    if (!h.error.empty()) {
                        components::text(sv, "engine.error")
                            .position(cvX, rowY + 2.0f)
                            .size(cvW, 20.0f)
                            .text(std::string(tr("eng.error_prefix")) + h.error)
                            .fontSize(11.0f)
                            .lineHeight(20.0f)
                            .color(theme.failed)
                            .build();
                        rowY += 24.0f;
                    }

                    // ---- 全局统计小卡 ----
                    const struct { const char* label; std::string value; } kStats[] = {
                        {tr("eng.speed_down"), formatBytes(h.downloadSpeedBps) + "/s"},
                        {tr("eng.speed_up"), formatBytes(h.uploadSpeedBps) + "/s"},
                        {tr("eng.active"), std::to_string(h.activeDownloads)},
                        {tr("eng.waiting"), std::to_string(h.waitingDownloads)},
                        {tr("eng.stopped"), std::to_string(h.stoppedDownloads)},
                    };
                    constexpr int kStatCount = 5;
                    const float statW = (cvW - statGap * (statCols - 1)) / statCols;
                    for (int i = 0; i < kStatCount; ++i) {
                        const int col = i % statCols;
                        const int row = i / statCols;
                        const float sx = cvX + col * (statW + statGap);
                        const float sy = statTop + row * (46.0f + statRowGap);
                        sv.rect(std::format("engine.stat.{}.bg", i))
                            .position(sx, sy)
                            .size(statW, 46.0f)
                            .color(theme.cardBg)
                            .radius(kCardRadius)
                            .border(kHairline, theme.outline)
                            .build();
                        components::text(sv, std::format("engine.stat.{}.value", i))
                            .position(sx, sy + 5.0f)
                            .size(statW, 19.0f)
                            .text(kStats[i].value)
                            .fontSize(12.0f)
                            .fontFamily(kMonoFont)  // 统计数字等宽
                            .lineHeight(19.0f)
                            .color(theme.nameText)
                            .horizontalAlign(core::HorizontalAlign::Center)
                            .build();
                        components::text(sv, std::format("engine.stat.{}.label", i))
                            .position(sx, sy + 26.0f)
                            .size(statW, 15.0f)
                            .text(kStats[i].label)
                            .fontSize(10.0f)
                            .lineHeight(15.0f)
                            .color(theme.metaText)
                            .horizontalAlign(core::HorizontalAlign::Center)
                            .build();
                    }

                    // ---- 运行参数 ----
                    const cfg::Aria2Config a2 = cfg::aria2Config();
                    const struct { const char* label; std::string value; } kParams[] = {
                        {tr("eng.split_conn"),
                         std::to_string(a2.split) + " / " + std::to_string(a2.maxConnectionPerServer)},
                        {tr("eng.max_concurrent"), std::to_string(a2.maxConcurrentDownloads)},
                        {tr("eng.per_task_limit"),
                         a2.maxDownloadLimit > 0 ? formatSpeed(static_cast<double>(a2.maxDownloadLimit))
                                                 : tr("eng.unlimited")},
                        {tr("eng.retry"),
                         std::to_string(a2.maxTries) + " (" + std::to_string(a2.retryWait) + "s)"},
                        {tr("eng.proxy"), a2.proxy.empty() ? tr("eng.none") : a2.proxy},
                        {tr("eng.cookie"), a2.loadCookies.empty() ? tr("eng.none") : a2.loadCookies},
                    };
                    components::text(sv, "engine.params.header")
                        .position(cvX, paramsTop)
                        .size(cvW, 16.0f)
                        .text(tr("eng.runtime_opts"))
                        .fontSize(11.0f)
                        .lineHeight(16.0f)
                        .color(theme.titleText)
                        .build();
                    const float paramGap = 8.0f;
                    const float paramColW = (cvW - paramGap * (paramCols - 1)) / paramCols;
                    const float paramRowH = 20.0f;
                    const float paramLabelW = std::min(84.0f, paramColW * 0.38f);
                    for (int i = 0; i < 6; ++i) {
                        const int col = i % paramCols;
                        const int r = i / paramCols;
                        const float px = cvX + col * (paramColW + paramGap);
                        const float py = paramsTop + 18.0f + r * paramRowH;
                        components::text(sv, std::format("engine.params.{}.label", i))
                            .position(px, py)
                            .size(paramLabelW, paramRowH)
                            .text(kParams[i].label)
                            .fontSize(10.0f)
                            .lineHeight(paramRowH)
                            .color(theme.metaText)
                            .build();
                        components::text(sv, std::format("engine.params.{}.value", i))
                            .position(px + paramLabelW, py)
                            .size(paramColW - paramLabelW, paramRowH)
                            .text(kParams[i].value)
                            .fontSize(10.0f)
                            .lineHeight(paramRowH)
                            .color(theme.nameText)
                            .build();
                    }
                })
                .build();
        })
        .build();

    // ---- 操作行（固定窗口底部）：立即检测 / 重启引擎 / 打开日志 ----
    {
        const bool checking = g_checking.load();
        drawTextButton(ui, "engine.check", infoX, actionY, 76.0f, kButtonHeight,
                       checking ? tr("eng.checking") : tr("eng.check_now"), theme,
                       [] {
                if (g_checking.load()) return;  // 检测中忽略连点
                g_checking.store(true);
                g_tasks.refreshHealth([](const dl::HealthInfo&) {
                    g_checking.store(false);
                    core::platform::requestUiUpdate();
                });
            });
    }

    const bool restarting = g_restartState.load() == 1;
    components::button(ui, "engine.restart")
        .position(infoX + 76.0f + kButtonGap, actionY)
        .size(84.0f, kButtonHeight)
        .text(restarting ? tr("eng.restarting") : tr("eng.restart"))
        .fontSize(kButtonFontSize)
        .theme(theme.components, true)
        .radius(kButtonRadius)
        .textColor(onPrimaryColor(theme))
        .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .disabled(restarting)
        .onClick([] {
            if (g_restartState.load() == 1) return;  // 正在重启，忽略连点
            // 文案在 UI 线程预翻译成静态串再捕获（后台回调不碰 g_lang）。
            const char* okMsg = tr("eng.restarted");
            const char* failMsg = tr("eng.restart_failed");
            g_restartState.store(1);
            g_tasks.restartEngine([okMsg, failMsg](bool ok) {
                postStatus(ok ? okMsg : failMsg);
                g_restartState.store(ok ? 2 : 3);
                // 重启后立即刷新健康信息（等 housekeep 的 ~2s 周期会显示旧状态）。
                g_tasks.refreshHealth([](const dl::HealthInfo&) {});
            });
        })
        .build();

    drawTextButton(ui, "engine.log",
                   infoX + 76.0f + kButtonGap + 84.0f + kButtonGap, actionY,
                   76.0f, kButtonHeight, tr("eng.open_log"), theme,
                   [] {
            openFile(cfg::configDir() / "tinynext-aria2.log");
        });

    components::text(ui, "engine.hint")
        .position(infoX + 76.0f + kButtonGap + 84.0f + kButtonGap + 76.0f + 12.0f, actionY)
        .size(innerW - (76.0f + kButtonGap + 84.0f + kButtonGap + 76.0f + 12.0f), kButtonHeight)
        .text(tr("eng.restart_hint"))
        .fontSize(10.0f)
        .lineHeight(kButtonHeight)
        .color(theme.metaText)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}
