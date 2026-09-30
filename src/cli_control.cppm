// cli_control.cppm — CLI 控制面：查询 TinyNext 运行状态 + 操作运行中的实例。
//
// 给连到电脑的 AI agent 与终端用户用：`tinynext status / list / watch / pause /
// resume / cancel / retry / remove / clear / quit`。纯领域层——只碰 g_tasks /
// config / utils，不依赖 eui / ui.*。
//
// 线程约定（与 tinynext.cli 的 IPC 层配套）：
//   * 只读动词（status / list / watch）在 IPC 监听线程直接执行：
//     snapshot() 内部有锁（housekeep 后台线程同款用法）、refreshHealth 的回调
//     在引擎后台命令线程触发，都与 UI 无关，窗口缩进托盘时照样能答。
//   * 变更动词（pause / resume / cancel / retry / remove / clear / quit）由
//     cli.cppm marshal 到 UI 线程后回调本模块的 handle()（store 约定：任务命令
//     在 UI 线程发起），保证与卡片操作同一条代码路径。
//
// 输出协议（对 agent 稳定）：
//   * 默认人类可读文本；带 --json 时输出单块 JSON（UTF-8，紧凑缩进 2）。
//   * 失败行以 "error: " 开头（json 模式则顶层 "ok": false）；客户端据此映射
//     退出码 2（见 cli.cppm runControlRequest）。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // GetCurrentProcessId
#else
#include <unistd.h>   // getpid
#endif

export module tinynext.cli_control;

import std;
import nlohmann.json;
import tinynext.config;        // cfg::kAppVersion / cfg::downloadDir
import tinynext.download_engine;  // dl::State / TaskView / HealthInfo
import tinynext.store.tasks;   // g_tasks / taskDisplayName
import tinynext.utils;         // formatBytes / formatSpeed / utf8FromPath

namespace cli_control {

// 控制动词白名单（CLI 首参数命中即走控制面，绝不启动 GUI）。
export bool isControlVerb(const std::string& s) {
    static const std::set<std::string, std::less<>> kVerbs{
        "status", "list", "watch", "pause", "resume",
        "cancel", "retry", "remove", "clear", "quit",
    };
    return kVerbs.count(s) > 0;
}

// 只读动词：不 marshal，IPC 监听线程直接执行（窗口在托盘里也能应答）。
export bool isReadOnlyVerb(const std::string& verb) {
    return verb == "status" || verb == "list" || verb == "watch";
}

namespace {

const char* stateName(dl::State s) {
    switch (s) {
        case dl::State::Queued: return "Queued";
        case dl::State::Downloading: return "Downloading";
        case dl::State::Paused: return "Paused";
        case dl::State::Done: return "Done";
        case dl::State::Failed: return "Failed";
        case dl::State::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

bool wantsJson(const std::vector<std::string>& tokens) {
    for (const auto& t : tokens) {
        if (t == "--json") return true;
    }
    return false;
}

// 动词后的非 flag 参数。只对有值 flag（白名单）连同其值一起跳过；无值 flag
// （--json / --until-idle…）只跳自身——否则 `pause --json all` 的 "all" 会被吃。
std::vector<std::string> positional(const std::vector<std::string>& tokens,
                                    std::size_t from = 1) {
    static const std::set<std::string, std::less<>> kValueFlags{"--state", "--interval"};
    std::vector<std::string> out;
    bool skipValue = false;
    for (std::size_t i = from; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        if (skipValue) { skipValue = false; continue; }
        if (t.starts_with("--")) {
            if (kValueFlags.count(t)) skipValue = true;
            continue;
        }
        out.push_back(t);
    }
    return out;
}

std::string flagValue(const std::vector<std::string>& tokens, const std::string& flag,
                      std::string fallback = "") {
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == flag) return tokens[i + 1];
    }
    return fallback;
}

unsigned long long currentPid() {
#ifdef _WIN32
    return static_cast<unsigned long long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long long>(::getpid());
#endif
}

// 0-100；未知大小（aria2 total -1）返回 -1（文本显示 "?"）。
double progressPct(const dl::TaskView& t) {
    if (t.state == dl::State::Done) return 100.0;
    if (t.isMedia) {
        return t.mediaLive ? -1.0 : std::clamp(t.mediaProgress, 0.0, 1.0) * 100.0;
    }
    if (t.totalBytes > 0) {
        return static_cast<double>(t.downloadedBytes) * 100.0 /
               static_cast<double>(t.totalBytes);
    }
    return -1.0;
}

std::string fmtPct(double pct) {
    if (pct < 0.0) return "?";
    return std::format("{:.1f}%", pct);
}

std::string fmtSize(std::int64_t bytes) {
    if (bytes < 0) return "?";
    return formatBytes(bytes);
}

// 活动 = 还在跑 / 还能跑（Queued / Downloading / Paused）。
bool isActive(const dl::TaskView& t) {
    return t.state == dl::State::Queued || t.state == dl::State::Downloading ||
           t.state == dl::State::Paused;
}

// ---- 健康信息：优先拿一次新鲜值（refreshHealth 走引擎后台命令线程，与 UI 无关），
// 5s 超时回退缓存（housekeep / 监控页刷新的 health()）。监听线程直接调。
// promise 用 shared_ptr：超时先返回时，后台回调仍可能稍后才触发，回调持强引用
// 就不会悬空写栈对象（future 先析构无害——共享状态本身引用计数）。----
dl::HealthInfo liveHealth() {
    auto pr = std::make_shared<std::promise<dl::HealthInfo>>();
    std::future<dl::HealthInfo> fu = pr->get_future();
    g_tasks.refreshHealth(
        [pr](const dl::HealthInfo& h) {
            try { pr->set_value(h); } catch (...) {}  // 已设过值则忽略
        });
    if (fu.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        try {
            return fu.get();
        } catch (...) {}
    }
    return g_tasks.health();
}

// ---- status ----
struct TaskCounts {
    int total = 0, queued = 0, downloading = 0, paused = 0, done = 0, failed = 0,
        cancelled = 0;
    double speedBps = 0.0;          // 下载中任务的速度合计
    std::int64_t downloadedBytes = 0;
};
TaskCounts countTasks(const std::vector<dl::TaskView>& tasks) {
    TaskCounts c;
    c.total = static_cast<int>(tasks.size());
    for (const auto& t : tasks) {
        switch (t.state) {
            case dl::State::Queued: ++c.queued; break;
            case dl::State::Downloading: ++c.downloading; break;
            case dl::State::Paused: ++c.paused; break;
            case dl::State::Done: ++c.done; break;
            case dl::State::Failed: ++c.failed; break;
            case dl::State::Cancelled: ++c.cancelled; break;
        }
        if (t.state == dl::State::Downloading) c.speedBps += t.speedBps;
        if (t.downloadedBytes > 0) c.downloadedBytes += t.downloadedBytes;
    }
    return c;
}

nlohmann::json statusJson(const dl::HealthInfo& h, const TaskCounts& c) {
    return {
        {"ok", true},
        {"app", {{"name", "TinyNext"},
                 {"version", std::string(cfg::kAppVersion)},
                 {"pid", currentPid()},
                 {"running", true}}},
        {"engine", {{"binaryFound", h.binaryFound},
                    {"daemonAlive", h.daemonAlive},
                    {"rpcReachable", h.rpcReachable},
                    {"wsConnected", h.wsConnected},
                    {"version", h.version},
                    {"rpcPort", h.rpcPort},
                    {"error", h.error}}},
        {"tasks", {{"total", c.total},
                   {"queued", c.queued},
                   {"downloading", c.downloading},
                   {"paused", c.paused},
                   {"done", c.done},
                   {"failed", c.failed},
                   {"cancelled", c.cancelled}}},
        {"speed", {{"downBps", static_cast<std::int64_t>(c.speedBps)},
                   {"upBps", h.uploadSpeedBps},
                   {"downloadedBytes", c.downloadedBytes}}},
        {"config", {{"downloadDir", utf8FromPath(cfg::downloadDir())}}},
    };
}

std::string handleStatus(const std::vector<std::string>& tokens) {
    const auto tasks = g_tasks.snapshot();
    const auto counts = countTasks(tasks);
    const auto h = liveHealth();
    if (wantsJson(tokens)) return statusJson(h, counts).dump(2);

    std::ostringstream o;
    o << "TinyNext v" << cfg::kAppVersion << " — running (pid " << currentPid() << ")\n";
    if (!h.binaryFound) {
        o << "engine   : aria2-next binary MISSING (downloads disabled)\n";
    } else if (h.rpcReachable && h.daemonAlive) {
        o << "engine   : aria2-next " << (h.version.empty() ? "?" : h.version)
          << " — ok (RPC 127.0.0.1:" << h.rpcPort
          << ", websocket " << (h.wsConnected ? "connected" : "down, polling") << ")\n";
    } else if (h.daemonAlive) {
        o << "engine   : daemon alive but RPC unreachable"
          << (h.error.empty() ? "" : " (" + h.error + ")") << "\n";
    } else if (h.daemonSpawned) {
        o << "engine   : daemon process exited (next download respawns it)"
          << (h.error.empty() ? "" : " — " + h.error) << "\n";
    } else {
        o << "engine   : daemon not running yet (starts with the first download / warmup)\n";
    }
    o << "tasks    : " << counts.total << " total";
    const auto add = [&](int n, const char* label) {
        if (n > 0) o << ", " << n << ' ' << label;
    };
    add(counts.downloading, "downloading");
    add(counts.queued, "queued");
    add(counts.paused, "paused");
    add(counts.done, "done");
    add(counts.failed, "failed");
    add(counts.cancelled, "cancelled");
    o << "\n";
    o << "speed    : " << (counts.speedBps > 0 ? formatSpeed(counts.speedBps) : "idle")
      << " down · " << (h.uploadSpeedBps > 0 ? formatSpeed(static_cast<double>(h.uploadSpeedBps)) : "0 B/s")
      << " up · downloaded " << fmtSize(counts.downloadedBytes) << "\n";
    o << "download : " << utf8FromPath(cfg::downloadDir()) << "\n";
    return o.str();
}

// ---- list ----
nlohmann::json taskJson(const dl::TaskView& t) {
    nlohmann::json j = {
        {"id", t.id},
        {"state", stateName(t.state)},
        {"name", taskDisplayName(t)},
        {"url", t.url},
        {"path", t.destPathUtf8.empty() ? utf8FromPath(t.destPath) : t.destPathUtf8},
        {"downloadedBytes", t.downloadedBytes},
        {"totalBytes", t.totalBytes},
        {"speedBps", static_cast<std::int64_t>(t.speedBps)},
        {"connections", t.connections},
        {"mirrorCount", t.mirrorCount},
    };
    const double pct = progressPct(t);
    if (pct >= 0.0) j["progress"] = std::round(pct * 10.0) / 10.0;
    else j["progress"] = nullptr;   // 大小未知（流式下载）
    if (t.isMedia) {
        j["media"] = {
            {"protocol", t.mediaProtocol},
            {"state", t.mediaState},
            {"live", t.mediaLive},
            {"progress", t.mediaLive ? nlohmann::json(nullptr)
                                      : nlohmann::json(std::round(t.mediaProgress * 1000.0) / 10.0)},
            {"durationMs", t.mediaDurationMs},
            {"completedDurationMs", t.mediaCompletedDurationMs},
            {"downloadedBytes", t.mediaDownloadedBytes},
        };
    }
    if (t.awaitingTorrentFileSelection) {
        j["bittorrent"] = { {"fileSelectionState", "awaiting"} };
        j["files"] = nlohmann::json::array();
        for (const auto& file : t.torrentFiles) {
            j["files"].push_back({
                {"index", file.index}, {"path", file.path}, {"length", file.length},
                {"selected", file.selected}
            });
        }
    }
    if (!t.error.empty()) j["error"] = t.error;
    return j;
}

std::string handleList(const std::vector<std::string>& tokens) {
    const std::string filter = flagValue(tokens, "--state", "all");
    auto tasks = g_tasks.snapshot();
    if (filter == "active") {
        std::erase_if(tasks, [](const dl::TaskView& t) { return !isActive(t); });
    } else if (filter == "done") {
        std::erase_if(tasks, [](const dl::TaskView& t) { return t.state != dl::State::Done; });
    } else if (filter == "failed") {
        std::erase_if(tasks, [](const dl::TaskView& t) {
            return t.state != dl::State::Failed && t.state != dl::State::Cancelled;
        });
    } else if (filter != "all") {
        return "error: unknown --state '" + filter + "' (use active|done|failed|all)\n";
    }

    if (wantsJson(tokens)) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& t : tasks) arr.push_back(taskJson(t));
        return nlohmann::json{{"ok", true},
                              {"app", {{"version", std::string(cfg::kAppVersion)}}},
                              {"count", arr.size()},
                              {"tasks", std::move(arr)}}
            .dump(2);
    }

    std::ostringstream o;
    o << std::left;
    o << "ID    STATE        PROGRESS  SPEED      SIZE                       NAME\n";
    for (const auto& t : tasks) {
        std::string size;
        if (t.totalBytes >= 0) size = fmtSize(t.downloadedBytes) + " / " + fmtSize(t.totalBytes);
        else size = fmtSize(t.downloadedBytes) + " / ?";
        o << std::setw(6) << t.id << std::setw(13) << stateName(t.state)
          << std::setw(10) << fmtPct(progressPct(t))
          << std::setw(11)
          << (t.state == dl::State::Downloading && t.speedBps > 0
                  ? formatSpeed(t.speedBps)
                  : (t.state == dl::State::Downloading ? "-" : ""))
          << std::setw(27) << size << taskDisplayName(t) << "\n";
    }
    o << "# " << tasks.size() << " task(s)";
    if (filter != "all") o << " (state=" << filter << ")";
    o << "\n";
    return o.str();
}

// ---- 变更类命令（结果逐条汇报；全部成功才 ok）----
struct ActionResult {
    bool ok = false;
    std::string message;   // ok 时为动作描述，失败时含原因（不带 "error:" 前缀）
};

// 通用：把逐条结果拼成响应（控制面输出是稳定英文，不受界面语言影响，不经 i18n）。
std::string finishActions(const std::string& verb,
                          const std::vector<ActionResult>& results,
                          const std::vector<std::string>& tokens) {
    bool allOk = true;
    for (const auto& r : results) {
        if (!r.ok) allOk = false;
    }
    if (wantsJson(tokens)) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : results) {
            arr.push_back({{"action", verb}, {"ok", r.ok}, {"message", r.message}});
        }
        return nlohmann::json{{"ok", allOk}, {"results", std::move(arr)}}.dump(2);
    }
    std::ostringstream o;
    for (const auto& r : results) {
        o << (r.ok ? "ok: " : "error: ") << r.message << "\n";
    }
    return o.str();
}

std::vector<std::uint64_t> parseIds(const std::vector<std::string>& args,
                                    std::vector<std::string>& bad) {
    std::vector<std::uint64_t> ids;
    for (const auto& a : args) {
        if (a == "all") continue;
        try {
            ids.push_back(std::stoull(a));
        } catch (...) {
            bad.push_back(a);
        }
    }
    return ids;
}

std::string handlePause(const std::vector<std::string>& tokens,
                        const std::vector<std::string>& args) {
    if (args.empty()) return "error: usage: tinynext pause <id...|all>\n";
    if (std::find(args.begin(), args.end(), "all") != args.end()) {
        g_tasks.pauseAll();
        return wantsJson(tokens)
                   ? nlohmann::json{{"ok", true}, {"message", "paused all active tasks"}}.dump(2)
                   : "ok: paused all active tasks\n";
    }
    std::vector<std::string> bad;
    const auto ids = parseIds(args, bad);
    const auto snapshot = g_tasks.snapshot();
    std::vector<ActionResult> results;
    for (const auto id : ids) {
        const dl::TaskView* t = nullptr;
        for (const auto& v : snapshot) {
            if (v.id == id) { t = &v; break; }
        }
        if (!t) {
            results.push_back({false, "no task #" + std::to_string(id)});
        } else if (t->state != dl::State::Queued && t->state != dl::State::Downloading) {
            results.push_back({false, "#" + std::to_string(id) + " is " + stateName(t->state) +
                                          " (pause needs a queued/downloading task)"});
        } else {
            g_tasks.pause(id);
            results.push_back({true, "paused #" + std::to_string(id) + " " + taskDisplayName(*t)});
        }
    }
    for (const auto& b : bad) results.push_back({false, "invalid task id '" + b + "'"});
    return finishActions("pause", results, tokens);
}

std::string handleResume(const std::vector<std::string>& tokens,
                         const std::vector<std::string>& args) {
    if (args.empty()) return "error: usage: tinynext resume <id...|all>\n";
    if (std::find(args.begin(), args.end(), "all") != args.end()) {
        g_tasks.resumeAll();
        return wantsJson(tokens)
                   ? nlohmann::json{{"ok", true}, {"message", "resumed all paused tasks"}}.dump(2)
                   : "ok: resumed all paused tasks\n";
    }
    std::vector<std::string> bad;
    const auto ids = parseIds(args, bad);
    const auto snapshot = g_tasks.snapshot();
    std::vector<ActionResult> results;
    for (const auto id : ids) {
        const dl::TaskView* t = nullptr;
        for (const auto& v : snapshot) {
            if (v.id == id) { t = &v; break; }
        }
        if (!t) {
            results.push_back({false, "no task #" + std::to_string(id)});
        } else if (t->state != dl::State::Paused) {
            results.push_back({false, "#" + std::to_string(id) + " is " + stateName(t->state) +
                                          " (resume needs a paused task)"});
        } else {
            g_tasks.resume(id);
            results.push_back({true, "resumed #" + std::to_string(id) + " " + taskDisplayName(*t)});
        }
    }
    for (const auto& b : bad) results.push_back({false, "invalid task id '" + b + "'"});
    return finishActions("resume", results, tokens);
}

std::string handleCancel(const std::vector<std::string>& tokens,
                         const std::vector<std::string>& args) {
    if (args.empty()) return "error: usage: tinynext cancel <id...>\n";
    std::vector<std::string> bad;
    const auto ids = parseIds(args, bad);
    const auto snapshot = g_tasks.snapshot();
    std::vector<ActionResult> results;
    for (const auto id : ids) {
        const dl::TaskView* t = nullptr;
        for (const auto& v : snapshot) {
            if (v.id == id) { t = &v; break; }
        }
        if (!t) {
            results.push_back({false, "no task #" + std::to_string(id)});
        } else if (t->state == dl::State::Done || t->state == dl::State::Failed ||
                   t->state == dl::State::Cancelled) {
            results.push_back({false, "#" + std::to_string(id) + " is already " +
                                          stateName(t->state) + " (nothing to cancel)"});
        } else {
            g_tasks.cancel(id);
            results.push_back({true, "cancelled #" + std::to_string(id) + " " + taskDisplayName(*t) +
                                      " (partial file kept, retry with: tinynext retry " +
                                      std::to_string(id) + ")"});
        }
    }
    for (const auto& b : bad) results.push_back({false, "invalid task id '" + b + "'"});
    return finishActions("cancel", results, tokens);
}

std::string handleRetry(const std::vector<std::string>& tokens,
                        const std::vector<std::string>& args) {
    if (args.empty()) return "error: usage: tinynext retry <id...>\n";
    std::vector<std::string> bad;
    const auto ids = parseIds(args, bad);
    const auto snapshot = g_tasks.snapshot();
    std::vector<ActionResult> results;
    for (const auto id : ids) {
        const dl::TaskView* t = nullptr;
        for (const auto& v : snapshot) {
            if (v.id == id) { t = &v; break; }
        }
        if (!t) {
            results.push_back({false, "no task #" + std::to_string(id)});
        } else if (t->state != dl::State::Failed && t->state != dl::State::Cancelled) {
            results.push_back({false, "#" + std::to_string(id) + " is " + stateName(t->state) +
                                          " (retry needs a failed/cancelled task)"});
        } else {
            g_tasks.retry(id);
            results.push_back({true, "retry requested for #" + std::to_string(id) + " " +
                                      taskDisplayName(*t) + " (resumes from aria2-next state)"});
        }
    }
    for (const auto& b : bad) results.push_back({false, "invalid task id '" + b + "'"});
    return finishActions("retry", results, tokens);
}

std::string handleRemove(const std::vector<std::string>& tokens,
                         const std::vector<std::string>& args) {
    if (args.empty()) return "error: usage: tinynext remove <id...>\n";
    std::vector<std::string> bad;
    const auto ids = parseIds(args, bad);
    const auto snapshot = g_tasks.snapshot();
    std::vector<ActionResult> results;
    for (const auto id : ids) {
        const dl::TaskView* t = nullptr;
        for (const auto& v : snapshot) {
            if (v.id == id) { t = &v; break; }
        }
        if (!t) {
            results.push_back({false, "no task #" + std::to_string(id)});
        } else {
            // 只删记录与内核恢复数据；已下载文件不动——删源文件请走 UI
            // 删除弹窗（有回收站/永久删除选项，需要用户决策，不适合 CLI 盲操作）。
            g_tasks.deleteRecord(*t);
            results.push_back({true, "removed record #" + std::to_string(id) + " " +
                                      taskDisplayName(*t)});
        }
    }
    for (const auto& b : bad) results.push_back({false, "invalid task id '" + b + "'"});
    return finishActions("remove", results, tokens);
}

std::string handleClear(const std::vector<std::string>& tokens,
                        const std::vector<std::string>& args) {
    if (args.empty() || args[0] != "done") {
        return "error: usage: tinynext clear done   (remove all completed records)\n";
    }
    const auto snapshot = g_tasks.snapshot();
    int n = 0;
    for (const auto& t : snapshot) {
        if (t.state == dl::State::Done) {
            g_tasks.deleteRecord(t);
            ++n;
        }
    }
    return wantsJson(tokens)
               ? nlohmann::json{{"ok", true}, {"removed", n}}.dump(2)
               : "ok: removed " + std::to_string(n) + " completed record(s)\n";
}

} // namespace

// 执行一条控制命令（tokens[0]=动词）。只读动词在 IPC 监听线程直接跑；变更
// 动词由 UI 线程回调（见文件头线程约定）。返回给 CLI 客户端的完整响应。
export std::string handle(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return "error: empty control request\n";
    const std::string& verb = tokens[0];
    const auto args = positional(tokens);
    if (verb == "status") return handleStatus(tokens);
    if (verb == "list") return handleList(tokens);
    if (verb == "watch") return handleStatus(tokens);  // 客户端循环；服务端按 status 答
    if (verb == "pause") return handlePause(tokens, args);
    if (verb == "resume") return handleResume(tokens, args);
    if (verb == "cancel") return handleCancel(tokens, args);
    if (verb == "retry") return handleRetry(tokens, args);
    if (verb == "remove") return handleRemove(tokens, args);
    if (verb == "clear") return handleClear(tokens, args);
    if (verb == "quit") {
        return wantsJson(tokens) ? nlohmann::json{{"ok", true}, {"message", "TinyNext is quitting"}}.dump(2)
                                 : "ok: TinyNext is quitting (session saved on exit)\n";
    }
    return "error: unknown command '" + verb + "' — try: tinynext agent\n";
}

} // namespace cli_control
