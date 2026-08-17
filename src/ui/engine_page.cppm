// ui/engine_page.cppm — aria2-next 引擎监控页：健康状态 + 全局统计 + 重启。
//
// 与设置页同款「整页单岛」布局：引擎二进制 / 守护进程 / RPC / WS / 版本 /
// RPC 端点逐行体检 + 右下全局统计卡 + 底部操作行（立即检测 / 重启引擎 / 打开日志）。
//
// 健康数据源：g_tasks.health() 是纯读缓存（UI 线程每帧读，不发 RPC）；内容由
// g_tasks.refreshHealth() 在后台命令线程刷新（housekeep 在监控页打开时 ~2s 一次，
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
import tinynext.ui.utils;        // kRailWidth/kRightMargin/kIslandVInset/kPanelPad
                                 // + formatBytes/formatSpeed（转发自 tinynext.utils）
import tinynext.ui.widgets;      // drawPanel / onPrimaryColor
import tinynext.store.tasks;     // g_tasks.health/refreshHealth/restartEngine
import tinynext.store.ui;        // postStatus（后台回调 → UI 状态条）
import tinynext.ui.platform;     // openFile（打开引擎日志）

namespace {

// 重启 / 检测的瞬时状态：后台回调写原子，UI 线程读。
// 0 空闲 / 1 重启中 / 2 成功 / 3 失败（2/3 只用于区分，按钮回到「重启引擎」）。
std::atomic<int> g_restartState{0};
std::atomic<bool> g_checking{false};  // 「立即检测」进行中

} // namespace

export void drawEnginePage(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    const float islandTop = kIslandVInset;
    const float islandH = screen.height - 2.0f * kIslandVInset;
    const float islandX = kRailWidth;
    const float islandW = screen.width - islandX - kRightMargin;
    const float pad = kPanelPad;
    const float infoX = islandX + pad;
    const float innerW = islandW - 2.0f * pad;
    constexpr float kLabelW = 96.0f;

    // 整页一张岛卡（无二级侧边栏，监控内容无需分栏）。
    drawPanel(ui, "engine.island", islandX, islandTop, islandW, islandH, theme);

    const float titleY = islandTop + 16.0f;
    components::text(ui, "engine.title")
        .position(infoX, titleY)
        .size(innerW, 24.0f)
        .text(tr("引擎监控", "Engine monitor"))
        .fontSize(17.0f)
        .lineHeight(24.0f)
        .color(theme.titleText)
        .build();

    // ---- 健康快照 + 顶部状态（圆点 + 标签）----
    const dl::HealthInfo h = g_tasks.health();
    const char* statusLabel = tr("检测中…", "Checking…");
    eui::Color statusColor = theme.metaText;
    if (h.checked) {
        if (!h.binaryFound) {
            statusLabel = tr("引擎缺失", "Engine missing");
            statusColor = theme.failed;
        } else if (h.rpcReachable) {
            statusLabel = tr("运行正常", "Healthy");
            statusColor = theme.done;
        } else if (h.daemonSpawned) {
            statusLabel = tr("服务异常", "Service error");
            statusColor = theme.failed;
        } else {
            statusLabel = tr("未运行", "Not running");
            statusColor = theme.metaText;
        }
    }
    const float dotX = islandX + islandW - pad - 140.0f;
    ui.rect("engine.status.dot")
        .position(dotX, titleY + 7.0f)
        .size(10.0f, 10.0f)
        .color(statusColor)
        .radius(5.0f)
        .build();
    components::text(ui, "engine.status.label")
        .position(dotX + 16.0f, titleY)
        .size(124.0f, 24.0f)
        .text(statusLabel)
        .fontSize(12.0f)
        .lineHeight(24.0f)
        .color(statusColor)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // ---- 体检行：标签 + 值（绿=正常 / 红=异常 / 灰=未运行）----
    float rowY = titleY + 30.0f;
    const auto statusRow = [&](const std::string& id, const char* label,
                               const std::string& value, const eui::Color& color) {
        components::text(ui, id + ".label")
            .position(infoX, rowY)
            .size(kLabelW, 22.0f)
            .text(label)
            .fontSize(11.0f)
            .lineHeight(22.0f)
            .color(theme.metaText)
            .build();
        components::text(ui, id + ".value")
            .position(infoX + kLabelW, rowY)
            .size(innerW - kLabelW, 22.0f)
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
        statusRow("engine.bin", tr("引擎二进制", "Engine binary"),
                  tr("检测中…", "Checking…"), idleColor);
    } else if (h.binaryFound) {
        statusRow("engine.bin", tr("引擎二进制", "Engine binary"),
                  tr("存在（engines/aria2-next）", "Found (engines/aria2-next)"), okColor);
    } else {
        statusRow("engine.bin", tr("引擎二进制", "Engine binary"),
                  tr("未找到（下载不可用）", "Not found (downloads disabled)"), badColor);
    }

    if (h.daemonAlive) {
        statusRow("engine.daemon", tr("守护进程", "Daemon"),
                  tr("运行中", "Running"), okColor);
    } else if (h.daemonSpawned) {
        statusRow("engine.daemon", tr("守护进程", "Daemon"),
                  tr("进程已退出", "Process exited"), badColor);
    } else if (!h.checked) {
        statusRow("engine.daemon", tr("守护进程", "Daemon"),
                  tr("检测中…", "Checking…"), idleColor);
    } else {
        statusRow("engine.daemon", tr("守护进程", "Daemon"),
                  tr("未运行", "Not running"), idleColor);
    }

    if (h.rpcReachable) {
        statusRow("engine.rpc", tr("RPC 服务", "RPC service"),
                  tr("正常", "OK"), okColor);
    } else if (h.daemonSpawned) {
        statusRow("engine.rpc", tr("RPC 服务", "RPC service"),
                  tr("无响应", "No response"), badColor);
    } else if (!h.checked) {
        statusRow("engine.rpc", tr("RPC 服务", "RPC service"),
                  tr("检测中…", "Checking…"), idleColor);
    } else {
        statusRow("engine.rpc", tr("RPC 服务", "RPC service"),
                  tr("未运行", "Not running"), idleColor);
    }

    if (h.wsConnected) {
        statusRow("engine.ws", tr("WS 推送", "WS push"),
                  tr("已连接", "Connected"), okColor);
    } else {
        statusRow("engine.ws", tr("WS 推送", "WS push"),
                  tr("未连接（轮询兜底）", "Disconnected (poll fallback)"), idleColor);
    }

    statusRow("engine.ver", tr("引擎版本", "Engine version"),
              h.version.empty() ? tr("未知", "Unknown") : h.version, theme.nameText);

    statusRow("engine.port", tr("RPC 端点", "RPC endpoint"),
              h.rpcPort > 0 ? "127.0.0.1:" + std::to_string(h.rpcPort)
                            : tr("—", "—"),
              theme.nameText);

    // ---- 最近错误（红字，仅当有）----
    if (!h.error.empty()) {
        components::text(ui, "engine.error")
            .position(infoX, rowY + 2.0f)
            .size(innerW, 20.0f)
            .text(std::string(tr("错误：", "Error: ")) + h.error)
            .fontSize(11.0f)
            .lineHeight(20.0f)
            .color(theme.failed)
            .build();
        rowY += 24.0f;
    }

    // ---- 全局统计小卡（下载/上传速度 + 任务数）----
    const struct { const char* label; std::string value; } kStats[] = {
        {tr("下载速度", "Download"), formatBytes(h.downloadSpeedBps) + "/s"},
        {tr("上传速度", "Upload"), formatBytes(h.uploadSpeedBps) + "/s"},
        {tr("活动任务", "Active"), std::to_string(h.activeDownloads)},
        {tr("等待任务", "Waiting"), std::to_string(h.waitingDownloads)},
        {tr("停止任务", "Stopped"), std::to_string(h.stoppedDownloads)},
    };
    constexpr int kStatCount = 5;
    const float statGap = 8.0f;
    const float statW = (innerW - statGap * (kStatCount - 1)) / kStatCount;
    const float statTop = rowY + 10.0f;
    float sx = infoX;
    for (int i = 0; i < kStatCount; ++i) {
        ui.rect(std::format("engine.stat.{}.bg", i))
            .position(sx, statTop)
            .size(statW, 46.0f)
            .color(theme.components.surface)
            .radius(8.0f)
            .border(1.0f, components::theme::withOpacity(theme.components.border, 0.6f))
            .build();
        components::text(ui, std::format("engine.stat.{}.value", i))
            .position(sx, statTop + 5.0f)
            .size(statW, 19.0f)
            .text(kStats[i].value)
            .fontSize(12.0f)
            .lineHeight(19.0f)
            .color(theme.nameText)
            .horizontalAlign(core::HorizontalAlign::Center)
            .build();
        components::text(ui, std::format("engine.stat.{}.label", i))
            .position(sx, statTop + 26.0f)
            .size(statW, 15.0f)
            .text(kStats[i].label)
            .fontSize(10.0f)
            .lineHeight(15.0f)
            .color(theme.metaText)
            .horizontalAlign(core::HorizontalAlign::Center)
            .build();
        sx += statW + statGap;
    }

    // ---- 运行参数（daemon 启动时的关键选项摘要，读配置，排 2 列 x 3 行）----
    const cfg::Aria2Config a2 = cfg::aria2Config();
    const struct { const char* label; std::string value; } kParams[] = {
        {tr("分片 / 连接", "Split / conn"),
         std::to_string(a2.split) + " / " + std::to_string(a2.maxConnectionPerServer)},
        {tr("最大并发", "Max concurrent"), std::to_string(a2.maxConcurrentDownloads)},
        {tr("每任务限速", "Per-task limit"),
         a2.maxDownloadLimit > 0 ? formatSpeed(static_cast<double>(a2.maxDownloadLimit))
                                 : tr("不限", "Unlimited")},
        {tr("重试", "Max tries"),
         std::to_string(a2.maxTries) + " (" + std::to_string(a2.retryWait) + "s)"},
        {tr("代理", "Proxy"), a2.proxy.empty() ? tr("无", "None") : a2.proxy},
        {tr("Cookie", "Cookies"), a2.loadCookies.empty() ? tr("无", "None") : a2.loadCookies},
    };
    const float paramsTop = statTop + 46.0f + 14.0f;
    components::text(ui, "engine.params.header")
        .position(infoX, paramsTop)
        .size(innerW, 16.0f)
        .text(tr("运行参数（daemon 启动时）", "Runtime options (at daemon start)"))
        .fontSize(11.0f)
        .lineHeight(16.0f)
        .color(theme.statusText)
        .build();
    constexpr int kParamCols = 2;
    constexpr int kParamRows = 3;
    const float paramGap = 8.0f;
    const float paramColW = (innerW - paramGap) / kParamCols;
    const float paramRowH = 20.0f;
    const float paramLabelW = 84.0f;
    for (int i = 0; i < kParamCols * kParamRows; ++i) {
        const int col = i % kParamCols;
        const int r = i / kParamCols;
        const float px = infoX + col * (paramColW + paramGap);
        const float py = paramsTop + 18.0f + r * paramRowH;
        components::text(ui, std::format("engine.params.{}.label", i))
            .position(px, py)
            .size(paramLabelW, paramRowH)
            .text(kParams[i].label)
            .fontSize(10.0f)
            .lineHeight(paramRowH)
            .color(theme.metaText)
            .build();
        components::text(ui, std::format("engine.params.{}.value", i))
            .position(px + paramLabelW, py)
            .size(paramColW - paramLabelW, paramRowH)
            .text(kParams[i].value)
            .fontSize(10.0f)
            .lineHeight(paramRowH)
            .color(theme.nameText)
            .build();
    }

    // ---- 操作行（固定窗口底部）：立即检测 / 重启引擎 / 打开日志 ----
    constexpr float kActionH = 26.0f;
    const float actionY = islandTop + islandH - pad - kActionH;

    components::button(ui, "engine.check")
        .position(infoX, actionY)
        .size(76.0f, kActionH)
        .text(g_checking.load() ? tr("检测中…", "Checking…") : tr("立即检测", "Check now"))
        .fontSize(12.0f)
        .theme(theme.components, false)
        .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .disabled(g_checking.load())
        .onClick([] {
            g_checking.store(true);
            g_tasks.refreshHealth([](const dl::HealthInfo&) {
                g_checking.store(false);
                core::platform::requestUiUpdate();
            });
        })
        .build();

    const bool restarting = g_restartState.load() == 1;
    components::button(ui, "engine.restart")
        .position(infoX + 76.0f + 8.0f, actionY)
        .size(84.0f, kActionH)
        .text(restarting ? tr("重启中…", "Restarting…") : tr("重启引擎", "Restart engine"))
        .fontSize(12.0f)
        .theme(theme.components, true)
        .textColor(onPrimaryColor(theme))
        .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .disabled(restarting)
        .onClick([] {
            if (g_restartState.load() == 1) return;  // 正在重启，忽略连点
            // 文案在 UI 线程预翻译成静态串再捕获（后台回调不碰 g_lang）。
            const char* okMsg = tr("引擎已重启，任务已恢复", "Engine restarted, tasks recovered");
            const char* failMsg = tr("引擎重启失败，请查看日志", "Engine restart failed, check the log");
            g_restartState.store(1);
            g_tasks.restartEngine([okMsg, failMsg](bool ok) {
                postStatus(ok ? okMsg : failMsg);
                g_restartState.store(ok ? 2 : 3);
                // 重启后立即刷新健康信息（等 housekeep 的 ~2s 周期会显示旧状态）。
                g_tasks.refreshHealth([](const dl::HealthInfo&) {});
            });
        })
        .build();

    components::button(ui, "engine.log")
        .position(infoX + 76.0f + 8.0f + 84.0f + 8.0f, actionY)
        .size(76.0f, kActionH)
        .text(tr("打开日志", "Open log"))
        .fontSize(12.0f)
        .theme(theme.components, false)
        .shadow(0.0f, 0.0f, 0.0f, core::Color{0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([] {
            openFile(cfg::configDir() / "tinynext-aria2.log");
        })
        .build();

    components::text(ui, "engine.hint")
        .position(infoX + 76.0f + 8.0f + 84.0f + 8.0f + 76.0f + 12.0f, actionY)
        .size(innerW - (76.0f + 8.0f + 84.0f + 8.0f + 76.0f + 12.0f), kActionH)
        .text(tr("重启会保存会话并重新拉起引擎，进行中的下载会继续。",
                 "Restart saves the session and relaunches the engine; in-progress downloads continue."))
        .fontSize(10.0f)
        .lineHeight(kActionH)
        .color(theme.metaText)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}
