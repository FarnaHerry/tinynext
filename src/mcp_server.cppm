// mcp_server.cppm — Model Context Protocol (MCP) stdio server（AI 助手伴生入口）。
//
// MCP 客户端（Claude Desktop / Claude Code / ZCode / Cursor…）按 mcpServers 配置
// 以 stdio 拉起 `tinynext --mcp`，本模块在 stdin/stdout 上按 MCP 规范应答：
// 换行分隔的 JSON-RPC 2.0（UTF-8、无内嵌换行；与 LSP 的 Content-Length 帧不同，
// 也与浏览器宿主的 u32 长度前缀不同）。绝不进 GUI、不抢单实例锁——CliBoot 在
// 浏览器宿主之后接管并 std::exit。
//
// 设计（见 docs/mcp.md 与 roadmap 2026-10-11 决策）：
//  * 不做第二个状态源：每个工具调用都翻译成控制面动词（status/list/pause/…，
//    走 <temp>/tinynext.port + TINYNEXT-CLI/2 握手），任务语义（seq/失败闸门/
//    会话恢复）留在主实例；本进程只是协议适配器。
//  * 参考实现取舍：Aria2 Agent（Python，8 工具，防幻影文件安全规则）与
//    aria2-mcp-rs（Rust，17 工具 + HTTP/SSE + RSS/调度）都直连 aria2 RPC，需要
//    自行补应用层语义；本 server 面向 TinyNext 任务模型，v1 收敛 7 个工具，
//    HTTP 传输 / MCP resources / 实时通知列 V2。
//  * 主实例未运行：只读工具返回明确提示；add_download 走转发路径（落 inbox +
//    拉起 GUI 进托盘），与 `tinynext <url>` 语义一致。
//  * 不暴露 quit（AI 不该随手关应用）。变更动词在托盘待机时的 10s 超时转成
//    可读提示（控制面限制，见 cli.cppm）。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif
#include <cstdio>  // stdout（NDJSON 输出）

#include "versions.generated.h"  // TINY_APP_VERSION

export module tinynext.mcp_server;

import std;
import nlohmann.json;
import tinynext.utils;         // isDownloadableSource
import tinynext.browser_host;  // spawnAppDetached（add_download 自动启动）

namespace mcp {

// cli 注入的回调（避免模块循环依赖，同 browser_host 模式）：
//   CtlFn    — 发一条控制动词行（如 "status --json"，不带 "ctl:" 前缀，由注入方
//              补齐线路格式）给运行中的主实例并取回响应；返回 false = 主实例
//              不可达/未运行。
//   ForwardFn — URL 行转发（TCP 直连，失败落 inbox），返回 false = 主实例未运行。
using CtlFn = std::function<bool(const std::string& ctlLine, std::string& response)>;
using ForwardFn = std::function<bool(const std::vector<std::string>& urls)>;

// MCP 协议版本：客户端提供的版本在我们支持列表内就回显，否则用我们最新的。
constexpr const char* kProtocolVersion = "2025-06-18";

bool writeMessage(const nlohmann::json& j) {
    std::string line = j.dump();
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), stdout);
    return std::fflush(stdout) == 0;
}

nlohmann::json makeResult(const nlohmann::json& id, nlohmann::json result) {
    nlohmann::json r;
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["result"] = std::move(result);
    return r;
}

nlohmann::json makeError(const nlohmann::json& id, int code,
                         const std::string& message) {
    nlohmann::json r;
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["error"] = {{"code", code}, {"message", message}};
    return r;
}

// 工具结果信封：text 内容 + 可选 isError（模型据此判断失败）。
nlohmann::json toolResult(const std::string& text, bool isError) {
    nlohmann::json r;
    r["content"] = nlohmann::json::array(
        {nlohmann::json{{"type", "text"}, {"text", text}}});
    if (isError) r["isError"] = true;
    return r;
}

// ---- 工具定义（tools/list）----

const nlohmann::json& toolDefs() {
    static const nlohmann::json tools = nlohmann::json::array({
        nlohmann::json{
            {"name", "get_status"},
            {"description",
             "TinyNext 下载器概览：引擎健康（daemon/RPC/WebSocket）、任务计数"
             "（排队/下载/暂停/完成/失败）、当前速度与下载目录。"},
            {"inputSchema",
             {{"type", "object"}, {"properties", nlohmann::json::object()}}}},
        nlohmann::json{
            {"name", "list_downloads"},
            {"description",
             "列出下载任务（id、状态、进度、速度、名称、URL、错误）。可按状态"
             "过滤、按名称/URL 子串搜索。state=failed 同时含 Failed 与 "
             "Cancelled；任务操作类工具需要的 id 从这里取。"},
            {"inputSchema",
             {{"type", "object"},
              {"properties",
               {{"state",
                 {{"type", "string"},
                  {"enum", nlohmann::json::array({"all", "active", "done",
                                                  "failed"})},
                  {"description", "状态过滤，默认 all（active=排队+下载+暂停）"},
                  {"default", "all"}}},
                {"query",
                 {{"type", "string"},
                  {"description",
                   "按任务名或 URL 的子串过滤（不区分大小写）"}}}}}}}},
        nlohmann::json{
            {"name", "add_download"},
            {"description",
             "提交一个下载到 TinyNext（http/https/ftp/sftp/magnet/ed2k 链接）"
             "。应用未运行时会自动启动它（直接进系统托盘，不弹窗）；提交后用 "
             "list_downloads 轮询进度。每个任务的重命名/目录等高级选项请在 "
             "TinyNext 界面里改。"},
            {"inputSchema",
             {{"type", "object"},
              {"properties",
               {{"url", {{"type", "string"}, {"description", "下载链接"}}}}},
              {"required", nlohmann::json::array({"url"})}}}},
        nlohmann::json{
            {"name", "pause"},
            {"description", "暂停任务。传 ids（单个或多个任务 id）或 all=true"
                            "（暂停全部排队/进行中的任务）。"},
            {"inputSchema",
             {{"type", "object"},
              {"properties",
               {{"ids",
                 {{"type", "array"},
                  {"items", {{"type", "integer"}}},
                  {"description", "任务 id 列表"}}},
                {"all", {{"type", "boolean"},
                         {"description", "暂停全部"}}}}}}}},
        nlohmann::json{
            {"name", "resume"},
            {"description", "恢复已暂停的任务。传 ids 或 all=true。等待用户在"
                            "界面里选择种子文件/媒体轨道的任务会被拒绝。"},
            {"inputSchema",
             {{"type", "object"},
              {"properties",
               {{"ids",
                 {{"type", "array"},
                  {"items", {{"type", "integer"}}},
                  {"description", "任务 id 列表"}}},
                {"all", {{"type", "boolean"}, {"description", "恢复全部"}}}}}}}},
        nlohmann::json{
            {"name", "cancel"},
            {"description", "取消进行中/排队的任务（保留已下载的部分文件，可用 "
                            "retry 续传）。只接受进行中/排队的任务。"},
            {"inputSchema",
             {{"type", "object"},
              {"properties",
               {{"ids",
                 {{"type", "array"}, {"items", {{"type", "integer"}}},
                  {"description", "任务 id 列表"}}}}},
              {"required", nlohmann::json::array({"ids"})}}}},
        nlohmann::json{
            {"name", "retry"},
            {"description", "重试失败/已取消的任务（复用原 GID 与断点数据续传）。"},
            {"inputSchema",
             {{"type", "object"},
              {"properties",
               {{"ids",
                 {{"type", "array"}, {"items", {{"type", "integer"}}},
                  {"description", "任务 id 列表"}}}}},
              {"required", nlohmann::json::array({"ids"})}}}},
        nlohmann::json{
            {"name", "remove"},
            {"description", "删除任务记录（不动已下载的磁盘文件）。任意状态可删。"},
            {"inputSchema",
             {{"type", "object"},
              {"properties",
               {{"ids",
                 {{"type", "array"}, {"items", {{"type", "integer"}}},
                  {"description", "任务 id 列表"}}}}},
              {"required", nlohmann::json::array({"ids"})}}}},
        nlohmann::json{
            {"name", "wait_until_idle"},
            {"description",
             "阻塞等待直到没有排队/进行中的任务（暂停不算未完成），适合在 "
             "add_download 之后等待下载完成。到超时返回当前状态快照。"},
            {"inputSchema",
             {{"type", "object"},
              {"properties",
               {{"timeout_seconds",
                 {{"type", "integer"},
                  {"description", "最长等待秒数（1-3600），默认 60"}}}}}}}},
    });
    return tools;
}

// ---- ctl 响应处理 ----

// 主实例不可达（未运行/端口文件过期）时的统一提示。
nlohmann::json notRunning() {
    return toolResult(
        "TinyNext is not running. Start the TinyNext app first, or call "
        "add_download (it launches the app automatically).", true);
}

// ctl 原始响应 → 工具结果。JSON（--json 输出）原样透传；文本 error: 行转成
// isError + 可读提示（含托盘待机时 10s 超时的专门解释）。
nlohmann::json ctlToResult(const std::string& response) {
    if (response.find("did not answer within 10s") != std::string::npos) {
        return toolResult(
            "TinyNext did not answer: its window is hidden to the tray and "
            "task changes need the UI loop. Restore the window from the tray "
            "menu and retry (read-only tools like list_downloads work while "
            "hidden).", true);
    }
    std::string trimmed = response;
    while (!trimmed.empty() &&
           (trimmed.back() == '\n' || trimmed.back() == '\r')) {
        trimmed.pop_back();
    }
    if (!trimmed.empty() && trimmed.front() == '{') {
        try {
            const nlohmann::json parsed = nlohmann::json::parse(trimmed);
            const bool ok = parsed.value("ok", false);
            return toolResult(parsed.dump(2), !ok);
        } catch (...) {
            // 落到下面的文本分支
        }
    }
    const bool isErr = trimmed.rfind("error:", 0) == 0;
    return toolResult(trimmed.empty() ? "(empty response)" : trimmed, isErr);
}

// ids/all 参数 → ctl 位置参数（如 " 1 2 3" 或 " all"）。返回空串表示参数不合法。
std::string idsArgs(const nlohmann::json& args, bool allowAll) {
    const bool all = args.value("all", false);
    if (all) {
        return allowAll ? std::string(" all") : std::string();
    }
    const auto it = args.find("ids");
    if (it == args.end() || !it->is_array() || it->empty()) return {};
    std::string out;
    for (const auto& id : *it) {
        if (!id.is_number_integer()) return {};
        if (!out.empty()) out.push_back(' ');
        out += std::to_string(id.get<long long>());
    }
    return out.empty() ? std::string() : " " + out;
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return s;
}

// list_downloads：ctl "list --json [--state X]"，再做 query 子串过滤。
nlohmann::json toolList(CtlFn& ctl, const nlohmann::json& args) {
    std::string line = "list --json";
    const std::string state = args.value("state", "all");
    if (state != "all" && state != "active" && state != "done" &&
        state != "failed") {
        return toolResult("invalid state '" + state +
                          "' (use all|active|done|failed)", true);
    }
    if (state != "all") line += " --state " + state;

    bool reachable = false;
    const std::string response = [&] {
        std::string r;
        reachable = ctl(line, r);
        return r;
    }();
    if (!reachable) return notRunning();

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(response);
    } catch (...) {
        return ctlToResult(response);
    }
    if (!parsed.value("ok", false)) return ctlToResult(response);

    const std::string query =
        toLowerAscii(args.value("query", std::string()));
    if (query.empty()) return toolResult(parsed.dump(2), false);

    nlohmann::json filtered = nlohmann::json::array();
    if (parsed.contains("tasks") && parsed["tasks"].is_array()) {
        for (const auto& task : parsed["tasks"]) {
            const std::string name = toLowerAscii(task.value("name", ""));
            const std::string url = toLowerAscii(task.value("url", ""));
            if (name.find(query) != std::string::npos ||
                url.find(query) != std::string::npos) {
                filtered.push_back(task);
            }
        }
    }
    nlohmann::json out = {{"ok", true},
                          {"count", filtered.size()},
                          {"query", args.value("query", std::string())},
                          {"tasks", std::move(filtered)}};
    return toolResult(out.dump(2), false);
}

// add_download：本地白名单校验 → 转发路径（TCP/inbox + 必要时拉起 GUI）。
nlohmann::json toolAdd(ForwardFn& forwardUrls, const nlohmann::json& args) {
    const std::string url = args.value("url", "");
    if (url.empty() || !isDownloadableSource(url)) {
        return toolResult(
            "unsupported url: TinyNext accepts http(s)://, ftp://, sftp://, "
            "ed2k://|file|, magnet: links and local .torrent paths", true);
    }
    const bool delivered = forwardUrls(std::vector<std::string>{url});
    if (delivered) {
        return toolResult("submitted to the running TinyNext instance. Use "
                          "list_downloads to watch progress.", false);
    }
    // 主实例不在：URL 已落 inbox，拉起 GUI（首帧后进托盘并消费 inbox）。
    browser_host::spawnAppDetached();
    return toolResult(
        "TinyNext was not running: the URL is queued and the app has been "
        "launched into the tray. It will pick the download up on startup; "
        "use list_downloads in a moment.", false);
}

// wait_until_idle：轮询 status --json 直到 queued+downloading==0 或超时。
nlohmann::json toolWait(CtlFn& ctl, const nlohmann::json& args) {
    long long timeout = args.value("timeout_seconds", 60LL);
    timeout = std::clamp<long long>(timeout, 1, 3600);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    std::string last;
    for (;;) {
        std::string response;
        if (!ctl("status --json", response)) {
            return notRunning();
        }
        last = response;
        try {
            const nlohmann::json parsed = nlohmann::json::parse(response);
            const auto& tasks = parsed.at("tasks");
            const long long busy = tasks.value("queued", 0LL) +
                                   tasks.value("downloading", 0LL);
            if (busy == 0) {
                nlohmann::json out = parsed;
                out["idle"] = true;
                return toolResult(out.dump(2), false);
            }
        } catch (...) {
            return ctlToResult(response);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            nlohmann::json out;
            try {
                out = nlohmann::json::parse(last);
            } catch (...) {
                return ctlToResult(last);
            }
            out["idle"] = false;
            out["waited_seconds"] = timeout;
            return toolResult(out.dump(2), false);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// tools/call 分发。
nlohmann::json callTool(CtlFn& ctl, ForwardFn& forwardUrls,
                        const nlohmann::json& params) {
    const std::string name = params.value("name", "");
    const nlohmann::json args =
        params.contains("arguments") && params["arguments"].is_object()
            ? params["arguments"]
            : nlohmann::json::object();

    if (name == "get_status") {
        std::string response;
        if (!ctl("status --json", response)) return notRunning();
        return ctlToResult(response);
    }
    if (name == "list_downloads") return toolList(ctl, args);
    if (name == "add_download") return toolAdd(forwardUrls, args);
    if (name == "wait_until_idle") return toolWait(ctl, args);

    // 变更类：pause/resume/cancel/retry/remove → ctl "<verb> --json <args>"
    static const std::array<std::string, 5> kMutating = {
        "pause", "resume", "cancel", "retry", "remove"};
    const auto verbIt = std::find(kMutating.begin(), kMutating.end(), name);
    if (verbIt != kMutating.end()) {
        const bool allowAll = name == "pause" || name == "resume";
        std::string ids = idsArgs(args, allowAll);
        if (ids.empty() && !(allowAll && args.value("all", false))) {
            return toolResult(
                "missing ids: pass an array of task ids (from "
                "list_downloads)" +
                std::string(allowAll ? " or all=true" : ""), true);
        }
        std::string response;
        if (!ctl(name + " --json" + ids, response)) return notRunning();
        return ctlToResult(response);
    }
    return toolResult("unknown tool: " + name, true);
}

} // namespace mcp

namespace mcp_server {

// 命令行是否为 MCP server 模式（显式 --mcp；MCP 客户端 mcpServers 配置拉起）。
export bool mcpServerRequested() {
    static const bool cached = [] {
        for (const auto& a : commandLineArgs()) {
            if (a == "--mcp") return true;
        }
        return false;
    }();
    return cached;
}

// MCP stdio 主循环：NDJSON JSON-RPC 2.0（换行分隔）。单线程顺序处理——stdio
// 客户端一次发一个请求，恰好匹配控制面「一次一连接」的约束。stdin EOF（客户端
// 断开/退出）即正常返回。ctl/forward 由 cli 注入（避免模块循环依赖）。
export int runMcpServer(
        mcp::CtlFn ctl, mcp::ForwardFn forwardUrls) {
#ifdef _WIN32
    // GUI 子系统下 MCP 客户端给 stdin/stdout 挂管道；文本模式会翻译换行破坏
    // NDJSON 帧，必须切二进制。
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) return 1;
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 1;
#endif
    std::string line;
    while (std::getline(std::cin, line)) {
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.empty()) continue;

        nlohmann::json msg;
        try {
            msg = nlohmann::json::parse(line);
        } catch (...) {
            if (!mcp::writeMessage(mcp::makeError(nullptr, -32700,
                                                  "Parse error"))) {
                return 1;
            }
            continue;
        }
        if (!msg.is_object() || !msg.contains("method") ||
            !msg["method"].is_string()) {
            continue;
        }
        // 通知（无 id）不回包；请求（有 id）必须应答。
        const bool isNotification = !msg.contains("id");
        const nlohmann::json id = isNotification
            ? nlohmann::json(nullptr)
            : msg["id"];
        const std::string method = msg["method"].get<std::string>();

        if (isNotification) {
            continue;  // notifications/initialized 等一律忽略
        }
        if (method == "initialize") {
            std::string requested = msg.value("params", nlohmann::json::object())
                                        .value("protocolVersion", "");
            // 已知版本回显，否则回到我们支持的最新版。
            static const std::array<std::string_view, 4> kKnown = {
                "2024-11-05", "2025-03-26", "2025-06-18", "2026-07-28"};
            std::string version = mcp::kProtocolVersion;
            if (std::find(kKnown.begin(), kKnown.end(), requested) !=
                kKnown.end()) {
                version = requested;
            }
            nlohmann::json result = {
                {"protocolVersion", version},
                {"capabilities",
                 {{"tools", {{"listChanged", false}}}}},
                {"serverInfo",
                 {{"name", "tinynext"}, {"version", TINY_APP_VERSION}}}};
            if (!mcp::writeMessage(mcp::makeResult(id, std::move(result)))) {
                return 1;
            }
            continue;
        }
        if (method == "ping") {
            if (!mcp::writeMessage(mcp::makeResult(id, nlohmann::json::object()))) {
                return 1;
            }
            continue;
        }
        if (method == "tools/list") {
            nlohmann::json result = {{"tools", mcp::toolDefs()}};
            if (!mcp::writeMessage(mcp::makeResult(id, std::move(result)))) {
                return 1;
            }
            continue;
        }
        if (method == "tools/call") {
            const nlohmann::json params =
                msg.contains("params") && msg["params"].is_object()
                    ? msg["params"]
                    : nlohmann::json::object();
            if (!mcp::writeMessage(mcp::makeResult(
                    id, mcp::callTool(ctl, forwardUrls, params)))) {
                return 1;
            }
            continue;
        }
        if (!mcp::writeMessage(
                mcp::makeError(id, -32601, "Method not found: " + method))) {
            return 1;
        }
    }
    return 0;  // stdin EOF：客户端断开/退出
}

} // namespace mcp_server
