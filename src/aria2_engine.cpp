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
#include "native_resource.hpp"
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
#include "native_resource.hpp"
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
import tinynext.utils;  // pathFromUtf8 / utf8FromPath（aria2 JSON 字符串是 UTF-8）

namespace dl {

struct Aria2Engine::NetworkSession {
    NetworkSession() : initialized(ix::initNetSystem()) {}
    ~NetworkSession() {
        if (initialized) ix::uninitNetSystem();
    }

    bool initialized = false;
};

struct Aria2Engine::Process {
#ifdef _WIN32
    void adopt(HANDLE value) noexcept { handle.reset(value); }

    bool exited() const noexcept {
        return !handle || WaitForSingleObject(handle.get(), 0) == WAIT_OBJECT_0;
    }

    bool waitForExit(int timeoutMs) const noexcept {
        return !handle ||
               WaitForSingleObject(handle.get(), static_cast<DWORD>(std::max(0, timeoutMs))) ==
                   WAIT_OBJECT_0;
    }

    void terminate() noexcept {
        if (!handle) return;
        if (WaitForSingleObject(handle.get(), 0) != WAIT_OBJECT_0) {
            TerminateProcess(handle.get(), 0);
            WaitForSingleObject(handle.get(), INFINITE);
        }
        handle.reset();
    }

    tinynext::native::UniqueHandle handle;
#else
    void adopt(pid_t value) noexcept {
        pid = value;
        reaped = false;
    }

    bool exited() noexcept {
        if (pid <= 0 || reaped) return true;
        int status = 0;
        pid_t result;
        do {
            result = ::waitpid(pid, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == pid || (result < 0 && errno == ECHILD)) {
            reaped = true;
            pid = -1;
            return true;
        }
        return false;
    }

    bool waitForExit(int timeoutMs) noexcept {
        if (exited()) return true;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
        while (std::chrono::steady_clock::now() < deadline) {
            if (exited()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return exited();
    }

    void terminate() noexcept {
        if (pid <= 0 || reaped || exited()) return;
        ::kill(pid, SIGTERM);
        for (int i = 0; i < 50; ++i) {
            if (exited()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!exited()) ::kill(pid, SIGKILL);
        int status = 0;
        pid_t result;
        do {
            result = ::waitpid(pid, &status, 0);
        } while (result < 0 && errno == EINTR);
        reaped = true;
        pid = -1;
    }

    pid_t pid = -1;
    bool reaped = true;
#endif

    ~Process() { terminate(); }
};

namespace {

// ---- 极简跨平台 TCP socket（替代 tinyhttps::Socket，仅用于本地 JSON-RPC）----
#ifdef _WIN32
using LocalFd = SOCKET;
constexpr LocalFd kInvalidFd = INVALID_SOCKET;

class WinsockSession {
public:
    WinsockSession() : active_(WSAStartup(MAKEWORD(2, 2), &data_) == 0) {}
    ~WinsockSession() { if (active_) WSACleanup(); }
    bool active() const noexcept { return active_; }

private:
    WSADATA data_{};
    bool active_ = false;
};

WinsockSession& winsockSession() {
    static WinsockSession session;
    return session;
}
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
#ifndef _WIN32
        const int fdFlags = ::fcntl(fd_, F_GETFD);
        if (fdFlags < 0 || ::fcntl(fd_, F_SETFD, fdFlags | FD_CLOEXEC) < 0) {
            close();
            return false;
        }
#endif

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
        return winsockSession().active();
#else
        return true;
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
    // 高段范围随机探测可绑定端口（避开常见服务区）。用本地 bind 试探而非 connect：
    // 零网络包、不触碰其他本地服务、无防火墙超时阻塞，且能排除 TIME_WAIT/仅绑定未监听口。
    std::uniform_int_distribution<int> dist(20000, 60000);
    for (int attempt = 0; attempt < 32; ++attempt) {
        const int candidate = dist(rng);
        struct ScopedSock {
            LocalFd fd = kInvalidFd;
            explicit ScopedSock(LocalFd v) noexcept : fd(v) {}
            ~ScopedSock() {
                if (fd != kInvalidFd) {
#ifdef _WIN32
                    ::closesocket(fd);
#else
                    ::close(fd);
#endif
                }
            }
            ScopedSock(const ScopedSock&) = delete;
            ScopedSock& operator=(const ScopedSock&) = delete;
        } probe(::socket(AF_INET, SOCK_STREAM, 0));
        if (probe.fd == kInvalidFd) continue;
#ifdef _WIN32
        const BOOL exclusive = TRUE;
        ::setsockopt(probe.fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
        const int fdFlags = ::fcntl(probe.fd, F_GETFD);
        if (fdFlags >= 0) ::fcntl(probe.fd, F_SETFD, fdFlags | FD_CLOEXEC);
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(candidate));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::bind(probe.fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
            return candidate;
        }
    }
    return dist(rng);
}

// daemon 级参数（除分片/连接数外的全部 --xxx 选项）：仅非空/非默认才传，
// 避免多余 flag。Persistent 模式启用会话恢复（--save-session / --input-file）；
// Isolated 模式（--headless）使用独立临时 state-dir，不读写主会话文件、不占用固定监听端口。
std::vector<std::pair<std::string, std::string>> daemonExtraOpts(
    const cfg::Aria2Config& a2, SessionMode mode,
    const std::filesystem::path& isolatedStateDir) {
    const bool persistent = mode == SessionMode::Persistent;
    std::vector<std::pair<std::string, std::string>> opts;
    auto add = [&](const char* name, const std::string& value) {
        opts.emplace_back(name, value);
    };
    if (!a2.proxy.empty()) add("all-proxy", a2.proxy);
    if (!a2.noProxy.empty()) add("no-proxy", a2.noProxy);
    if (a2.maxTries > 0) add("max-tries", std::to_string(a2.maxTries));
    if (a2.retryWait > 0) add("retry-wait", std::to_string(a2.retryWait));
    add("max-concurrent-downloads", std::to_string(a2.maxConcurrentDownloads));
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
    if (persistent && !a2.listenPort.empty()) add("listen-port", a2.listenPort);
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
    if (persistent && !a2.saveCookies.empty()) add("save-cookies", a2.saveCookies);
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
    if (persistent && !a2.ed2kListenPort.empty()) add("ed2k-listen-port", a2.ed2kListenPort);
    if (persistent && !a2.ed2kUdpListenPort.empty()) add("ed2k-udp-listen-port", a2.ed2kUdpListenPort);
    if (a2.ed2kUploadSlots > 0) {
        add("ed2k-upload-slots", std::to_string(a2.ed2kUploadSlots));
    }
    if (persistent) {
        // aria2-next 的 HTTP/BT/ED2K/media 恢复数据必须落在稳定的 state-dir；
        // 绝不能放临时/cache 目录。下载目录不再生成旁置 .aria2 控制文件。
        const std::filesystem::path stateDir = cfg::configDir() / "aria2-state";
        std::error_code stateEc;
        std::filesystem::create_directories(stateDir, stateEc);
        add("state-dir", utf8FromPath(stateDir));
        // 会话恢复：shutdown 前用 aria2.saveSession 持久化未完成任务，下次启动用
        // --input-file 载入续传。首次运行会话文件不存在，跳过 --input-file。
        // 会话文件放 per-user 配置目录：安装版经快捷方式启动时 cwd 可能是 System32
        // （不可写），不能依赖 cwd。
        const std::filesystem::path sessionPath = cfg::configDir() / "tinynext.session";
        std::error_code ec;
        std::filesystem::create_directories(sessionPath.parent_path(), ec);
        add("save-session", utf8FromPath(sessionPath));
        if (std::filesystem::exists(sessionPath, ec)) {
            add("input-file", utf8FromPath(sessionPath));
        }
    } else if (!isolatedStateDir.empty()) {
        std::error_code stateEc;
        std::filesystem::create_directories(isolatedStateDir, stateEc);
        add("state-dir", utf8FromPath(isolatedStateDir));
    }
    // 异常退出兜底（对齐 Motrix）：主进程崩溃/被强杀时 atexit 链不会执行，没人
    // terminate daemon。让 aria2 轮询主进程 PID，父进程消失即按正常流程自杀
    // （含保存会话）。优雅退出仍走 shutdown() 的 saveSession + forceShutdown，
    // 不依赖本选项。
#ifdef _WIN32
    add("stop-with-process", std::to_string(::GetCurrentProcessId()));
#else
    add("stop-with-process", std::to_string(::getpid()));
#endif
    // 磁力/BT：保存元数据为 .torrent + 显式开启 DHT / PEX。
    add("bt-save-metadata", "true");
    add("enable-dht", "true");
    add("enable-peer-exchange", "true");
#ifndef _WIN32
    // CA bundle：aria2-next 内置 OpenSSL 的默认证书路径在部分发行版上不存在
    // （如实测本机 /usr/lib/ssl/certs 为空目录），导致所有 HTTPS 下载报
    // "SSL peer certificate or SSH remote key was not OK"。按常见发行版路径
    // 显式指定第一个存在的 bundle；都不存在则不加（保持默认行为）。
    static const char* kCaBundles[] = {
        "/etc/ssl/certs/ca-bundle.crt",          // openSUSE / 部分 Fedora
        "/etc/ssl/certs/ca-certificates.crt",    // Debian/Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",      // RHEL/Fedora
        "/etc/ssl/ca-bundle.pem",                // openSUSE 旧版
        "/etc/ssl/cert.pem",                     // macOS
    };
    for (const char* bundle : kCaBundles) {
        std::error_code caEc;
        if (std::filesystem::exists(bundle, caEc)) {
            add("ca-certificate", bundle);
            break;
        }
    }
#endif
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
#ifndef _WIN32
    // 与 component_updater 保持一致：POSIX 回退常见系统安装目录。
    for (const char* dir : {"/usr/bin", "/usr/local/bin", "/opt/homebrew/bin"}) {
        const std::filesystem::path candidate = std::filesystem::path(dir) / name;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate.string();
    }
#endif
    return {};
}

bool hasMediaTrackSelectionSupport(const nlohmann::json& versionInfo) {
    if (!versionInfo.is_object()) return false;
    const auto it = versionInfo.find("mediaFeatures");
    if (it == versionInfo.end() || !it->is_array()) return false;
    return std::ranges::any_of(*it, [](const nlohmann::json& feature) {
        return feature.is_string() && feature.get<std::string>() == "stable-track-ids";
    });
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
    bool fromSession = false;     // 从会话恢复的历史任务（不触发完成/失败通知）
    bool isMedia = false;
    std::string mediaProtocol;
    std::string mediaState;
    bool mediaLive = false;
    double mediaProgress = 0.0;
    std::int64_t mediaDurationMs = 0;
    std::int64_t mediaCompletedDurationMs = 0;
    std::int64_t mediaDownloadedBytes = 0;
    bool awaitingMediaTrackSelection = false;
    std::vector<MediaTrackView> mediaTracks;
    bool awaitingTorrentFileSelection = false;
    std::vector<TorrentFileView> torrentFiles;
};

struct Aria2Engine::RpcEndpoint {
    bool active = false;
    int port = 0;
    std::string secret;
};

Aria2Engine::Aria2Engine(SessionMode mode)
    : sessionMode_(mode), restoreFailedOnRecover_(cfg::autoRetryFailed()) {
    if (sessionMode_ == SessionMode::Isolated) {
#ifdef _WIN32
        const auto pid = static_cast<unsigned long long>(::GetCurrentProcessId());
#else
        const auto pid = static_cast<unsigned long long>(::getpid());
#endif
        isolatedStateDir_ = std::filesystem::temp_directory_path() /
                            ("tinynext-headless-" + std::to_string(pid) + "-" + randomHex(8));
    }
    // Winsock for the local RPC socket (Windows only; POSIX no-op).
    LocalSocket::platformInit();
    // The session pairs IXWebSocket's network initialization and cleanup; it is
    // released only after shutdown() has stopped the websocket worker.
    networkSession_ = std::make_unique<NetworkSession>();
}

Aria2Engine::~Aria2Engine() {
    shutdown();
    networkSession_.reset();
}

Aria2Engine::RpcEndpoint Aria2Engine::currentEndpoint() const {
    std::lock_guard<std::mutex> lock(daemonMutex_);
    if (!daemonSpawned_ || port_ == 0 || !process_ || process_->exited()) {
        return {};
    }
    return {true, port_, secret_};
}

bool Aria2Engine::engineActive() const {
    std::lock_guard<std::mutex> lock(daemonMutex_);
    return daemonSpawned_ && port_ != 0 && process_ && !process_->exited();
}

std::string Aria2Engine::lastError() const {
    std::lock_guard<std::mutex> lock(daemonMutex_);
    return lastError_;
}

void Aria2Engine::warmup(bool restoreFailed) {
    // 启动时后台拉起 daemon + 恢复上次会话历史任务（ensureDaemonLocked 首次 spawn
    // 时内部调 recoverSession + 启动 WS 监听）。全程持 daemonMutex_ 与 UI 线程互斥。
    std::lock_guard<std::mutex> lock(daemonMutex_);
    restoreFailedOnRecover_ = restoreFailed;
    ensureDaemonLocked();
}

void Aria2Engine::downloadFile(const std::string& url,
                               const std::filesystem::path& dest,
                               std::function<void(int)> onProgress,
                               std::function<void(bool, std::string)> onDone) {
    // 组件更新专用静默下载：不进任务表、不出卡片。worker 由引擎持有；shutdown
    // 会请求停止并 join，因此回调结束前 this 与 daemon 状态都保持有效。
    std::unique_lock workerLock(downloadThreadMutex_);
    if (downloadShutdown_) {
        workerLock.unlock();
        if (onDone) onDone(false, tr("err.engine_unavailable"));
        return;
    }
    if (downloadThread_.joinable()) {
        downloadThread_.request_stop();
        downloadThread_.join();
    }
    downloadThread_ = std::jthread(
        [this, url, dest, onProgress = std::move(onProgress),
         onDone = std::move(onDone)](std::stop_token stop) mutable {
        auto fail = [&onDone](const std::string& err) {
            if (onDone) onDone(false, err);
        };
        if (stop.stop_requested()) {
            fail(tr("err.engine_unavailable"));
            return;
        }
        if (!ensureDaemon()) {
            const std::string err = lastError();
            fail(err.empty() ? tr("err.engine_unavailable") : err);
            return;
        }
        const RpcEndpoint ep = currentEndpoint();
        if (!ep.active || stop.stop_requested()) {
            fail(tr("err.engine_unavailable"));
            return;
        }
        // 文件名必须精确（校验文件按资产名取 hash）：关自动改名、允许覆盖残留。
        nlohmann::json optionsJ = nlohmann::json::object();
        optionsJ["dir"] = utf8FromPath(dest.parent_path());
        optionsJ["out"] = utf8FromPath(dest.filename());
        optionsJ["allow-overwrite"] = "true";
        optionsJ["auto-file-renaming"] = "false";
        optionsJ["max-tries"] = "3";
        optionsJ["retry-wait"] = "1";
        optionsJ["connect-timeout"] = "15";
        std::string gid;
        try {
            gid = rpcCall(ep.port, ep.secret, "aria2.addUri",
                          nlohmann::json::array({nlohmann::json::array({url}),
                                                 optionsJ}))
                      .get<std::string>();
        } catch (const std::exception& e) {
            fail(e.what());
            return;
        }
        // 退出守卫：无论成功、报错还是被 shutdown()/新请求提前终止，都必须把内部
        // 静默任务从 daemon 中彻底移除，避免被随后的 saveSession 写进用户会话文件。
        struct SilentTaskCleanup {
            const Aria2Engine* self;
            const std::string* gid;
            ~SilentTaskCleanup() {
                if (!gid || gid->empty()) return;
                const RpcEndpoint cur = self->currentEndpoint();
                if (!cur.active) return;
                try {
                    rpcCall(cur.port, cur.secret, "aria2.forceRemove",
                            nlohmann::json::array({*gid}));
                } catch (...) {}
                try {
                    rpcCall(cur.port, cur.secret, "aria2.removeDownloadResult",
                            nlohmann::json::array({*gid}));
                } catch (...) {}
            }
        } cleanup{this, &gid};

        for (;;) {
            for (int tick = 0; tick < 10; ++tick) {
                if (stop.stop_requested()) {
                    fail(tr("err.engine_unavailable"));
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            if (stop.stop_requested()) {
                fail(tr("err.engine_unavailable"));
                return;
            }
            const RpcEndpoint pollEp = currentEndpoint();
            if (!pollEp.active) {
                fail(tr("err.engine_unavailable"));
                return;
            }
            nlohmann::json st;
            try {
                st = rpcCall(pollEp.port, pollEp.secret, "aria2.tellStatus",
                             nlohmann::json::array({gid}));
            } catch (const std::exception& e) {
                fail(e.what());
                return;
            }
            const auto strI64 = [&st](const char* key) -> std::int64_t {
                try { return std::stoll(st.value(key, "0")); } catch (...) { return 0; }
            };
            const std::int64_t total = strI64("totalLength");
            const std::int64_t done = strI64("completedLength");
            if (onProgress && total > 0) {
                onProgress(static_cast<int>(done * 100 / total));
            }
            const std::string status = st.value("status", "");
            if (status == "complete") {
                if (onProgress) onProgress(100);
                if (onDone) onDone(true, {});
                return;
            }
            if (status == "error" || status == "removed") {
                const std::string err = st.value("errorMessage", "download error");
                fail(err);
                return;
            }
        }
    });
    workerLock.unlock();
}

void Aria2Engine::stopDaemonLocked(bool saveSession) const {
    const bool running = daemonSpawned_ && port_ != 0 && process_ && !process_->exited();
    if (running) {
        if (saveSession && sessionMode_ == SessionMode::Persistent) {
            purgeFailedBeforeSessionSave(port_, secret_);
            try {
                rpcCall(port_, secret_, "aria2.saveSession", nlohmann::json::array());
            } catch (...) {}
        }
        bool shutdownRequested = false;
        try {
            rpcCall(port_, secret_, "aria2.forceShutdown", nlohmann::json::array());
            shutdownRequested = true;
        } catch (...) {}
        // 给 aria2-next 留优雅退出窗口刷写 state.db 检查点、disk-cache 与 cookies，
        // 避免 Windows 上 0ms 立即 TerminateProcess 截断落盘。
        if (shutdownRequested && process_) {
            process_->waitForExit(1500);
        }
    }
    if (process_) {
        process_->terminate();
        process_.reset();
    }
    daemonSpawned_ = false;
    port_ = 0;
    secret_.clear();
    // WS 回调只取 tasksMutex_（遵守 daemonMutex_ → tasksMutex_ 锁序），在
    // daemonMutex_ 内安全 stop + join。
    if (ws_) {
        ws_->stop();
        ws_.reset();
    }
}

bool Aria2Engine::ensureDaemon() const {
    std::lock_guard<std::mutex> daemonLock(daemonMutex_);
    return ensureDaemonLocked();
}

bool Aria2Engine::ensureDaemonLocked() const {
    if (daemonSpawned_) {
        if (port_ != 0 && process_ && !process_->exited()) {
            return true;
        }
        // 先前拉起的 daemon 意外退出（崩溃/被外部强杀）：清理旧进程句柄与 WS 线程，
        // 继续向下重新拉起内核。
        stopDaemonLocked(/*saveSession=*/false);
    }

    const std::string exe = engineExePath();
    if (exe.empty()) {
        lastError_ = tr("err.engine_binary_not_found");
        return false;
    }

    const cfg::Aria2Config a2 = cfg::aria2Config();
    const auto extra = daemonExtraOpts(a2, sessionMode_, isolatedStateDir_);
    const std::filesystem::path logPath =
        cfg::configDir() / (sessionMode_ == SessionMode::Persistent
                                ? "tinynext-aria2.log"
                                : "tinynext-aria2-headless.log");

    bool exitedImmediately = false;
    for (int spawnAttempt = 0; spawnAttempt < 2; ++spawnAttempt) {
        exitedImmediately = false;
        const int port = pickFreePort();
        const std::string secret = randomHex(16);

#ifdef _WIN32
        process_ = std::make_unique<Process>();
        const std::wstring wExe = std::filesystem::path(exe).wstring();
        const auto utf8ToWide = [](std::string_view value) -> std::wstring {
            if (value.empty()) return {};
            const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                  value.data(),
                                                  static_cast<int>(value.size()),
                                                  nullptr, 0);
            if (count <= 0) return {};
            std::wstring wide(static_cast<std::size_t>(count), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                    static_cast<int>(value.size()), wide.data(), count) <= 0) {
                return {};
            }
            return wide;
        };
        // 按 Windows MSVCRT / CommandLineToArgvW 规则转义参数：含空格/Tab/双引号时
        // 外包双引号，且内部双引号与闭合引号前的连续反斜杠严格倍增转义（保证带双引号的
        // HTTP Header / UA 及带反斜杠结尾的路径不破坏命令行解析）。
        const auto winValue = [&](const std::string& v) -> std::wstring {
            const std::wstring w = utf8ToWide(v);
            if (w.find_first_of(L" \t\"") == std::wstring::npos) {
                return w;
            }
            std::wstring out = L"\"";
            std::size_t backslashes = 0;
            for (const wchar_t ch : w) {
                if (ch == L'\\') {
                    ++backslashes;
                } else if (ch == L'"') {
                    out.append(backslashes * 2 + 1, L'\\');
                    out.push_back(L'"');
                    backslashes = 0;
                } else {
                    out.append(backslashes, L'\\');
                    out.push_back(ch);
                    backslashes = 0;
                }
            }
            out.append(backslashes * 2, L'\\');
            out.push_back(L'"');
            return out;
        };
        std::wstring cmdLine = winValue(utf8FromPath(std::filesystem::path(exe))) +
                               L" --enable-rpc --rpc-listen-all=false "
                               L"--rpc-listen-port=" + std::to_wstring(port) + L" "
                               L"--rpc-secret=" + winValue(secret) + L" "
                               L"--max-connection-per-server=" +
                                   std::to_wstring(a2.maxConnectionPerServer) + L" "
                               L"--split=" + std::to_wstring(a2.split) + L" "
                               L"--min-split-size=" + winValue(a2.minSplitSize) + L" "
                               L"--console-log-level=warn";
        for (const auto& [name, value] : extra) {
            cmdLine += L" --" + utf8ToWide(name) + L"=" + winValue(value);
        }

        std::error_code lsec;
        std::filesystem::create_directories(logPath.parent_path(), lsec);
        SECURITY_ATTRIBUTES inheritAttrs{};
        inheritAttrs.nLength = sizeof(inheritAttrs);
        inheritAttrs.bInheritHandle = TRUE;
        tinynext::native::UniqueHandle logFile(CreateFileW(
            logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inheritAttrs, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        tinynext::native::UniqueHandle childInput;
        if (logFile) {
            childInput.reset(CreateFileW(L"NUL", GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         &inheritAttrs, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr));
        }

        STARTUPINFOEXW startup{};
        STARTUPINFOW& si = startup.StartupInfo;
        if (logFile) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = childInput.get();
            si.hStdOutput = logFile.get();
            si.hStdError = logFile.get();
        }
        std::vector<HANDLE> inheritedHandles;
        if (logFile) inheritedHandles.push_back(logFile.get());
        if (childInput) inheritedHandles.push_back(childInput.get());
        tinynext::native::UniqueProcThreadAttributeList attributes;
        const bool inheritHandles = !inheritedHandles.empty() &&
            attributes.initialize(inheritedHandles.data(),
                                  static_cast<DWORD>(inheritedHandles.size()));
        DWORD creationFlags = CREATE_NO_WINDOW;
        if (inheritHandles) {
            startup.lpAttributeList = attributes.get();
            si.cb = sizeof(startup);
            creationFlags |= EXTENDED_STARTUPINFO_PRESENT;
        } else {
            si.dwFlags = 0;
            si.cb = sizeof(STARTUPINFOW);
            logFile.reset();
            childInput.reset();
        }
        PROCESS_INFORMATION pi{};
        // The explicit handle list prevents unrelated inheritable app handles from
        // keeping sockets, pipes, or locks alive in the aria2 child process.
        if (!CreateProcessW(wExe.c_str(), &cmdLine[0], nullptr, nullptr, inheritHandles,
                            creationFlags, nullptr, nullptr, &si, &pi)) {
            process_.reset();
            lastError_ = tr("err.engine_process_failed");
            return false;
        }
        process_->adopt(pi.hProcess);
        tinynext::native::UniqueHandle thread(pi.hThread);
        logFile.reset();
        childInput.reset();
#else
        process_ = std::make_unique<Process>();
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

        std::error_code lsec;
        std::filesystem::create_directories(logPath.parent_path(), lsec);
        tinynext::native::UniqueFd logFd(
            ::open(logPath.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644));

        pid_t pid = -1;
        int spawnResult = 0;
        {
            posix_spawn_file_actions_t fa;
            if (posix_spawn_file_actions_init(&fa) != 0) {
                process_.reset();
                lastError_ = tr("err.engine_process_failed");
                return false;
            }
            struct ActionsGuard {
                posix_spawn_file_actions_t* actions;
                ~ActionsGuard() { posix_spawn_file_actions_destroy(actions); }
            } actionsGuard{&fa};
            if (logFd) {
                posix_spawn_file_actions_adddup2(&fa, logFd.get(), STDOUT_FILENO);
                posix_spawn_file_actions_adddup2(&fa, logFd.get(), STDERR_FILENO);
            }
            spawnResult = ::posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
        }
        if (spawnResult != 0) {
            process_.reset();
            lastError_ = tr("err.engine_process_failed");
            return false;
        }
        process_->adopt(pid);
        logFd.reset();
#endif

        port_ = port;
        secret_ = secret;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (process_ && process_->exited()) {
                exitedImmediately = true;
                break;  // daemon died right after spawn (e.g. invalid args / rare port race)
            }
            try {
                const nlohmann::json v =
                    rpcCall(port_, secret_, "aria2.getVersion", nlohmann::json::array());
                daemonSpawned_ = true;
                if (v.is_object()) {
                    const std::string ver = v.value("version", "");
                    std::lock_guard<std::mutex> hlock(healthMutex_);
                    healthInfo_.binaryFound = true;
                    healthInfo_.daemonSpawned = true;
                    healthInfo_.daemonAlive = true;
                    healthInfo_.version = ver;
                    healthInfo_.supportsMediaTrackSelection = hasMediaTrackSelectionSupport(v);
                    healthInfo_.rpcReachable = true;
                    healthInfo_.rpcPort = port_;
                    healthInfo_.error.clear();
                    healthInfo_.checked = true;
                }
                lastError_.clear();
                if (sessionMode_ == SessionMode::Persistent) {
                    recoverSession(port_, secret_);
                    core::platform::requestUiUpdate();
                }
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

        if (process_) process_->terminate();
        process_.reset();
        port_ = 0;
        secret_.clear();
        // 若进程并未退出而是超时未响应，不必再换端口重来。
        if (!exitedImmediately) break;
    }

    lastError_ = exitedImmediately ? tr("err.engine_process_failed")
                                   : tr("err.engine_startup_timeout");
    return false;
}

std::uint64_t Aria2Engine::start(const std::string& url, const std::filesystem::path& destPath,
                                 const StartOptions& options) {
    if (!ensureDaemon()) return 0;
    const RpcEndpoint ep = currentEndpoint();
    if (!ep.active) return 0;

    auto task = std::make_shared<Task>();
    task->id = nextId_.fetch_add(1, std::memory_order_relaxed);
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
    optionsJ["dir"] = utf8FromPath(dir);  // aria2 JSON 是 UTF-8，.string() 在 Windows 走 ANSI
    optionsJ["split"] = std::to_string(connections);
    optionsJ["max-connection-per-server"] = std::to_string(connections);
    optionsJ["min-split-size"] = a2.minSplitSize;
    // 新任务不从目标目录继承旧文件。aria2-next 2.8.x 只按 state-dir 中的
    // GID/URL/输出路径匹配恢复记录，不再读取旁置 .aria2 控制文件。
    optionsJ["continue"] = "false";
    if (options.pauseMetadata && url.starts_with("magnet:")) {
        optionsJ["pause-metadata"] = "true";
    }
    if (options.pauseMediaAfterProbe) optionsJ["media-pause-after-probe"] = "true";
    if (options.mediaMode != "auto") optionsJ["media"] = options.mediaMode;
    if (options.mediaFormat == "mkv") optionsJ["media-format"] = "mkv";

    // 每任务限速已移除（无意义），统一用配置的 maxDownloadLimit。
    if (a2.maxDownloadLimit > 0) {
        optionsJ["max-download-limit"] = std::to_string(a2.maxDownloadLimit);
    }

    nlohmann::json params;
    std::string method = "aria2.addUri";

    if (!options.torrentPath.empty()) {
        task->destPath = dir / ("torrent-" + std::to_string(task->id));
        std::ifstream in(options.torrentPath, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            task->state = State::Failed;
            task->error = tr("err.torrent_read_failed") +
                          utf8FromPath(options.torrentPath);
            tasks_.push_back(task);
            return task->id;
        }
        params = nlohmann::json::array();
        params.push_back(macaron::Base64::Encode(bytes));
        params.push_back(nlohmann::json::array());
        params.push_back(optionsJ);
        method = "aria2.addTorrent";
    } else {
        const bool magnet = url.starts_with("magnet:");
        if (magnet) {
            task->destPath = dir / ("magnet-" + std::to_string(task->id));
        } else if (!options.outputName.empty()) {
            const std::filesystem::path uniqueDest =
                makeUniqueDest(dir / pathFromUtf8(options.outputName));
            optionsJ["out"] = utf8FromPath(uniqueDest.filename());
            task->destPath = uniqueDest;
        } else {
            task->destPath = dir / destPath.filename();
        }
        nlohmann::json uriArray = nlohmann::json::array();
        uriArray.push_back(url);
        for (const auto& m : options.mirrors) uriArray.push_back(m);
        params = nlohmann::json::array();
        params.push_back(std::move(uriArray));
        params.push_back(optionsJ);
    }

    try {
        const nlohmann::json result = rpcCall(ep.port, ep.secret, method, params);
        task->gid = result.get<std::string>();
    } catch (const std::exception& e) {
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

// ---- 后台命令队列：任务动作的 rpcCall 走独立线程，UI 线程只乐观更新后立即返回 ----

void Aria2Engine::enqueue(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        if (cmdShutdown_) return;
        if (!cmdThread_.joinable()) {
            // 懒创建：首个命令提交时才起 worker（第二实例 boot 不会白开线程）。
            cmdThread_ = std::thread([this] { commandLoop(); });
        }
        cmdQueue_.push_back(std::move(fn));
    }
    cmdCv_.notify_one();
}

void Aria2Engine::commandLoop() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(cmdMutex_);
            cmdCv_.wait(lock, [this] { return cmdShutdown_ || !cmdQueue_.empty(); });
            if (cmdShutdown_) break;  // 退出时丢弃所有未处理命令
            fn = std::move(cmdQueue_.front());
            cmdQueue_.pop_front();
        }
        try {
            if (fn) fn();
        } catch (...) {}  // 单条命令异常不拖垮队列
    }
}

void Aria2Engine::cancel(std::uint64_t id) {
    const bool active = engineActive();
    std::string gid;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        auto task = findTask(id);
        if (!task || !active) return;
        gid = task->gid;
        task->state = State::Cancelled;
        task->speedBps = 0.0;
    }
    if (gid.empty()) return;
    enqueue([this, gid] {
        const RpcEndpoint ep = currentEndpoint();
        if (!ep.active) return;
        try {
            rpcCall(ep.port, ep.secret, "aria2.remove", nlohmann::json::array({gid}));
        } catch (...) {
            try {
                rpcCall(ep.port, ep.secret, "aria2.forceRemove", nlohmann::json::array({gid}));
            } catch (...) {}
        }
    });
}

void Aria2Engine::remove(std::uint64_t id) {
    const bool active = engineActive();
    std::string gid;
    bool needsDaemon = false;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        auto task = findTask(id);
        if (!task) return;
        gid = task->gid;
        needsDaemon = active && !gid.empty();
        std::erase_if(tasks_, [&](const std::shared_ptr<Task>& t) { return t->id == id; });
    }
    if (!needsDaemon) return;
    enqueue([this, gid] {
        const RpcEndpoint ep = currentEndpoint();
        if (!ep.active) return;
        try {
            rpcCall(ep.port, ep.secret, "aria2.forceRemove", nlohmann::json::array({gid}));
        } catch (...) {}
        try {
            rpcCall(ep.port, ep.secret, "aria2.removeDownloadResult",
                    nlohmann::json::array({gid}));
        } catch (...) {}
        if (sessionMode_ == SessionMode::Persistent) {
            purgeFailedBeforeSessionSave(ep.port, ep.secret);
            try {
                rpcCall(ep.port, ep.secret, "aria2.saveSession", nlohmann::json::array());
            } catch (...) {}
        }
    });
}

void Aria2Engine::pause(std::uint64_t id) {
    const bool active = engineActive();
    std::string gid;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        auto task = findTask(id);
        if (!task || !active) return;
        if (task->state != State::Queued && task->state != State::Downloading) return;
        gid = task->gid;
        task->state = State::Paused;
        task->speedBps = 0.0;
    }
    if (gid.empty()) return;
    enqueue([this, gid] {
        const RpcEndpoint ep = currentEndpoint();
        if (!ep.active) return;
        try {
            rpcCall(ep.port, ep.secret, "aria2.pause", nlohmann::json::array({gid}));
        } catch (...) {}
    });
}

void Aria2Engine::resume(std::uint64_t id) {
    const bool active = engineActive();
    const bool mediaTrackSelectionSupported = health().supportsMediaTrackSelection;
    std::string gid;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        auto task = findTask(id);
        if (!task || !active) return;
        if (task->state != State::Paused) return;
        const bool hasMediaChoices = std::ranges::any_of(
            task->mediaTracks, [](const MediaTrackView& track) {
                return track.type == "video" || track.type == "audio" ||
                       track.type == "subtitle" || track.type == "subtitles";
        });
        if (task->awaitingTorrentFileSelection ||
            (task->awaitingMediaTrackSelection && mediaTrackSelectionSupported &&
             hasMediaChoices)) return;
        if (task->gid.empty()) return;
        gid = task->gid;
        task->state = State::Downloading;
    }
    if (gid.empty()) return;
    enqueue([this, gid] {
        const RpcEndpoint ep = currentEndpoint();
        if (!ep.active) return;
        try {
            rpcCall(ep.port, ep.secret, "aria2.unpause", nlohmann::json::array({gid}));
        } catch (...) {}
    });
}

void Aria2Engine::pauseAll() {
    if (!engineActive()) return;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        for (const auto& task : tasks_) {
            if (task->state == State::Queued || task->state == State::Downloading) {
                task->state = State::Paused;
                task->speedBps = 0.0;
            }
        }
    }
    enqueue([this] {
        const RpcEndpoint ep = currentEndpoint();
        if (!ep.active) return;
        try {
            rpcCall(ep.port, ep.secret, "aria2.pauseAll", nlohmann::json::array());
        } catch (...) {}
    });
}

void Aria2Engine::resumeAll() {
    if (!engineActive()) return;
    const bool mediaTrackSelectionSupported = health().supportsMediaTrackSelection;
    std::vector<std::string> gids;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        for (const auto& task : tasks_) {
            if (task->state == State::Paused) {
                if (task->gid.empty()) continue;
                const bool hasMediaChoices = std::ranges::any_of(
                    task->mediaTracks, [](const MediaTrackView& track) {
                        return track.type == "video" || track.type == "audio" ||
                               track.type == "subtitle" || track.type == "subtitles";
                    });
                if (task->awaitingTorrentFileSelection ||
                    (task->awaitingMediaTrackSelection && mediaTrackSelectionSupported &&
                     hasMediaChoices)) continue;
                task->state = State::Downloading;
                gids.push_back(task->gid);
            }
        }
    }
    if (gids.empty()) return;
    enqueue([this, gids = std::move(gids)] {
        const RpcEndpoint ep = currentEndpoint();
        if (!ep.active) return;
        for (const auto& gid : gids) {
            try {
                rpcCall(ep.port, ep.secret, "aria2.unpause", nlohmann::json::array({gid}));
            } catch (...) {}
        }
    });
}

void Aria2Engine::retry(std::uint64_t id) {
    bool allowed = false;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        const auto task = findTask(id);
        allowed = task &&
                  (task->state == State::Failed || task->state == State::Cancelled);
    }
    if (!allowed) return;
    enqueue([this, id] { retryOnWorker(id); });
}

void Aria2Engine::retryOnWorker(std::uint64_t id) {
    if (!ensureDaemon()) {
        const std::string err = lastError();
        std::lock_guard<std::mutex> lock(tasksMutex_);
        const auto task = findTask(id);
        if (task) {
            task->state = State::Failed;
            task->error = err.empty() ? tr("err.engine_unavailable") : err;
        }
        return;
    }
    const RpcEndpoint ep = currentEndpoint();
    if (!ep.active) {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        if (const auto task = findTask(id)) {
            task->state = State::Failed;
            task->error = tr("err.engine_unavailable");
        }
        return;
    }

    std::lock_guard<std::mutex> lock(tasksMutex_);
    const auto task = findTask(id);
    if (!task) return;

    const cfg::Aria2Config a2 = cfg::aria2Config();
    const int connections = task->opts.connections > 0
        ? std::clamp(task->opts.connections, 1, 64)
        : a2.split;
    nlohmann::json optionsJ = nlohmann::json::object();
    optionsJ["dir"] = utf8FromPath(task->destPath.parent_path());
    optionsJ["split"] = std::to_string(connections);
    optionsJ["max-connection-per-server"] = std::to_string(connections);
    optionsJ["min-split-size"] = a2.minSplitSize;
    optionsJ["continue"] = "true";
    if (task->opts.pauseMetadata && task->url.starts_with("magnet:")) {
        optionsJ["pause-metadata"] = "true";
    }
    if (task->opts.mediaMode != "auto") optionsJ["media"] = task->opts.mediaMode;
    if (task->opts.mediaFormat == "mkv") optionsJ["media-format"] = "mkv";
    if (a2.maxDownloadLimit > 0) {
        optionsJ["max-download-limit"] = std::to_string(a2.maxDownloadLimit);
    }
    if (task->isMedia && !task->gid.empty()) {
        try {
            rpcCall(ep.port, ep.secret, "aria2.retryMedia",
                    nlohmann::json::array({task->gid}));
            task->state = State::Queued;
            task->error.clear();
            task->speedBps = 0.0;
        } catch (const std::exception& e) {
            task->state = State::Failed;
            task->error = e.what();
        }
        return;
    }

    const std::string originalGid = task->gid;
    if (!originalGid.empty()) {
        try {
            rpcCall(ep.port, ep.secret, "aria2.removeDownloadResult",
                    nlohmann::json::array({originalGid}));
        } catch (...) {}
        optionsJ["gid"] = originalGid;
    }

    nlohmann::json params;
    std::string method = "aria2.addUri";

    if (!task->opts.torrentPath.empty()) {
        std::ifstream in(task->opts.torrentPath, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            task->state = State::Failed;
            task->error = tr("err.torrent_read_failed") +
                          utf8FromPath(task->opts.torrentPath);
            return;
        }
        params = nlohmann::json::array();
        params.push_back(macaron::Base64::Encode(bytes));
        params.push_back(nlohmann::json::array());
        params.push_back(optionsJ);
        method = "aria2.addTorrent";
    } else {
        if (!task->url.starts_with("magnet:")) {
            optionsJ["out"] = utf8FromPath(task->destPath.filename());
        }
        nlohmann::json uriArray = nlohmann::json::array();
        uriArray.push_back(task->url);
        for (const auto& m : task->opts.mirrors) uriArray.push_back(m);
        params = nlohmann::json::array();
        params.push_back(std::move(uriArray));
        params.push_back(optionsJ);
    }
    try {
        const nlohmann::json result = rpcCall(ep.port, ep.secret, method, params);
        task->gid = result.get<std::string>();
        task->state = State::Queued;
        task->error.clear();
        task->speedBps = 0.0;
    } catch (const std::exception& e) {
        task->state = State::Failed;
        task->error = e.what();
    }
}

void Aria2Engine::addMirror(std::uint64_t id, const std::string& url,
                            std::function<void(bool)> onDone) {
    const bool active = engineActive();
    std::string gid;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        auto task = findTask(id);
        if (!task || !active || task->gid.empty()) {
            if (onDone) onDone(false);
            return;
        }
        if (task->state != State::Queued && task->state != State::Downloading &&
            task->state != State::Paused) {
            if (onDone) onDone(false);
            return;
        }
        gid = task->gid;
    }
    enqueue([this, id, gid, url, onDone] {
        const RpcEndpoint ep = currentEndpoint();
        if (!ep.active) {
            if (onDone) onDone(false);
            return;
        }
        try {
            const nlohmann::json res = rpcCall(
                ep.port, ep.secret, "aria2.changeUri",
                nlohmann::json::array({gid, 1, nlohmann::json::array(),
                                       nlohmann::json::array({url})}));
            if (res.is_array() && res.size() == 2 && res[1].get<int>() > 0) {
                std::lock_guard<std::mutex> lock(tasksMutex_);
                if (const auto task = findTask(id)) {
                    task->opts.mirrors.push_back(url);
                }
                if (onDone) onDone(true);
                return;
            }
        } catch (...) {}
        if (onDone) onDone(false);
    });
}

void Aria2Engine::removeMirror(std::uint64_t id, const std::string& url,
                               std::function<void(bool)> onDone) {
    const bool active = engineActive();
    std::string gid;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        auto task = findTask(id);
        if (!task || !active || task->gid.empty()) {
            if (onDone) onDone(false);
            return;
        }
        if (task->state != State::Queued && task->state != State::Downloading &&
            task->state != State::Paused) {
            if (onDone) onDone(false);
            return;
        }
        gid = task->gid;
    }
    enqueue([this, id, gid, url, onDone] {
        const RpcEndpoint ep = currentEndpoint();
        if (!ep.active) {
            if (onDone) onDone(false);
            return;
        }
        try {
            const nlohmann::json res = rpcCall(
                ep.port, ep.secret, "aria2.changeUri",
                nlohmann::json::array({gid, 1, nlohmann::json::array({url}),
                                       nlohmann::json::array()}));
            if (res.is_array() && res.size() == 2 && res[0].get<int>() > 0) {
                std::lock_guard<std::mutex> lock(tasksMutex_);
                if (const auto task = findTask(id)) {
                    std::erase(task->opts.mirrors, url);
                }
                if (onDone) onDone(true);
                return;
            }
        } catch (...) {}
        if (onDone) onDone(false);
    });
}

void Aria2Engine::selectTorrentFiles(
    std::uint64_t id, const std::vector<int>& indexes,
    std::function<void(bool, std::string)> onDone) {
    std::string gid;
    std::vector<int> selected;
    for (const int index : indexes) {
        if (index <= 0) continue;
        if (std::ranges::find(selected, index) == selected.end()) {
            selected.push_back(index);
        }
    }
    if (selected.empty()) {
        if (onDone) onDone(false, "Select at least one file");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        const auto task = findTask(id);
        if (!task || task->gid.empty() || !task->awaitingTorrentFileSelection ||
            task->state != State::Paused) {
            if (onDone) onDone(false, "Task is not waiting for file selection");
            return;
        }
        gid = task->gid;
    }
    std::string selection;
    for (const int index : selected) {
        if (!selection.empty()) selection += ',';
        selection += std::to_string(index);
    }
    enqueue([this, id, gid = std::move(gid), selection = std::move(selection),
             selected = std::move(selected), onDone = std::move(onDone)] {
        const RpcEndpoint ep = currentEndpoint();
        if (!ep.active) {
            if (onDone) onDone(false, tr("err.engine_unavailable"));
            return;
        }
        try {
            rpcCall(ep.port, ep.secret, "aria2.changeOption",
                    nlohmann::json::array({gid, {{"select-file", selection}}}));
            rpcCall(ep.port, ep.secret, "aria2.unpause", nlohmann::json::array({gid}));
            {
                std::lock_guard<std::mutex> lock(tasksMutex_);
                if (const auto task = findTask(id)) {
                    task->state = State::Queued;
                    task->awaitingTorrentFileSelection = false;
                    for (auto& file : task->torrentFiles) {
                        file.selected = std::ranges::find(selected, file.index) != selected.end();
                    }
                }
            }
            if (onDone) onDone(true, {});
        } catch (const std::exception& e) {
            if (onDone) onDone(false, e.what());
        }
    });
}

void Aria2Engine::finishMedia(
    std::uint64_t id, std::function<void(bool, std::string)> onDone) {
    std::string gid;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        const auto task = findTask(id);
        if (!task || task->gid.empty() || !task->isMedia || !task->mediaLive ||
            (task->mediaState != "recording" && task->mediaState != "paused") ||
            (task->state != State::Downloading && task->state != State::Paused)) {
            if (onDone) onDone(false, "Task is not an active live recording");
            return;
        }
        gid = task->gid;
    }
    enqueue([this, id, gid = std::move(gid), onDone = std::move(onDone)] {
        const RpcEndpoint ep = currentEndpoint();
        if (!ep.active) {
            if (onDone) onDone(false, tr("err.engine_unavailable"));
            return;
        }
        try {
            rpcCall(ep.port, ep.secret, "aria2.finishMedia", nlohmann::json::array({gid}));
            if (onDone) onDone(true, {});
        } catch (const std::exception& e) {
            if (onDone) onDone(false, e.what());
        }
    });
}

void Aria2Engine::selectMediaTracks(
    std::uint64_t id, const std::string& video, const std::string& audio,
    const std::string& subtitles, std::function<void(bool, std::string)> onDone) {
    if (!health().supportsMediaTrackSelection) {
        if (onDone) onDone(false, {});
        return;
    }
    std::string gid;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        const auto task = findTask(id);
        if (!task || task->gid.empty() || !task->isMedia ||
            !task->awaitingMediaTrackSelection || task->state != State::Paused) {
            if (onDone) onDone(false, "Task is not waiting for media track selection");
            return;
        }
        if (task->mediaTracks.empty()) {
            if (onDone) onDone(false, {});
            return;
        }
        gid = task->gid;
    }
    enqueue([this, id, gid = std::move(gid), video, audio, subtitles,
             onDone = std::move(onDone)] {
        const RpcEndpoint ep = currentEndpoint();
        if (!ep.active) {
            if (onDone) onDone(false, tr("err.engine_unavailable"));
            return;
        }
        try {
            rpcCall(ep.port, ep.secret, "aria2.changeOption",
                    nlohmann::json::array({gid, {
                        {"media-video", video},
                        {"media-audio", audio},
                        {"media-subtitles", subtitles},
                        {"media-pause-after-probe", "false"}
                    }}));
            rpcCall(ep.port, ep.secret, "aria2.unpause", nlohmann::json::array({gid}));
            {
                std::lock_guard<std::mutex> lock(tasksMutex_);
                if (const auto task = findTask(id)) {
                    task->opts.pauseMediaAfterProbe = false;
                    task->state = State::Queued;
                    task->awaitingMediaTrackSelection = false;
                    for (auto& track : task->mediaTracks) {
                        track.selected = track.id == video || track.id == audio ||
                                         track.id == subtitles;
                    }
                }
            }
            if (onDone) onDone(true, {});
        } catch (const std::exception& e) {
            if (onDone) onDone(false, e.what());
        }
    });
}

// 重启或崩溃自愈后同步任务表：daemon 用 --input-file 载入了 --save-session 的未完成
// 任务，这里通过 tellActive/tellWaiting/tellStopped 把它们对齐成本地 Task。
// 调用方须持 daemonMutex_（锁序 daemonMutex_ → tasksMutex_）。
void Aria2Engine::recoverSession(int port, const std::string& secret) const {
    if (!daemonSpawned_ || port == 0) return;
    std::lock_guard<std::mutex> lock(tasksMutex_);
    std::vector<std::shared_ptr<Task>> recovered;
    std::unordered_set<std::string> seenGids;

    const auto findByGid = [&](const std::string& gid) -> std::shared_ptr<Task> {
        if (gid.empty()) return nullptr;
        for (const auto& t : tasks_) {
            if (t->gid == gid) return t;
        }
        return nullptr;
    };

    const auto populateMeta = [&](const std::shared_ptr<Task>& task,
                                  const nlohmann::json& st) {
        if (st.contains("files") && st["files"].is_array() && !st["files"].empty()) {
            const auto& file = st["files"][0];
            const std::string p = file.value("path", "");
            if (!p.empty()) task->destPath = pathFromUtf8(p);
            if (file.contains("uris") && file["uris"].is_array() && !file["uris"].empty()) {
                if (task->url.empty()) {
                    task->url = file["uris"][0].value("uri", "");
                }
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
        if (task->url.empty() && st.contains("bittorrent") && st["bittorrent"].is_object()) {
            const std::string ih = st["bittorrent"].value("infoHash", "");
            if (!ih.empty()) task->url = "magnet:?xt=urn:btih:" + ih;
        }
        if (st.contains("bittorrent") && st["bittorrent"].is_object() &&
            st["bittorrent"].contains("info") && st["bittorrent"]["info"].is_object()) {
            task->displayName = st["bittorrent"]["info"].value("name", "");
        }
        applyTellStatus(task, st);
    };

    const auto addFrom = [&](const nlohmann::json& st) {
        if (!st.is_object()) return;
        const std::string gid = st.value("gid", "");
        if (!gid.empty()) seenGids.insert(gid);
        // 崩溃重拉起场景下 tasks_ 可能已有同 GID 卡片：原地更新，避免生成重复卡片。
        if (const auto existing = findByGid(gid)) {
            populateMeta(existing, st);
            return;
        }
        auto task = std::make_shared<Task>();
        task->id = nextId_.fetch_add(1, std::memory_order_relaxed);
        task->gid = gid;
        populateMeta(task, st);
        task->fromSession = true;   // 会话恢复的历史任务：不再发完成/失败通知
        recovered.push_back(std::move(task));
    };

    try {
        for (const auto& st : rpcCall(port, secret, "aria2.tellActive",
                                      nlohmann::json::array())) {
            addFrom(st);
        }
        for (const auto& st : rpcCall(port, secret, "aria2.tellWaiting",
                                      nlohmann::json::array({0, 1000}))) {
            addFrom(st);
        }
        for (const auto& st : rpcCall(port, secret, "aria2.tellStopped",
                                      nlohmann::json::array({0, 1000}))) {
            if (!st.is_object()) continue;
            const std::string status = st.value("status", "");
            if (status == "error" && restoreFailedOnRecover_) {
                addFrom(st);
                const bool mediaTask = st.contains("media") && st["media"].is_object();
                if (!mediaTask) {
                    try {
                        rpcCall(port, secret, "aria2.removeDownloadResult",
                                nlohmann::json::array({st.value("gid", "")}));
                    } catch (...) {}
                }
            } else if (status == "error" || status == "removed") {
                try {
                    rpcCall(port, secret, "aria2.removeDownloadResult",
                            nlohmann::json::array({st.value("gid", "")}));
                } catch (...) {}
            }
        }
    } catch (...) {
        // 会话枚举失败不致命：本地任务仍可用，下次再试。
    }

    // 崩溃重拉起后，凡未出现在新 daemon 会话里的残留活动任务统一置 Failed（用户可点重试按原 GID 续传）。
    for (const auto& existing : tasks_) {
        if ((existing->state == State::Queued ||
             existing->state == State::Downloading ||
             existing->state == State::Paused) &&
            (existing->gid.empty() || seenGids.count(existing->gid) == 0)) {
            existing->state = State::Failed;
            existing->speedBps = 0.0;
            existing->needsFinalize = false;
            if (existing->error.empty()) {
                existing->error = tr("err.engine_process_failed");
            }
        }
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
    const auto readString = [](const nlohmann::json& value) -> std::string {
        if (value.is_string()) return value.get<std::string>();
        if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
        if (value.is_number_float()) return std::to_string(value.get<double>());
        if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
        return {};
    };
    const auto mediaIt = st.find("media");
    task->isMedia = mediaIt != st.end() && mediaIt->is_object();
    if (task->isMedia) {
        const auto& media = *mediaIt;
        task->mediaProtocol = media.value("protocol", "");
        task->mediaState = media.value("state", "");
        task->mediaLive = readString(media.value("live", nlohmann::json("false"))) == "true";
        task->mediaProgress = 0.0;
        try {
            task->mediaProgress = std::clamp(
                std::stod(readString(media.value("progress", nlohmann::json("0")))),
                0.0, 1.0);
        } catch (...) {}
        const auto readMediaI64 = [&](const char* key) -> std::int64_t {
            try { return std::stoll(readString(media.value(key, nlohmann::json("0")))); }
            catch (...) { return std::int64_t{0}; }
        };
        task->mediaDurationMs = readMediaI64("duration");
        task->mediaCompletedDurationMs = readMediaI64("completedDuration");
        task->mediaDownloadedBytes = readMediaI64("downloadedLength");
        task->mediaTracks.clear();
        const auto tracksIt = media.find("tracks");
        if (tracksIt != media.end() && tracksIt->is_array()) {
            for (const auto& item : *tracksIt) {
                if (!item.is_object()) continue;
                MediaTrackView track;
                track.id = readString(item.value("id", nlohmann::json{}));
                track.type = readString(item.value("type", item.value("kind", nlohmann::json{})));
                if (track.type == "subtitles") track.type = "subtitle";
                track.codec = readString(item.value("codec", nlohmann::json{}));
                track.language = readString(item.value("language", nlohmann::json{}));
                track.width = readString(item.value("width", nlohmann::json{}));
                track.height = readString(item.value("height", nlohmann::json{}));
                track.frameRate = readString(item.value("frameRate", nlohmann::json{}));
                track.bandwidth = readString(item.value("bandwidth", nlohmann::json{}));
                track.selected = readString(item.value("selected", nlohmann::json("false"))) == "true";
                if (!track.id.empty()) task->mediaTracks.push_back(std::move(track));
            }
        }
        task->awaitingMediaTrackSelection = task->mediaState == "awaiting-selection";
        if (task->awaitingMediaTrackSelection) task->opts.pauseMediaAfterProbe = true;
    } else {
        task->mediaProtocol.clear();
        task->mediaState.clear();
        task->mediaLive = false;
        task->mediaProgress = 0.0;
        task->mediaDurationMs = 0;
        task->mediaCompletedDurationMs = 0;
        task->mediaDownloadedBytes = 0;
        task->awaitingMediaTrackSelection = false;
        task->mediaTracks.clear();
    }
    // 磁力/BT 任务拿到元数据后，files[0].path 才是真实下载路径（更新占位）。
    if (st.contains("files") && st["files"].is_array() &&
        !st["files"].empty()) {
        const std::string p = st["files"][0].value("path", "");
        if (!p.empty()) task->destPath = pathFromUtf8(p);  // aria2 JSON 是 UTF-8
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
    task->torrentFiles.clear();
    task->awaitingTorrentFileSelection = false;
    if (st.contains("bittorrent") && st["bittorrent"].is_object()) {
        const auto& bt = st["bittorrent"];
        task->awaitingTorrentFileSelection =
            bt.value("fileSelectionState", "") == "awaiting";
        if (task->awaitingTorrentFileSelection) task->opts.pauseMetadata = true;
        if (st.contains("files") && st["files"].is_array()) {
            for (const auto& file : st["files"]) {
                if (!file.is_object()) continue;
                TorrentFileView entry;
                try { entry.index = std::stoi(readString(file.value("index", nlohmann::json("0")))); }
                catch (...) { continue; }
                entry.path = file.value("path", "");
                try { entry.length = std::stoll(readString(file.value("length", nlohmann::json("0")))); }
                catch (...) { entry.length = 0; }
                entry.selected = readString(file.value("selected", nlohmann::json("false"))) == "true";
                if (entry.index > 0) task->torrentFiles.push_back(std::move(entry));
            }
        }
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

// 轮询活动任务进度 + 补全 needsFinalize 任务。内部管理锁（不要求调用方持锁）。
// 状态迁移主要由 WS 事件接管（即时），这里的 tellStatus 负责进度字节/速度/连接数，
// 以及事件只带 gid 时的最终态补全。
// 注意：rpcCall 可能阻塞 ~500ms，所以先收集目标后释放锁，RPC 完毕再取锁写结果；
// 避免堵塞 UI 线程的 snapshot（它也要取 tasksMutex_），导致「连接信息时有时无」。
void Aria2Engine::refreshStates() const {
    RpcEndpoint ep;
    bool daemonExitedUnexpectedly = false;
    {
        std::lock_guard<std::mutex> lock(daemonMutex_);
        if (!daemonSpawned_) return;
        if (port_ != 0 && process_ && !process_->exited()) {
            ep = {true, port_, secret_};
        } else {
            daemonExitedUnexpectedly = true;
        }
    }
    if (daemonExitedUnexpectedly) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            for (const auto& task : tasks_) {
                if (task->state == State::Queued ||
                    task->state == State::Downloading ||
                    task->state == State::Paused) {
                    task->state = State::Failed;
                    task->speedBps = 0.0;
                    task->needsFinalize = false;
                    if (task->error.empty()) {
                        task->error = tr("err.engine_process_failed");
                    }
                    changed = true;
                }
            }
        }
        if (changed && sessionMode_ == SessionMode::Persistent) {
            core::platform::requestUiUpdate();
        }
        return;
    }

    // 阶段 1：持锁收集目标 gid，极短。
    std::vector<std::pair<std::uint64_t, std::string>> targets;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        targets.reserve(tasks_.size());
        for (const auto& task : tasks_) {
            const State s = task->state;
            if (s == State::Queued || s == State::Downloading || s == State::Paused ||
                task->needsFinalize) {
                targets.emplace_back(task->id, task->gid);
            }
        }
    }
    // 阶段 2：解锁做 RPC（不阻塞 UI snapshot）。
    for (const auto& [id, gid] : targets) {
        if (gid.empty()) continue;
        nlohmann::json st;
        try {
            st = rpcCall(ep.port, ep.secret, "aria2.tellStatus", nlohmann::json::array({gid}));
        } catch (...) {
            continue; // Transient RPC failure — leave state as-is, retry next poll.
        }
        // 阶段 3：取锁写结果（短暂持锁）。
        {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            auto task = findTask(id);
            if (!task) continue;
            applyTellStatus(task, st);
            task->needsFinalize = false;
        }
    }
}

// 后台线程进度轮询：~1s 节流批量 tellStatus（UI 线程的 snapshot 已剥离 RPC，
// 进度刷新由 housekeep / headless 等待循环显式驱动，绝不占 UI 线程）。
void Aria2Engine::pollProgress() {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPoll_ >= std::chrono::seconds(1)) {
        lastPoll_ = now;
        refreshStates();
    }
}

// ---- 引擎监控：健康快照（纯读缓存）+ 后台健康检查 + 重启 ----

HealthInfo Aria2Engine::health() const {
    std::lock_guard<std::mutex> lock(healthMutex_);
    return healthInfo_;
}

// 后台命令线程执行：一次 getVersion + getGlobalStat 刷新健康缓存。RPC 参数在
// daemonMutex_ 下快照后释放锁再发请求（rpcCall 可能阻塞 ~3s，不能占锁）。结果
// 写 healthInfo_（healthMutex_），UI 线程经 health() 读；onDone 后台线程调用。
void Aria2Engine::refreshHealth(std::function<void(const HealthInfo&)> onDone) {
    enqueue([this, onDone] {
        HealthInfo info;
        info.binaryFound = !engineExePath().empty();
        int port = 0;
        std::string secret;
        {
            std::lock_guard<std::mutex> lock(daemonMutex_);
            info.daemonSpawned = daemonSpawned_;
            info.daemonAlive = daemonSpawned_ && process_ && !process_->exited();
            info.wsConnected = info.daemonAlive && ws_ != nullptr && ws_->isConnected();
            info.error = lastError_;
            if (info.daemonAlive) {
                port = port_;
                secret = secret_;
            }
        }
        if (port != 0) {
            try {
                const nlohmann::json v = rpcCall(port, secret, "aria2.getVersion",
                                                 nlohmann::json::array());
                info.rpcReachable = true;
                if (v.is_object()) {
                    info.version = v.value("version", "");
                    info.supportsMediaTrackSelection = hasMediaTrackSelectionSupport(v);
                }
                const nlohmann::json gs = rpcCall(port, secret, "aria2.getGlobalStat",
                                                  nlohmann::json::array());
                if (gs.is_object()) {
                    const auto i64 = [&](const char* key) -> std::int64_t {
                        try { return std::stoll(gs.value(key, "0")); }
                        catch (...) { return 0; }
                    };
                    info.downloadSpeedBps = i64("downloadSpeed");
                    info.uploadSpeedBps = i64("uploadSpeed");
                    info.activeDownloads = static_cast<int>(i64("numActive"));
                    info.waitingDownloads = static_cast<int>(i64("numWaiting"));
                    info.stoppedDownloads = static_cast<int>(i64("numStopped"));
                }
                info.rpcPort = port;
                info.error.clear();  // RPC 通了，旧错误不再有效
            } catch (const std::exception& e) {
                info.error = e.what();  // 服务在但无响应
            }
        }
        {
            std::lock_guard<std::mutex> lock(healthMutex_);
            info.checked = true;  // 无论结果如何，本次检查已执行
            healthInfo_ = info;
        }
        if (onDone) onDone(info);
        core::platform::requestUiUpdate();
    });
}

// 后台命令线程执行：全程持 daemonMutex_ 完成「保存会话 → 优雅停机 → 停旧 WS →
// 清任务表 → beforeRespawn 替换二进制 → ensureDaemonLocked 重新拉起」，杜绝中途
// 其他线程抢先拉起旧二进制或并发读写 ws_/tasks_。
void Aria2Engine::restartEngine(std::function<void(bool)> onDone,
                                std::function<bool()> beforeRespawn) {
    enqueue([this, onDone, beforeRespawn = std::move(beforeRespawn)]() mutable {
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(daemonMutex_);
            stopDaemonLocked(/*saveSession=*/true);
            {
                std::lock_guard<std::mutex> taskLock(tasksMutex_);
                tasks_.clear();
            }
            if (beforeRespawn && !beforeRespawn()) {
                lastError_ = tr("comp.error.replace");
                ok = false;
            } else {
                ok = ensureDaemonLocked();
            }
        }
        if (onDone) onDone(ok);
        core::platform::requestUiUpdate();
    });
}

std::vector<TaskView> Aria2Engine::snapshot() const {
    // 纯读缓存、立即返回：只持锁拷贝任务表，不发任何 RPC（UI 线程每帧调用）。
    // 状态迁移由 WS 事件接管（即时），进度字节/速度由 pollProgress（后台线程）
    // ~1s 刷新（RPC 在锁外做，绝不让本 snapshot 等待），UI 显示至多滞后 1s
    // （对齐 Motrix/AriaNg）。
    std::lock_guard<std::mutex> lock(tasksMutex_);
    std::vector<TaskView> out;
    out.reserve(tasks_.size());
    for (auto it = tasks_.rbegin(); it != tasks_.rend(); ++it) {
        const Task& task = **it;
        TaskView tv;
        tv.id = task.id;
        tv.url = task.url;
        tv.destPath = task.destPath;
        tv.state = task.state;
        tv.totalBytes = task.totalBytes;
        tv.downloadedBytes = task.downloadedBytes;
        tv.error = task.error;
        tv.speedBps = task.speedBps;
        tv.connections = task.connections;
        tv.displayName = task.displayName;
        tv.mirrorCount = static_cast<int>(task.opts.mirrors.size());
        tv.mirrors = task.mirrors;
        tv.fromSession = task.fromSession;
        tv.isMedia = task.isMedia;
        tv.mediaProtocol = task.mediaProtocol;
        tv.mediaState = task.mediaState;
        tv.mediaLive = task.mediaLive;
        tv.mediaProgress = task.mediaProgress;
        tv.mediaDurationMs = task.mediaDurationMs;
        tv.mediaCompletedDurationMs = task.mediaCompletedDurationMs;
        tv.mediaDownloadedBytes = task.mediaDownloadedBytes;
        tv.awaitingMediaTrackSelection = task.awaitingMediaTrackSelection;
        tv.mediaTracks = task.mediaTracks;
        tv.awaitingTorrentFileSelection = task.awaitingTorrentFileSelection;
        tv.torrentFiles = task.torrentFiles;
        // 在任务数据仍存活时预编码 destPath 到 UTF-8 串，后续读（如任务信息弹窗、终端输出）
        // 不走可能已悬空的 destPath（原生下载分支的遗留问题，见 dialogs.cppm）。
        tv.destPathUtf8 = task.destPath.empty() ? std::string() : utf8FromPath(task.destPath);
        out.push_back(std::move(tv));
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

// 「启动时自动重试失败任务」关闭时：Failed/Cancelled 的 gid 从 daemon 的 stopped
// 结果列表移除（aria2.removeDownloadResult，仅对已停止记录有效），save-session
// 便不再把它们写进会话文件；下次启动 --input-file 无从重载重开。开启时直接返回，
// 保留失败记录让 aria2 重载自动续传（= 自动重试）。tasks_ 快照在锁内取、RPC 在
// 锁外发（与 recoverSession 持锁发 RPC 的用法一致，锁序 daemon → tasks 不变）。
void Aria2Engine::purgeFailedBeforeSessionSave(int port,
                                               const std::string& secret) const {
    if (port == 0 || cfg::autoRetryFailed()) return;
    std::vector<std::string> gids;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        for (const auto& task : tasks_) {
            if ((task->state == State::Failed || task->state == State::Cancelled) &&
                !task->gid.empty()) {
                gids.push_back(task->gid);
            }
        }
    }
    for (const auto& gid : gids) {
        try {
            rpcCall(port, secret, "aria2.removeDownloadResult",
                    nlohmann::json::array({gid}));
        } catch (...) {}
    }
}

void Aria2Engine::shutdown() {
    // Finish the engine-owned silent download worker before stopping the daemon.
    // It may be waiting in ensureDaemon(), so do not hold daemonMutex_ here;
    // stopping it while the daemon is still up also lets its cleanup guard remove
    // the temporary silent download before saveSession runs below.
    {
        std::lock_guard lock(downloadThreadMutex_);
        downloadShutdown_ = true;
        if (downloadThread_.joinable()) {
            downloadThread_.request_stop();
            downloadThread_.join();
        }
    }

    // 先停命令队列（必须在取 daemonMutex_ 之前 join：worker 若正在 retryOnWorker
    // 里调 ensureDaemon 会等 daemonMutex_，此时持锁 join 会死锁）。置位后 worker
    // 丢弃未处理命令并退出；当前命令执行完才返回。
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        cmdShutdown_ = true;
    }
    cmdCv_.notify_all();
    if (cmdThread_.joinable()) cmdThread_.join();

    // 与可能仍在跑的 warmup 后台线程互斥，优雅停止 daemon + join WS 线程。
    {
        std::lock_guard<std::mutex> daemonLock(daemonMutex_);
        stopDaemonLocked(/*saveSession=*/true);
        std::lock_guard<std::mutex> lock(tasksMutex_);
        tasks_.clear();
    }

    if (sessionMode_ == SessionMode::Isolated && !isolatedStateDir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(isolatedStateDir_, ec);
    }
}

std::filesystem::path Aria2Engine::makeUniqueDest(const std::filesystem::path& dest) const {
    std::vector<std::filesystem::path> reserved;
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        reserved.reserve(tasks_.size());
        for (const auto& task : tasks_) {
            reserved.push_back(task->destPath);
        }
    }

    auto taken = [&](const std::filesystem::path& candidate) {
        return std::filesystem::exists(candidate) ||
               std::ranges::count(reserved, candidate) > 0;
    };
    if (!taken(dest)) return dest;

    const std::filesystem::path parent = dest.parent_path();
    // stem/ext 拼接按宽字符做（Windows 窄串会走 ANSI 代码页，中文名抛异常）。
    const std::wstring stem = dest.stem().wstring();
    const std::wstring ext = dest.extension().wstring();
    for (int i = 1;; ++i) {
        const auto candidate =
            parent / (stem + L" (" + std::to_wstring(i) + L")" + ext);
        if (!taken(candidate)) return candidate;
    }
}

} // namespace dl
