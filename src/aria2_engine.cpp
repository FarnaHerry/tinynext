// aria2_engine.cpp — implementation unit for tinynext.aria2_engine.
//
// Spawns the bundled engines/aria2-next.exe (resolved next to the app exe,
// falling back to the working directory) with --enable-rpc, then drives it via
// plain-HTTP JSON-RPC on 127.0.0.1:<random-port>. The tiny RPC POST is written
// directly on a small local socket (LocalSocket below; no external HTTP dep).
// No worker threads: every operation is a synchronous RPC on the UI thread.

module;

#ifdef _WIN32
// windows.h is used for CreateProcess/TerminateProcess/HANDLE. LEAN_AND_MEAN
// keeps winsock.h out (winsock2.h is included explicitly for LocalSocket).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
// POSIX (macOS + Linux). Each header must be included explicitly: glibc pulls
// them in transitively so Linux "works by accident", but the macOS SDK does
// not — pid_t/waitpid/kill/posix_spawn all need their own header.
#include <sys/types.h>   // pid_t
#include <sys/wait.h>    // waitpid, WNOHANG
#include <signal.h>      // kill, SIGTERM, SIGKILL
#include <spawn.h>       // posix_spawn
#include <sys/socket.h>  // socket/connect/send/recv
#include <netinet/in.h>  // sockaddr_in
#include <arpa/inet.h>   // inet_pton
#include <fcntl.h>       // fcntl, O_NONBLOCK
#include <sys/select.h>  // fd_set, select
#include <cerrno>        // errno, EINPROGRESS, EAGAIN
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif
// macOS's <unistd.h> does not declare `environ` (glibc does); the symbol still
// exists in the system libc, so declare it here for the posix_spawn call below.
extern char** environ;
#endif

// IXWebSocket (compat.websocket)：WebSocket 客户端，用于收 aria2 的 RPC 推送事件
// （aria2.onDownloadStart/Complete/Error/...）。必须放在全局模块片段（普通 C++11 header）。
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>  // ix::initNetSystem（IXWebSocket.h 不会间接包含它）
#include <ixwebsocket/IXBase64.h>     // macaron::Base64::Encode（.torrent → base64 → aria2.addTorrent）

// eui 的 UI 唤醒：WS 推送事件改了任务状态后让 UI 重绘一帧（IXWebSocket 后台线程
// 调用，跨线程安全）。
namespace core::platform { void requestUiUpdate(); }

module tinynext.aria2_engine;

import std;
import nlohmann.json;
import tinynext.config;
import tinynext.i18n;   // tr / trf（引擎错误文案按语言）
import tinynext.sha256;

namespace dl {

namespace {

// ---- 极简跨平台 TCP socket（替代 tinyhttps::Socket，仅用于本地 JSON-RPC）----
#ifdef _WIN32
using LocalFd = SOCKET;
constexpr LocalFd kInvalidFd = INVALID_SOCKET;
#else
using LocalFd = int;
constexpr LocalFd kInvalidFd = -1;
#endif

class LocalSocket {
public:
    LocalSocket() = default;
    ~LocalSocket() { close(); }
    LocalSocket(const LocalSocket&) = delete;
    LocalSocket& operator=(const LocalSocket&) = delete;

    bool connect(const char* host, int port, int timeoutMs) {
        close();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == kInvalidFd) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        // host 在本项目里恒为字面 IP（127.0.0.1），不需要 getaddrinfo。
        if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            close();
            return false;
        }

        // 非阻塞 connect + select 实现超时（避免 connect 卡死 UI 线程）。
        setNonBlocking(true);
        const int rc = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc != 0) {
#ifdef _WIN32
            const bool inProgress = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            const bool inProgress = errno == EINPROGRESS || errno == EAGAIN;
#endif
            if (!inProgress || !waitWritable(timeoutMs)) {
                close();
                return false;
            }
        }
        setNonBlocking(false);
        return true;
    }

    int write(const char* data, int len) {
        if (fd_ == kInvalidFd) return -1;
#ifdef _WIN32
        return ::send(fd_, data, len, 0);
#else
        return static_cast<int>(::send(fd_, data, len, 0));
#endif
    }

    int read(char* buf, int size) {
        if (fd_ == kInvalidFd) return -1;
#ifdef _WIN32
        return ::recv(fd_, buf, size, 0);
#else
        return static_cast<int>(::recv(fd_, buf, size, 0));
#endif
    }

    bool waitReadable(int timeoutMs) {
        return waitOn(timeoutMs, false);
    }

    void close() {
        if (fd_ != kInvalidFd) {
#ifdef _WIN32
            ::closesocket(fd_);
#else
            ::close(fd_);
#endif
            fd_ = kInvalidFd;
        }
    }

    static bool platformInit() {
#ifdef _WIN32
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
        return true;
#endif
    }
    static void platformCleanup() {
#ifdef _WIN32
        WSACleanup();
#endif
    }

private:
    void setNonBlocking(bool enable) {
#ifdef _WIN32
        u_long mode = enable ? 1 : 0;
        ioctlsocket(fd_, FIONBIO, &mode);
#else
        const int flags = fcntl(fd_, F_GETFL, 0);
        if (flags < 0) return;
        fcntl(fd_, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
    }

    bool waitOn(int timeoutMs, bool wantWritable) {
        if (fd_ == kInvalidFd) return false;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
        const int rc = wantWritable ? select(0, nullptr, &fds, nullptr, &tv)
                                    : select(0, &fds, nullptr, nullptr, &tv);
#else
        const int rc = wantWritable ? select(fd_ + 1, nullptr, &fds, nullptr, &tv)
                                    : select(fd_ + 1, &fds, nullptr, nullptr, &tv);
#endif
        return rc > 0;
    }

    bool waitWritable(int timeoutMs) {
        return waitOn(timeoutMs, true);
    }

    LocalFd fd_ = kInvalidFd;
};

// ---- minimal HTTP/JSON-RPC client for the local daemon ----

bool writeAll(LocalSocket& sock, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        const int n = sock.write(data + sent, len - sent);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

// Read one HTTP response (Connection: close) from the socket; returns the body.
std::string readHttpResponse(LocalSocket& sock) {
    std::string header;
    header.reserve(512);
    while (header.find("\r\n\r\n") == std::string::npos) {
        if (!sock.waitReadable(5000)) return {};
        char c;
        const int n = sock.read(&c, 1);
        if (n <= 0) return {};
        header.push_back(c);
        if (header.size() > 65536) return {};
    }

    // Parse Content-Length (case-insensitive).
    std::int64_t contentLength = -1;
    std::size_t pos = 0;
    while ((pos = header.find("\r\n", pos)) != std::string::npos) {
        pos += 2;
        const std::size_t end = header.find("\r\n", pos);
        if (end == std::string::npos) break;
        const std::string line = header.substr(pos, end - pos);
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        for (auto& ch : key) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
        }
        if (key == "content-length") {
            contentLength = 0;
            for (char ch : line.substr(colon + 1)) {
                if (ch >= '0' && ch <= '9') {
                    contentLength = contentLength * 10 + (ch - '0');
                }
            }
            break;
        }
    }

    const std::size_t bodyStart = header.find("\r\n\r\n") + 4;
    std::string body = header.substr(bodyStart);
    while (contentLength < 0 || static_cast<std::int64_t>(body.size()) < contentLength) {
        if (!sock.waitReadable(5000)) break;
        char buf[8192];
        const int n = sock.read(buf, sizeof(buf));
        if (n <= 0) break;
        body.append(buf, static_cast<std::size_t>(n));
    }
    if (contentLength >= 0 && static_cast<std::int64_t>(body.size()) > contentLength) {
        body.resize(static_cast<std::size_t>(contentLength));
    }
    return body;
}

std::string httpPost(int port, const std::string& body) {
    LocalSocket sock;
    if (!sock.connect("127.0.0.1", port, 3000)) return {};
    const std::string request =
        "POST /jsonrpc HTTP/1.1\r\n"
        "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    if (!writeAll(sock, request.data(), static_cast<int>(request.size()))) return {};
    return readHttpResponse(sock);
}

// JSON-RPC call; throws std::runtime_error on transport/JSON/error responses.
nlohmann::json rpcCall(int port, const std::string& secret,
                       const std::string& method, const nlohmann::json& params) {
    nlohmann::json request = nlohmann::json::object();
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = method;
    nlohmann::json fullParams = nlohmann::json::array();
    fullParams.push_back("token:" + secret);
    if (params.is_array()) {
        for (const auto& p : params) fullParams.push_back(p);
    } else {
        fullParams.push_back(params);
    }
    request["params"] = fullParams;

    const std::string raw = httpPost(port, request.dump());
    if (raw.empty()) throw std::runtime_error("rpc: no response from daemon");
    nlohmann::json response = nlohmann::json::parse(raw);
    if (response.contains("error")) {
        throw std::runtime_error("rpc error: " + response["error"].dump());
    }
    return response["result"];
}

std::string randomHex(std::size_t n) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) s.push_back(kHex[dist(rng)]);
    return s;
}

int pickFreePort() {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(20000, 50000);
    for (int attempt = 0; attempt < 16; ++attempt) {
        const int candidate = dist(rng);
        LocalSocket probe;
        if (!probe.connect("127.0.0.1", candidate, 300)) {
            return candidate;  // nothing listening → free
        }
    }
    return 16800;
}

// daemon 级参数（除分片/连接数外的全部 --xxx 选项）：仅非空/非默认才传，
// 避免多余 flag。会话恢复（--save-session / --input-file）与磁力 BT 相关
// flag 也在这里统一生成。
std::vector<std::pair<std::string, std::string>> daemonExtraOpts(
    const cfg::Aria2Config& a2) {
    std::vector<std::pair<std::string, std::string>> opts;
    auto add = [&](const char* name, const std::string& value) {
        opts.emplace_back(name, value);
    };
    if (!a2.proxy.empty()) add("all-proxy", a2.proxy);
    if (!a2.noProxy.empty()) add("no-proxy", a2.noProxy);
    if (a2.maxTries > 0) add("max-tries", std::to_string(a2.maxTries));
    if (a2.retryWait > 0) add("retry-wait", std::to_string(a2.retryWait));
    add("max-concurrent-downloads", std::to_string(a2.maxConcurrentDownloads));
    if (a2.removeControlFile) add("remove-control-file", "true");
    if (!a2.onDownloadComplete.empty()) {
        add("on-download-complete", a2.onDownloadComplete);
    }
    if (!a2.userAgent.empty()) add("user-agent", a2.userAgent);
    if (!a2.referer.empty()) add("referer", a2.referer);
    if (!a2.diskCache.empty()) add("disk-cache", a2.diskCache);
    // ---- BitTorrent ----
    if (a2.seedTime > 0) add("seed-time", std::to_string(a2.seedTime));
    if (a2.seedRatio > 0.0) add("seed-ratio", std::to_string(a2.seedRatio));
    if (a2.btMaxPeers > 0) add("bt-max-peers", std::to_string(a2.btMaxPeers));
    if (!a2.listenPort.empty()) add("listen-port", a2.listenPort);
    if (a2.btEnableLpd) add("bt-enable-lpd", "true");
    // --bt-tracker：额外 tracker 列表（换行/逗号分隔 → 逗号连接）。全局对所有
    // 磁力/BT 任务生效（aria2 是「额外」tracker，不覆盖种子自带）。
    if (!a2.btTracker.empty()) {
        std::string text = a2.btTracker;
        for (char& ch : text) {
            if (ch == '\n' || ch == '\r') ch = ',';
        }
        std::string joined;
        std::istringstream ss(text);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            const std::size_t f = tok.find_first_not_of(" \t\r\n");
            const std::size_t l = tok.find_last_not_of(" \t\r\n");
            if (f == std::string::npos) continue;
            const std::string tt = tok.substr(f, l - f + 1);
            if (!tt.empty()) {
                if (!joined.empty()) joined += ",";
                joined += tt;
            }
        }
        if (!joined.empty()) add("bt-tracker", joined);
    }
    // ---- HTTP ----
    // --header 每条一个 flag：配置里多行 → 拆成多个 --header 参数。
    if (!a2.header.empty()) {
        std::size_t pos = 0;
        while (pos <= a2.header.size()) {
            const std::size_t nl = a2.header.find('\n', pos);
            const std::size_t end = nl == std::string::npos ? a2.header.size() : nl;
            std::string line = a2.header.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) add("header", line);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
    if (!a2.loadCookies.empty()) add("load-cookies", a2.loadCookies);
    if (!a2.saveCookies.empty()) add("save-cookies", a2.saveCookies);
    // ---- 下载行为 ----
    if (a2.maxOverallDownloadLimit > 0) {
        add("max-overall-download-limit", std::to_string(a2.maxOverallDownloadLimit));
    }
    if (!a2.fileAllocation.empty()) add("file-allocation", a2.fileAllocation);
    // 从硬编码命令行移过来的可配项（默认值与原来一致：自动改名开、覆盖关）。
    add("auto-file-renaming", a2.autoFileRenaming ? "true" : "false");
    add("allow-overwrite", a2.allowOverwrite ? "true" : "false");
    // ---- 完整性校验 ----
    if (a2.checkIntegrity) add("check-integrity", "true");
    if (!a2.checksum.empty()) add("checksum", a2.checksum);
    // ---- ED2K（aria2-next 原生支持电驴：ed2k:// 链接 / 服务器发现）----
    if (!a2.ed2kServers.empty()) add("ed2k-server", a2.ed2kServers);
    if (!a2.ed2kListenPort.empty()) add("ed2k-listen-port", a2.ed2kListenPort);
    if (!a2.ed2kUdpListenPort.empty()) add("ed2k-udp-listen-port", a2.ed2kUdpListenPort);
    if (a2.ed2kUploadSlots > 0) {
        add("ed2k-upload-slots", std::to_string(a2.ed2kUploadSlots));
    }
    // 会话恢复：shutdown 前用 aria2.saveSession 持久化未完成任务，下次启动用
    // --input-file 载入续传。首次运行会话文件不存在，跳过 --input-file。
    // 会话文件放 per-user 配置目录：安装版经快捷方式启动时 cwd 可能是 System32
    // （不可写），不能依赖 cwd。
    const std::filesystem::path sessionPath = cfg::configDir() / "tinynext.session";
    std::error_code ec;
    std::filesystem::create_directories(sessionPath.parent_path(), ec);
    add("save-session", sessionPath.string());
    if (std::filesystem::exists(sessionPath, ec)) {
        add("input-file", sessionPath.string());
    }
    // 磁力/BT：保存元数据为 .torrent + 显式开启 DHT / PEX。
    add("bt-save-metadata", "true");
    add("enable-dht", "true");
    add("enable-peer-exchange", "true");
    return opts;
}

std::string engineExePath() {
    std::filesystem::path exeDir;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        exeDir = std::filesystem::path(buf).parent_path();
    }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        exeDir = std::filesystem::path(buf).parent_path();
    }
#else  // Linux
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) exeDir = self.parent_path();
#endif
    // 二进制名：Windows 带 .exe，unix 不带。
    const char* name = "aria2-next.exe";
#ifndef _WIN32
    name = "aria2-next";
#endif
    for (const std::filesystem::path& base : {exeDir, std::filesystem::current_path()}) {
        if (base.empty()) continue;
        const std::filesystem::path candidate = base / "engines" / name;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate.string();
    }
    return {};
}

// 当前平台的 checksums.sha256 条目标签（release 资产名后缀），如
// "windows-x86_64" / "linux-x86_64" / "macos-arm64"。OS×架构各恰好一个条目。
std::string platformTag() {
#ifdef _WIN32
    const char* os = "windows";
#elif defined(__APPLE__)
    const char* os = "macos";
#else
    const char* os = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    const char* arch = "arm64";
#else
    const char* arch = "x86_64";
#endif
    return std::string(os) + "-" + arch;
}

// 从 engines/checksums.sha256 里查本平台引擎二进制的期望 SHA-256（小写）。
// 条目文件名是 release 资产名（aria2-next-<ver>-<平台>[.exe]），与磁盘上
// 改名后的运行时文件名不同，所以按平台标签做后缀匹配。无匹配返回空串。
std::string lookupExpectedChecksum(const std::filesystem::path& manifest) {
    const std::string tag = platformTag();
    const std::string suffix[] = {"-" + tag, "-" + tag + ".exe"};
    std::ifstream in(manifest);
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 64) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF 兜底
        std::string hash = line.substr(0, 64);
        for (auto& c : hash) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::size_t i = 64;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const std::string name = line.substr(i);
        for (const auto& s : suffix) {
            if (name.size() >= s.size() &&
                name.compare(name.size() - s.size(), s.size(), s) == 0) {
                return hash;
            }
        }
    }
    return {};
}

// 跨平台进程助手。processHandle_ 在 Windows 上是 HANDLE，POSIX 上是 pid_t
// （经 intptr_t 存放）。
bool processExited(void* handle) {
    if (!handle) return true;
#ifdef _WIN32
    return WaitForSingleObject(static_cast<HANDLE>(handle), 0) == WAIT_OBJECT_0;
#else
    int status = 0;
    return ::waitpid(static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle)),
                     &status, WNOHANG) > 0;
#endif
}

void terminateProcess(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    TerminateProcess(static_cast<HANDLE>(handle), 0);
    CloseHandle(static_cast<HANDLE>(handle));
#else
    const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle));
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {  // ~1s 宽限
        if (::waitpid(pid, nullptr, WNOHANG) > 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
#endif
}

} // namespace

// WebSocket 事件监听（compat.websocket / IXWebSocket）：只连 aria2 的 RPC WS 端点
// 收推送事件（aria2.onDownloadStart/Complete/Error/...），不发请求（请求仍走 HTTP
// rpcCall）。IXWebSocket 的 start() 起后台线程，onMessage 回调在该线程执行，因此
// 事件回调里取 tasksMutex_ 更新任务状态（短暂持锁）。stop() 会 join 线程，保证
// 之后不再有回调执行。
struct WsNotifier {
    using EventFn = std::function<void(const std::string& method, const std::string& gid)>;

    ix::WebSocket ws;
    EventFn onEvent;
    std::atomic<bool> connected{false};

    WsNotifier(int port, EventFn fn) : onEvent(std::move(fn)) {
        ws.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/jsonrpc");
        ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    connected = true;
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    connected = false;
                    break;
                case ix::WebSocketMessageType::Message:
                    // 推送帧形如：
                    // {"jsonrpc":"2.0","method":"aria2.onDownloadComplete",
                    //  "params":["<gid>"]}
                    if (msg->str.empty() || msg->str[0] != '{') break;
                    try {
                        const nlohmann::json j = nlohmann::json::parse(msg->str);
                        if (!j.is_object()) break;
                        const std::string method = j.value("method", "");
                        if (method.rfind("aria2.on", 0) == 0 && j.contains("params") &&
                            j["params"].is_array() && !j["params"].empty()) {
                            onEvent(method, j["params"][0].get<std::string>());
                        }
                    } catch (...) {}
                    break;
                default:
                    break;
            }
        });
    }

    void start() { ws.start(); }
    void stop() { ws.stop(); }
    bool isConnected() const { return connected.load(); }
};

struct Aria2Engine::Task {
    std::uint64_t id;
    std::string gid;
    std::string url;
    std::filesystem::path destPath;
    State state = State::Queued;
    std::int64_t totalBytes = -1;
    std::int64_t downloadedBytes = 0;
    std::string error;
    double speedBps = 0.0;
    int connections = 1;          // numConnections from tellStatus
    StartOptions opts;            // 该任务的起始选项（retry 时复用）
    bool needsFinalize = false;   // WS 事件置位：下一轮 poll 补一次 tellStatus 拿最终字节/错误
    std::string displayName;      // BT/磁力拿到元数据后的真实名（bittorrent.info.name）
    std::vector<dl::MirrorSource> mirrors;  // 实时源列表（去重，aria2 uris 按 URL 合并，含状态）
};

Aria2Engine::Aria2Engine() {
    // Winsock for the local RPC socket (Windows only; POSIX no-op).
    LocalSocket::platformInit();
    // IXWebSocket 需要（Windows 上内部 WSAStartup；重复调用安全）。不调
    // ix::uninitNetSystem()：避免与 ~Aria2Engine 里 LocalSocket::platformCleanup
    // 的 WSACleanup 冲突（进程退出时 OS 会回收，无需显式清理）。
    ix::initNetSystem();
}

Aria2Engine::~Aria2Engine() {
    shutdown();
    LocalSocket::platformCleanup();
}

bool Aria2Engine::engineActive() const {
    std::lock_guard<std::mutex> lock(daemonMutex_);
    return daemonSpawned_;
}

std::string Aria2Engine::lastError() const {
    std::lock_guard<std::mutex> lock(daemonMutex_);
    return lastError_;
}

void Aria2Engine::warmup() {
    // 启动时后台拉起 daemon + 恢复上次会话历史任务（ensureDaemon 首次 spawn 时
    // 内部调 recoverSession + 启动 WS 监听）。与 UI 线程的 start()/retry() 经
    // daemonMutex_ 互斥，先到者完成 spawn，后到者直接复用。
    ensureDaemon();
}

bool Aria2Engine::ensureDaemon() const {
    // daemon 生命周期与 UI 线程并发（warmup 在后台线程调本方法），全程持
    // daemonMutex_ 串行化 spawn / RPC 就绪等待 / recoverSession / WS 启动。
    std::lock_guard<std::mutex> daemonLock(daemonMutex_);
    if (daemonSpawned_) return port_ != 0;

    const std::string exe = engineExePath();
    if (exe.empty()) {
        lastError_ = tr("未找到下载引擎二进制（engines/aria2-next(.exe)）",
                        "Engine binary not found (engines/aria2-next(.exe))");
        return false;
    }

    // 运行时完整性校验：engines/checksums.sha256 里本平台条目与本机二进制比对
    // （checksums 此前只在 CI 校验，运行时从不检查——被篡改的二进制会被静默执行）。
    // 本平台条目缺失 → fail-closed；清单整体缺失 → 警告继续（兼容历史分发，
    // make-dist.sh 修复后新装必带）。
    const std::filesystem::path manifest =
        std::filesystem::path(exe).parent_path() / "checksums.sha256";
    std::error_code mec;
    if (std::filesystem::exists(manifest, mec)) {
        const std::string expected = lookupExpectedChecksum(manifest);
        if (expected.empty()) {
            lastError_ = trf("下载引擎完整性校验失败：清单中无本平台条目（{}）",
                             "Engine integrity check failed: no entry for this platform ({})",
                             platformTag());
            return false;
        }
        const std::string actual = sha::fileSha256(exe);
        if (actual != expected) {
            lastError_ = tr("下载引擎完整性校验失败（checksum 不匹配，二进制可能被篡改）",
                            "Engine integrity check failed (checksum mismatch, binary may be tampered)");
            return false;
        }
    }

    const int port = pickFreePort();
    const std::string secret = randomHex(16);

    const cfg::Aria2Config a2 = cfg::aria2Config();
    const auto extra = daemonExtraOpts(a2);

#ifdef _WIN32
    const std::wstring wExe = std::filesystem::path(exe).wstring();
    // 值含空格时用引号包起来（aria2 的 cmdline 解析按 MSVCRT 规则分词）。
    const auto winValue = [](const std::string& v) -> std::wstring {
        std::wstring w(v.begin(), v.end());
        if (v.find_first_of(" \t") != std::string::npos) {
            return L"\"" + w + L"\"";
        }
        return w;
    };
    std::wstring cmdLine = L"\"" + wExe + L"\" --enable-rpc --rpc-listen-all=false "
                           L"--rpc-listen-port=" + std::to_wstring(port) + L" "
                           L"--rpc-secret=" + winValue(secret) + L" "
                           L"--max-connection-per-server=" +
                               std::to_wstring(a2.maxConnectionPerServer) + L" "
                           L"--split=" + std::to_wstring(a2.split) + L" "
                           L"--min-split-size=" + winValue(a2.minSplitSize) + L" "
                           L"--console-log-level=warn";
    for (const auto& [name, value] : extra) {
        cmdLine += L" --" + std::wstring(name.begin(), name.end()) + L"=" + winValue(value);
    }

    // 重定向 daemon 的 stdout/stderr 到日志文件，避免 aria2 的进度摘要 / 错误刷进
    // 应用终端（headless 下尤其吵，GUI 下 nohup 也会被污染）。日志与 session 同级。
    const std::filesystem::path logPath = cfg::configDir() / "tinynext-aria2.log";
    std::error_code lsec;
    std::filesystem::create_directories(logPath.parent_path(), lsec);
    const HANDLE logFile = CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (logFile != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = logFile;
        si.hStdError = logFile;
    }
    PROCESS_INFORMATION pi{};
    // STARTF_USESTDHANDLES 时子进程需继承这些句柄，bInheritHandles 必须 TRUE。
    if (!CreateProcessW(wExe.c_str(), &cmdLine[0], nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (logFile != INVALID_HANDLE_VALUE) CloseHandle(logFile);
        lastError_ = tr("下载引擎进程启动失败", "Engine process failed to start");
        return false;
    }
    if (logFile != INVALID_HANDLE_VALUE) CloseHandle(logFile);
    CloseHandle(pi.hThread);
    processHandle_ = pi.hProcess;
#else
    std::vector<std::string> args = {
        exe, "--enable-rpc", "--rpc-listen-all=false",
        "--rpc-listen-port=" + std::to_string(port),
        "--rpc-secret=" + secret,
        "--max-connection-per-server=" + std::to_string(a2.maxConnectionPerServer),
        "--split=" + std::to_string(a2.split),
        "--min-split-size=" + a2.minSplitSize,
        "--console-log-level=warn"};
    for (const auto& [name, value] : extra) {
        args.push_back("--" + name + "=" + value);
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    // 同 Windows 分支：daemon 输出重定向到 configDir/tinynext-aria2.log。
    const std::filesystem::path logPath = cfg::configDir() / "tinynext-aria2.log";
    std::error_code lsec;
    std::filesystem::create_directories(logPath.parent_path(), lsec);
    const int logFd = ::open(logPath.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (logFd >= 0) {
        // adddup2 到 stdout/stderr 会清掉 CLOEXEC，子进程正确继承。
        posix_spawn_file_actions_adddup2(&fa, logFd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, logFd, STDERR_FILENO);
    }

    pid_t pid = -1;
    if (::posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ) != 0) {
        if (logFd >= 0) ::close(logFd);
        posix_spawn_file_actions_destroy(&fa);
        lastError_ = tr("下载引擎进程启动失败", "Engine process failed to start");
        return false;
    }
    posix_spawn_file_actions_destroy(&fa);
    if (logFd >= 0) ::close(logFd);
    processHandle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
#endif

    // Wait until the RPC endpoint answers (up to ~3 s). If the spawned process
    // exits immediately (e.g. invalid args), fail fast instead of hanging the
    // UI thread on the full deadline.
    port_ = port;
    secret_ = secret;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (processHandle_ && processExited(processHandle_)) {
            break;  // daemon died right after spawn (e.g. invalid args)
        }
        try {
            rpcCall(port_, secret_, "aria2.getVersion", nlohmann::json::array());
            daemonSpawned_ = true;
            // 首次拉起 daemon 后，重建上次会话（--save-session）里的未完成任务。
            recoverSession();
            // 启动 WebSocket 事件监听（仅收推送，请求仍走 HTTP）。连接失败不影响
            // 功能——refreshStates 轮询兜底（状态迁移延迟 ≤1s）。
            ws_ = std::make_unique<WsNotifier>(port_, [this](const std::string& method,
                                                             const std::string& gid) {
                handleWsEvent(method, gid);
            });
            ws_->start();
            return true;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    if (processHandle_) {
        terminateProcess(processHandle_);
        processHandle_ = nullptr;
    }
    port_ = 0;
    secret_.clear();
    lastError_ = tr("下载引擎启动超时（本地 RPC 未就绪）",
                    "Engine startup timed out (local RPC not ready)");
    return false;
}

std::uint64_t Aria2Engine::start(const std::string& url, const std::filesystem::path& destPath,
                                 const StartOptions& options) {
    if (!ensureDaemon()) return 0;

    auto task = std::make_shared<Task>();
    task->id = nextId_++;
    task->url = url;
    task->opts = options;

    const cfg::Aria2Config a2 = cfg::aria2Config();
    // 每任务连接数覆盖：>0 时同时覆盖 split 和 max-connection-per-server。
    const int connections = options.connections > 0
        ? std::clamp(options.connections, 1, 64)
        : a2.split;

    // 目录：dirOverride 优先（相对路径按配置下载目录解析），否则用 destPath 的父目录。
    std::filesystem::path dir = options.dirOverride.empty()
        ? destPath.parent_path()
        : options.dirOverride;
    if (dir.is_relative()) dir = cfg::downloadDir() / dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    nlohmann::json optionsJ = nlohmann::json::object();
    optionsJ["dir"] = dir.string();
    optionsJ["split"] = std::to_string(connections);
    optionsJ["max-connection-per-server"] = std::to_string(connections);
    optionsJ["min-split-size"] = a2.minSplitSize;
    // continue 只对 HTTP(S)/FTP 生效。仅当目标名已有 .aria2 控制文件（真正的部分
    // 文件待续传）才开：否则一旦目录里已有同名完整文件，continue=true 会让 aria2
    // 直接判定"已下载完"（不下载也不触发 --auto-file-renaming）——文件冲突应交给
    // auto-file-renaming 自动改名重新下载（实测：RealName.txt → RealName.1.txt）。
    // 续传的正式入口是 retry()（那里固定 continue=true 复用原路径），start() 只对
    // 能按 URL 文件名找到控制文件的情况自动续传。
    std::filesystem::path control = dir / destPath.filename();
    control += ".aria2";
    const bool hasControl = std::filesystem::exists(control, ec);
    optionsJ["continue"] = hasControl ? "true" : "false";

    // 每任务限速已移除（无意义），统一用配置的 maxDownloadLimit。
    if (a2.maxDownloadLimit > 0) {
        optionsJ["max-download-limit"] = std::to_string(a2.maxDownloadLimit);
    }

    // 磁力/BT：内容名由种子决定，只设 dir 不设 out；destPath 用占位，等
    // refreshStates() 从 files[0].path 更新为真实路径。
    // 普通 HTTP(S) 且未显式重命名：同样不设 out，让 aria2 从响应头
    // Content-Disposition 解析真实文件名——强制 out 会用 URL 末尾段命名，CDN/网盘
    // 链接末尾是 uuid/随机串时文件就被命名成 uuid（实测：带 out 用 url 名、不带
    // out 用 Content-Disposition 名）。重名由 daemon 的 --auto-file-renaming=true
    // 处理（RealName.txt → RealName.1.txt）。destPath 先用 URL 文件名占位，等
    // headers 后 applyTellStatus 从 files[0].path 更新为真实路径。
    // 仅当用户显式重命名时才强制 out，并走 makeUniqueDest 避免撞上已存在文件
    // （aria2 有 --allow-overwrite=false，撞上会直接失败）——自动加 " (1)"。
    nlohmann::json params;
    std::string method = "aria2.addUri";

    if (!options.torrentPath.empty()) {
        // 本地 .torrent 文件：读文件 → base64 → aria2.addTorrent。内容名由种子决定，
        // 只设 dir 不设 out；destPath 用占位，等 refreshStates() 更新为真实路径。
        task->destPath = dir / ("torrent-" + std::to_string(task->id));
        std::ifstream in(options.torrentPath, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            task->state = State::Failed;
            task->error = tr("种子文件读取失败或为空：", "Torrent file read failed or empty: ") +
                          options.torrentPath.string();
            tasks_.push_back(task);
            return task->id;
        }
        params = nlohmann::json::array();
        params.push_back(macaron::Base64::Encode(bytes));
        params.push_back(nlohmann::json::array());  // urls：附加源/tracker，留空
        params.push_back(optionsJ);
        method = "aria2.addTorrent";
    } else {
        const bool magnet = url.starts_with("magnet:");
        if (magnet) {
            task->destPath = dir / ("magnet-" + std::to_string(task->id));
        } else if (!options.outputName.empty()) {
            const std::filesystem::path uniqueDest =
                makeUniqueDest(dir / options.outputName);
            optionsJ["out"] = uniqueDest.filename().string();
            task->destPath = uniqueDest;
        } else {
            task->destPath = dir / destPath.filename();
        }
        // 镜像多源：首 URL + mirrors 一起作为 addUri 的 URIs 数组（aria2 多源并发、
        // 源挂自动切换）。有镜像时不设 out（除非显式重命名），文件名以首个响应的
        // 源为准，applyTellStatus 从 files[0].path 更新。
        nlohmann::json uriArray = nlohmann::json::array();
        uriArray.push_back(url);
        for (const auto& m : options.mirrors) uriArray.push_back(m);
        params = nlohmann::json::array();
        params.push_back(std::move(uriArray));
        params.push_back(optionsJ);
    }

    try {
        const nlohmann::json result = rpcCall(port_, secret_, method, params);
        task->gid = result.get<std::string>();
    } catch (const std::exception& e) {
        // task 尚未进入 tasks_，WS 线程看不到它，只需在 push 时持锁。
        std::lock_guard<std::mutex> lock(tasksMutex_);
        task->state = State::Failed;
        task->error = e.what();
        tasks_.push_back(task);
        return task->id;
    }

    std::lock_guard<std::mutex> lock(tasksMutex_);
    tasks_.push_back(task);
    return task->id;
}

std::shared_ptr<Aria2Engine::Task> Aria2Engine::findTask(std::uint64_t id) const {
    for (const auto& task : tasks_) {
        if (task->id == id) return task;
    }
    return nullptr;
}

void Aria2Engine::cancel(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto task = findTask(id);
    if (!task || !daemonSpawned_) return;
    try {
        rpcCall(port_, secret_, "aria2.remove", nlohmann::json::array({task->gid}));
    } catch (...) {
        try {
            rpcCall(port_, secret_, "aria2.forceRemove", nlohmann::json::array({task->gid}));
        } catch (...) {}
    }
    task->state = State::Cancelled;
    task->speedBps = 0.0;
}

void Aria2Engine::remove(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto task = findTask(id);
    if (!task) return;
    const std::string gid = task->gid;
    if (daemonSpawned_ && !gid.empty()) {
        // 活动/等待/暂停任务：forceRemove 停掉并移出队列（已完成等已停止任务会失败，
        // 由下面的 removeDownloadResult 兜底清除）。
        try {
            rpcCall(port_, secret_, "aria2.forceRemove", nlohmann::json::array({gid}));
        } catch (...) {}
        // 清掉 daemon stopped 列表里残留的 download result（完成/失败/removed），
        // 否则会留在结果里被再次枚举到。
        try {
            rpcCall(port_, secret_, "aria2.removeDownloadResult",
                    nlohmann::json::array({gid}));
        } catch (...) {}
        // 立即重写会话文件：已删任务不再出现在 --input-file，下次启动不会复活。
        // （只删面板不动会话是"随便下载一个就拉起历史任务"的根因。）
        try {
            rpcCall(port_, secret_, "aria2.saveSession", nlohmann::json::array());
        } catch (...) {}
    }
    std::erase_if(tasks_, [&](const std::shared_ptr<Task>& t) { return t->id == id; });
}

void Aria2Engine::pause(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto task = findTask(id);
    if (!task || !daemonSpawned_) return;
    if (task->state != State::Queued && task->state != State::Downloading) return;
    try {
        rpcCall(port_, secret_, "aria2.pause", nlohmann::json::array({task->gid}));
        task->state = State::Paused;
        task->speedBps = 0.0;
    } catch (...) {}
}

void Aria2Engine::resume(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto task = findTask(id);
    if (!task || !daemonSpawned_) return;
    if (task->state != State::Paused) return;
    try {
        rpcCall(port_, secret_, "aria2.unpause", nlohmann::json::array({task->gid}));
        task->state = State::Downloading;
    } catch (...) {}
}

void Aria2Engine::pauseAll() {
    if (!daemonSpawned_) return;
    std::lock_guard<std::mutex> lock(tasksMutex_);
    // 全部挂起：aria2.pauseAll 会把所有活动/等待任务置为 paused；本地状态同步
    // 更新。已有 paused 的调用 pauseAll 后仍为 paused，本地判断不变。
    try {
        rpcCall(port_, secret_, "aria2.pauseAll", nlohmann::json::array());
        for (const auto& task : tasks_) {
            if (task->state == State::Queued || task->state == State::Downloading) {
                task->state = State::Paused;
                task->speedBps = 0.0;
            }
        }
    } catch (...) {}
}

void Aria2Engine::resumeAll() {
    if (!daemonSpawned_) return;
    std::lock_guard<std::mutex> lock(tasksMutex_);
    try {
        rpcCall(port_, secret_, "aria2.unpauseAll", nlohmann::json::array());
        for (const auto& task : tasks_) {
            if (task->state == State::Paused) {
                task->state = State::Downloading;
            }
        }
    } catch (...) {}
}

void Aria2Engine::retry(std::uint64_t id) {
    // 状态检查在锁内（WS 线程可能改状态）。ensureDaemon() 内部会调 recoverSession()
    // （那里取锁），所以必须在本方法取锁之前调用，避免非递归互斥量死锁。
    bool allowed = false;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        const auto task = findTask(id);
        allowed = task &&
                  (task->state == State::Failed || task->state == State::Cancelled);
    }
    if (!allowed) return;

    if (!ensureDaemon()) {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        const auto task = findTask(id);
        if (task) {
            task->state = State::Failed;
            task->error = lastError_.empty() ? tr("引擎不可用", "Engine unavailable") : lastError_;
        }
        return;
    }

    std::lock_guard<std::mutex> lock(tasksMutex_);
    const auto task = findTask(id);
    if (!task) return;

    // 复用原任务的 URL 与 destPath 重新 addUri；continue=true 时 aria2 会从
    // 同目录的 .aria2 控制文件续传（真正的断点续传）。
    const cfg::Aria2Config a2 = cfg::aria2Config();
    const int connections = task->opts.connections > 0
        ? std::clamp(task->opts.connections, 1, 64)
        : a2.split;
    nlohmann::json optionsJ = nlohmann::json::object();
    optionsJ["dir"] = task->destPath.parent_path().string();
    optionsJ["split"] = std::to_string(connections);
    optionsJ["max-connection-per-server"] = std::to_string(connections);
    optionsJ["min-split-size"] = a2.minSplitSize;
    // continue 仅当确有 .aria2 控制文件（真正的部分文件待续传）时开启：失败/取消
    // 任务续传用；已完成任务没有控制文件 → 不 continue，重新下载走 daemon 的
    // --auto-file-renaming 改名（避免 aria2 把已存在完整文件判成"已下载完"直接完成，
    // 那样点重新下载等于没反应）。
    std::error_code ec;
    std::filesystem::path control = task->destPath;
    control += ".aria2";
    const bool hasControl = std::filesystem::exists(control, ec);
    optionsJ["continue"] = hasControl ? "true" : "false";
    // 每任务限速已移除（无意义），统一用配置的 maxDownloadLimit。
    if (a2.maxDownloadLimit > 0) {
        optionsJ["max-download-limit"] = std::to_string(a2.maxDownloadLimit);
    }
    // 磁力/BT 不设 out（内容名由种子决定）；HTTP 仅当显式重命名时才强制 out
    // （否则重新走 Content-Disposition 解析，避免续传回来仍是 uuid 名）。
    nlohmann::json params;
    std::string method = "aria2.addUri";

    if (!task->opts.torrentPath.empty()) {
        // 本地 .torrent 重下：同 start()，读文件 → addTorrent。destPath 是
        // refreshStates 更新后的真实路径，dir 取它父目录即可。
        std::ifstream in(task->opts.torrentPath, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            task->state = State::Failed;
            task->error = tr("种子文件读取失败或为空：", "Torrent file read failed or empty: ") +
                          task->opts.torrentPath.string();
            return;
        }
        params = nlohmann::json::array();
        params.push_back(macaron::Base64::Encode(bytes));
        params.push_back(nlohmann::json::array());  // urls：附加源/tracker，留空
        params.push_back(optionsJ);
        method = "aria2.addTorrent";
    } else {
        if (!task->url.starts_with("magnet:") && !task->opts.outputName.empty()) {
            optionsJ["out"] = task->destPath.filename().string();
        }
        // 重下同样带上镜像源（opts.mirrors 从原任务复用）。
        nlohmann::json uriArray = nlohmann::json::array();
        uriArray.push_back(task->url);
        for (const auto& m : task->opts.mirrors) uriArray.push_back(m);
        params = nlohmann::json::array();
        params.push_back(std::move(uriArray));
        params.push_back(optionsJ);
    }
    try {
        const nlohmann::json result = rpcCall(port_, secret_, method, params);
        task->gid = result.get<std::string>();
        task->state = State::Queued;
        task->error.clear();
        task->speedBps = 0.0;
    } catch (const std::exception& e) {
        task->state = State::Failed;
        task->error = e.what();
    }
}

// 镜像源管理：aria2.changeUri 只对活动任务（active/waiting/paused）有效。注意
// aria2-next 的 changeUri 签名与标准 aria2 不同：params = [gid, fileIndex, delUris,
// addUris, pos?]（token 由 rpcCall 前置）。fileIndex 恒为 1（单文件任务）。
bool Aria2Engine::addMirror(std::uint64_t id, const std::string& url) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto task = findTask(id);
    if (!task || !daemonSpawned_ || task->gid.empty()) return false;
    if (task->state != State::Queued && task->state != State::Downloading &&
        task->state != State::Paused) {
        return false;
    }
    try {
        const nlohmann::json res = rpcCall(
            port_, secret_, "aria2.changeUri",
            nlohmann::json::array({task->gid, 1, nlohmann::json::array(),
                                   nlohmann::json::array({url})}));
        // 返回 [deletedCount, addedCount]。
        if (res.is_array() && res.size() == 2 && res[1].get<int>() > 0) {
            task->opts.mirrors.push_back(url);  // 记录到配置镜像（retry 复用）
            return true;
        }
    } catch (...) {}
    return false;
}

bool Aria2Engine::removeMirror(std::uint64_t id, const std::string& url) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto task = findTask(id);
    if (!task || !daemonSpawned_ || task->gid.empty()) return false;
    if (task->state != State::Queued && task->state != State::Downloading &&
        task->state != State::Paused) {
        return false;
    }
    try {
        const nlohmann::json res = rpcCall(
            port_, secret_, "aria2.changeUri",
            nlohmann::json::array({task->gid, 1, nlohmann::json::array({url}),
                                   nlohmann::json::array()}));
        if (res.is_array() && res.size() == 2 && res[0].get<int>() > 0) {
            std::erase(task->opts.mirrors, url);  // 同步配置镜像
            return true;
        }
    } catch (...) {}
    return false;
}

// 重启后重建任务表：daemon 用 --input-file 载入了 --save-session 的未完成任务，
// 这里通过 tellActive/tellWaiting/tellStopped 把它们同步成本地 Task。
void Aria2Engine::recoverSession() const {
    if (!daemonSpawned_) return;
    // 本方法只从 ensureDaemon() 调用（那里未持锁），所以在开头取锁。
    std::lock_guard<std::mutex> lock(tasksMutex_);
    std::vector<std::shared_ptr<Task>> recovered;

    const auto strI64 = [](const nlohmann::json& st, const char* key) -> std::int64_t {
        const std::string v = st.value(key, "0");
        try { return std::stoll(v); } catch (...) { return 0; }
    };
    const auto strDbl = [](const nlohmann::json& st, const char* key) -> double {
        try { return std::stod(st.value(key, "0")); } catch (...) { return 0.0; }
    };
    const auto strConn = [](const nlohmann::json& st) -> int {
        const std::string v = st.value("connections", st.value("numConnections", "1"));
        try { return std::max(1, std::stoi(v)); } catch (...) { return 1; }
    };

    const auto addFrom = [&](const nlohmann::json& st) {
        if (!st.is_object()) return;
        auto task = std::make_shared<Task>();
        task->id = nextId_++;
        task->gid = st.value("gid", "");
        if (st.contains("files") && st["files"].is_array() && !st["files"].empty()) {
            const auto& file = st["files"][0];
            task->destPath = std::filesystem::path(file.value("path", ""));
            if (file.contains("uris") && file["uris"].is_array() && !file["uris"].empty()) {
                task->url = file["uris"][0].value("uri", "");
                // 镜像源随会话恢复：files[0].uris 是 aria2 去重后的全部源（实测顺序
                // 不稳定，首条不一定是原主 URL），除主 URL 外的唯一源还给
                // opts.mirrors——重下（retry）复用 opts 时多源不丢，卡片也能显示镜像数。
                for (std::size_t i = 1; i < file["uris"].size(); ++i) {
                    const std::string u = file["uris"][i].value("uri", "");
                    if (!u.empty() && u != task->url &&
                        std::ranges::find(task->opts.mirrors, u) ==
                            task->opts.mirrors.end()) {
                        task->opts.mirrors.push_back(u);
                    }
                }
            }
        }
        // 磁力任务没有可用 uri 时用 infoHash 拼回显。
        if (task->url.empty() && st.contains("bittorrent") && st["bittorrent"].is_object()) {
            const std::string ih = st["bittorrent"].value("infoHash", "");
            if (!ih.empty()) task->url = "magnet:?xt=urn:btih:" + ih;
        }
        // BT/磁力：拿到元数据后记下种子真实名，供显示（避免展示 GID 占位名）。
        if (st.contains("bittorrent") && st["bittorrent"].is_object() &&
            st["bittorrent"].contains("info") && st["bittorrent"]["info"].is_object()) {
            task->displayName = st["bittorrent"]["info"].value("name", "");
        }
        task->totalBytes = strI64(st, "totalLength");
        task->downloadedBytes = strI64(st, "completedLength");
        task->speedBps = strDbl(st, "downloadSpeed");
        task->connections = strConn(st);
        const std::string status = st.value("status", "");
        if (status == "active") {
            task->state = State::Downloading;
        } else if (status == "waiting") {
            task->state = State::Queued;
        } else if (status == "paused") {
            task->state = State::Paused;
        } else if (status == "complete") {
            task->state = State::Done;
        } else if (status == "error") {
            task->state = State::Failed;
            task->error = st.value("errorMessage", "download error");
        } else {
            task->state = State::Cancelled;
        }
        recovered.push_back(std::move(task));
    };

    try {
        for (const auto& st : rpcCall(port_, secret_, "aria2.tellActive",
                                      nlohmann::json::array())) {
            addFrom(st);
        }
        for (const auto& st : rpcCall(port_, secret_, "aria2.tellWaiting",
                                      nlohmann::json::array({0, 1000}))) {
            addFrom(st);
        }
        for (const auto& st : rpcCall(port_, secret_, "aria2.tellStopped",
                                      nlohmann::json::array({0, 1000}))) {
            if (!st.is_object()) continue;
            const std::string status = st.value("status", "");
            if (status == "error" || status == "removed") {
                // 失败/已移除的任务不再重挂，从 daemon 清掉避免下次会话又载入。
                try {
                    rpcCall(port_, secret_, "aria2.remove",
                            nlohmann::json::array({st.value("gid", "")}));
                } catch (...) {}
            }
            // complete 的任务不重建（已完成无需展示/续传）。
        }
    } catch (...) {
        // 会话枚举失败不致命：本地任务仍可用，下次再试。
    }

    if (!recovered.empty()) {
        tasks_.insert(tasks_.end(), recovered.begin(), recovered.end());
    }
}

// 用一条 tellStatus 的 JSON 刷新任务字段（含 status→State 映射）。调用方须持锁。
// 也被 needsFinalize 路径复用：WS 事件把完成/失败任务置位后，这里补一次 tellStatus
// 拿到最终字节、磁力真实路径和 errorMessage。
void Aria2Engine::applyTellStatus(const std::shared_ptr<Task>& task,
                                  const nlohmann::json& st) const {
    const std::string status = st.value("status", "active");

    auto strI64 = [&](const char* key) -> std::int64_t {
        const std::string v = st.value(key, "0");
        try { return std::stoll(v); } catch (...) { return 0; }
    };
    task->totalBytes = strI64("totalLength");
    task->downloadedBytes = strI64("completedLength");
    try { task->speedBps = std::stod(st.value("downloadSpeed", "0")); }
    catch (...) { task->speedBps = 0.0; }
    // 磁力/BT 任务拿到元数据后，files[0].path 才是真实下载路径（更新占位）。
    if (st.contains("files") && st["files"].is_array() &&
        !st["files"].empty()) {
        const std::string p = st["files"][0].value("path", "");
        if (!p.empty()) task->destPath = std::filesystem::path(p);
        // 实时源列表：files[0].uris 按 URL 去重（aria2 对同一 URL 分片会生成多个
        // URIResult），状态合并取最优（used > waiting > error）。
        task->mirrors.clear();
        if (st["files"][0].contains("uris") && st["files"][0]["uris"].is_array()) {
            for (const auto& u : st["files"][0]["uris"]) {
                if (!u.is_object()) continue;
                const std::string uri = u.value("uri", "");
                const std::string status = u.value("status", "waiting");
                if (uri.empty()) continue;
                auto it = std::find_if(task->mirrors.begin(), task->mirrors.end(),
                                       [&](const dl::MirrorSource& m) {
                                           return m.uri == uri;
                                       });
                const int prio = [](const std::string& s) {
                    return s == "used" ? 2 : (s == "waiting" ? 1 : 0);
                }(status);
                if (it == task->mirrors.end()) {
                    task->mirrors.push_back(dl::MirrorSource{uri, status});
                } else if (prio > [&](const std::string& s) {
                    return s == "used" ? 2 : (s == "waiting" ? 1 : 0);
                }(it->status)) {
                    it->status = status;
                }
            }
        }
    }
    // BT/磁力：记录种子真实名（bittorrent.info.name），供卡片/弹窗显示，
    // 避免展示 GID/磁力占位名。
    if (st.contains("bittorrent") && st["bittorrent"].is_object() &&
        st["bittorrent"].contains("info") && st["bittorrent"]["info"].is_object()) {
        task->displayName = st["bittorrent"]["info"].value("name", "");
    }
    // aria2-next reports the field as "connections"; original aria2 uses
    // "numConnections". Accept both so the engine works with either.
    const std::string connField =
        st.value("connections", st.value("numConnections", "1"));
    try { task->connections = std::max(1, std::stoi(connField)); }
    catch (...) { task->connections = 1; }

    if (status == "complete") {
        task->state = State::Done;
        task->speedBps = 0.0;
    } else if (status == "error") {
        task->state = State::Failed;
        task->error = st.value("errorMessage", "download error");
        task->speedBps = 0.0;
    } else if (status == "removed") {
        task->state = State::Cancelled;
        task->speedBps = 0.0;
    } else if (status == "waiting") {
        task->state = State::Queued;
    } else if (status == "paused") {
        task->state = State::Paused;
        task->speedBps = 0.0;
    } else {  // "active"
        task->state = State::Downloading;
    }
}

// 轮询活动任务进度 + 补全 needsFinalize 任务。调用方（snapshot）须已持锁。
// 状态迁移主要由 WS 事件接管（即时），这里的 tellStatus 负责进度字节/速度/连接数，
// 以及事件只带 gid 时的最终态补全。
void Aria2Engine::refreshStates() const {
    if (!daemonSpawned_) return;
    // 收集需要轮询/补全的任务（活动状态 + needsFinalize），RPC 期间锁保持（WS
    // 事件回调只是短暂等待，无死锁）。
    std::vector<std::pair<std::shared_ptr<Task>, std::string>> targets;
    targets.reserve(tasks_.size());
    for (const auto& task : tasks_) {
        const State s = task->state;
        if (s == State::Queued || s == State::Downloading || s == State::Paused ||
            task->needsFinalize) {
            targets.emplace_back(task, task->gid);
        }
    }
    for (const auto& [task, gid] : targets) {
        try {
            const nlohmann::json st =
                rpcCall(port_, secret_, "aria2.tellStatus", nlohmann::json::array({gid}));
            applyTellStatus(task, st);
            task->needsFinalize = false;
        } catch (...) {
            // Transient RPC failure — leave state as-is, retry next poll.
        }
    }
}

std::vector<TaskView> Aria2Engine::snapshot() const {
    // 状态迁移由 WS 事件接管（即时），进度字节/速度只需 ~1s 刷新（对齐 Motrix/AriaNg）。
    std::lock_guard<std::mutex> lock(tasksMutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPoll_ >= std::chrono::seconds(1)) {
        lastPoll_ = now;
        refreshStates();
    }
    std::vector<TaskView> out;
    out.reserve(tasks_.size());
    for (auto it = tasks_.rbegin(); it != tasks_.rend(); ++it) {
        const Task& task = **it;
        out.push_back(TaskView{task.id, task.url, task.destPath, task.state,
                               task.totalBytes, task.downloadedBytes,
                               task.error, task.speedBps, task.connections,
                               task.displayName,
                               static_cast<int>(task.opts.mirrors.size()),
                               task.mirrors});
    }
    return out;
}

bool Aria2Engine::busy() const {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    for (const auto& task : tasks_) {
        const State s = task->state;
        if (s == State::Queued || s == State::Downloading || s == State::Paused) {
            return true;
        }
    }
    return false;
}

// WebSocket 推送事件（IXWebSocket 后台线程）→ 任务状态即时迁移。事件只带 gid，
// 因此除状态外还需要 needsFinalize 让下一轮 poll 补一次 tellStatus（最终字节 /
// 磁力真实路径 / errorMessage）。
void Aria2Engine::handleWsEvent(const std::string& method, const std::string& gid) const {
    if (gid.empty()) return;
    bool handled = false;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        for (const auto& task : tasks_) {
            if (task->gid != gid) continue;
            if (method == "aria2.onDownloadStart") {
                task->state = State::Downloading;
            } else if (method == "aria2.onDownloadPause") {
                task->state = State::Paused;
                task->speedBps = 0.0;
            } else if (method == "aria2.onDownloadComplete" ||
                       method == "aria2.onBtDownloadComplete") {
                task->state = State::Done;
                task->speedBps = 0.0;
                task->needsFinalize = true;
            } else if (method == "aria2.onDownloadError") {
                task->state = State::Failed;
                task->speedBps = 0.0;
                task->needsFinalize = true;
            } else if (method == "aria2.onDownloadStop") {
                // 停下的真实状态（complete/error/removed/paused）交给下次 tellStatus 定夺。
                task->needsFinalize = true;
            }
            handled = true;
            break;
        }
    }
    // 状态迁移后唤醒 UI 一帧：不依赖 onFrame 的每帧重绘，任务完成/失败即时可见。
    if (handled) {
        core::platform::requestUiUpdate();
    }
}

void Aria2Engine::shutdown() {
    // 与可能仍在跑的 warmup 后台线程互斥（它正在 ensureDaemon 里等 RPC 就绪）。
    std::lock_guard<std::mutex> daemonLock(daemonMutex_);
    if (daemonSpawned_) {
        // 先持久化未完成任务（--save-session）；forceShutdown 会跳过会话保存。
        try {
            rpcCall(port_, secret_, "aria2.saveSession", nlohmann::json::array());
        } catch (...) {}
        try {
            rpcCall(port_, secret_, "aria2.forceShutdown", nlohmann::json::array());
        } catch (...) {}
        if (processHandle_) {
            terminateProcess(processHandle_);
            processHandle_ = nullptr;
        }
        daemonSpawned_ = false;
        port_ = 0;
        secret_.clear();
    }
    // 先停掉 WS 监听（stop 会 join IXWebSocket 的线程，此后不再有回调），再清任务，
    // 保证没有任何回调访问已清空的任务。顺序不可颠倒。
    if (ws_) {
        ws_->stop();
        ws_.reset();
    }
    std::lock_guard<std::mutex> lock(tasksMutex_);
    tasks_.clear();
}

std::filesystem::path Aria2Engine::makeUniqueDest(const std::filesystem::path& dest) const {
    std::vector<std::filesystem::path> reserved;
    reserved.reserve(tasks_.size());
    for (const auto& task : tasks_) {
        reserved.push_back(task->destPath);
    }

    auto taken = [&](const std::filesystem::path& candidate) {
        return std::filesystem::exists(candidate) ||
               std::ranges::count(reserved, candidate) > 0;
    };
    if (!taken(dest)) return dest;

    const std::filesystem::path parent = dest.parent_path();
    const std::string stem = dest.stem().string();
    const std::string ext = dest.extension().string();
    for (int i = 1;; ++i) {
        const auto candidate = parent / (stem + " (" + std::to_string(i) + ")" + ext);
        if (!taken(candidate)) return candidate;
    }
}

} // namespace dl
