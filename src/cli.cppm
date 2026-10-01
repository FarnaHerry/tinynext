// cli.cppm — command-line entry: URL add, single-instance detection, and the
// CLI control plane (status / list / pause / ... for users & AI agents).
//
// The app owns a per-user single-instance lock (Windows named mutex, POSIX
// flock). A second launch forwards its URL args to the running instance over a
// TCP loopback socket (event-driven: the primary's background thread blocks on
// accept, so it suspends when idle) and exits; if no instance is running, this
// process becomes the primary and adds its own CLI URLs at first compose. The
// old inbox-file path (temp/tinynext.inbox) is kept as a fallback when the
// socket isn't up yet (e.g. the primary is still starting).
//
// Loopback protocol v2（横幅 TINYNEXT-CLI/2）：
//   * URL 行（含 "mirror:" 编码）：fire-and-forget，客户端发完即关（v1 语义不变，
//     服务端对 /1 前缀横幅仍兼容校验）。
//   * 控制请求：客户端发一行 "ctl:<verb> [args...]\n" 后半关写侧（SHUT_WR），
//     服务端执行完毕把响应写回同一连接再关闭——请求/响应一次一连接。只读动词
//     （status/list/watch）在监听线程直接执行（snapshot/refreshHealth 线程安全，
//     窗口缩进托盘也能答）；变更动词 marshal 到 UI 线程（store 约定：任务命令
//     UI 线程发起，与卡片操作同路径），10s 无应答即报超时。命令语义与输出格式
//     在 tinynext.cli_control，本模块只管收发。
//   * 控制动词在 CliBoot 抢锁之前接管：绝不因查询/操作起 GUI，应用没在跑直接
//     报 "not running"（退出码 1）。
module;

#ifdef _WIN32
// winsock2.h 必须在 windows.h 之前（LEAN_AND_MEAN 会把 winsock.h 排除，但我们
// 直接用 winsock2 的 socket API 做 CLI 转发）。
#include <winsock2.h>
#include <ws2tcpip.h>  // inet_pton
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "native_resource.hpp"
#else
#include <sys/file.h>    // flock
#include <sys/socket.h>  // socket / bind / listen / accept / recv / send
#include <netinet/in.h>  // sockaddr_in
#include <arpa/inet.h>   // inet_pton
#include <fcntl.h>       // open, O_CREAT/O_RDWR
#include <unistd.h>      // close
#include "native_resource.hpp"
#include <cerrno>        // errno / EINTR（accept 失败重试；macOS 不显式引入会报错）
#endif

// eui 的 UI 唤醒：后台线程收到转发 URL 时调用，让主循环跑一帧（跨线程安全，
// eui 的 network 线程也这么用）。
namespace core::platform { void requestUiUpdate(); }

export module tinynext.cli;

import std;
import nlohmann.json;              // watch --until-idle 解析 status JSON
import tinynext.i18n;             // tr / trf（CLI 下载提示按用户语言）
import tinynext.store.tasks;      // g_tasks.startFromUrl（下载流程唯一入口）
import tinynext.store.ui;         // showStatus（转发/CLI 添加下载的结果提示）
import tinynext.utils;            // isDownloadableSource（下载源白名单）
import tinynext.download_engine;  // dl::StartOptions（--mirror 的多源任务）
import tinynext.headless;         // --headless 脚本模式（CliBoot 在 main 前接管）
import tinynext.cli_control;      // 控制面命令执行与输出格式

namespace cli {

namespace {

std::filesystem::path inboxPath() {
    return std::filesystem::temp_directory_path() / "tinynext.inbox";
}

// CLI 转发的 TCP loopback 监听端口文件（主实例启动时写入，第二实例转发时读取）。
std::filesystem::path portPath() {
    return std::filesystem::temp_directory_path() / "tinynext.port";
}

// 跨平台 fd / SOCKET 关闭与无效值。
#ifdef _WIN32
using CliFd = SOCKET;
constexpr CliFd kCliInvalidFd = INVALID_SOCKET;
inline void closeFd(CliFd fd) { ::closesocket(fd); }
#else
using CliFd = int;
constexpr CliFd kCliInvalidFd = -1;
inline void closeFd(CliFd fd) { ::close(fd); }
#endif

class CliSocket {
public:
    CliSocket() = default;
    explicit CliSocket(CliFd fd) noexcept : fd_(fd) {
#ifndef _WIN32
        // aria2/restart child processes must not inherit CLI sockets or keep the
        // single-instance listener alive after the parent exits.
        if (fd_ != kCliInvalidFd) {
            const int flags = ::fcntl(fd_, F_GETFD);
            if (flags < 0 || ::fcntl(fd_, F_SETFD, flags | FD_CLOEXEC) < 0) {
                closeFd(fd_);
                fd_ = kCliInvalidFd;
            }
        }
#endif
    }
    ~CliSocket() { reset(); }
    CliSocket(const CliSocket&) = delete;
    CliSocket& operator=(const CliSocket&) = delete;
    CliSocket(CliSocket&& other) noexcept : fd_(other.release()) {}
    CliSocket& operator=(CliSocket&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    explicit operator bool() const noexcept { return fd_ != kCliInvalidFd; }
    CliFd get() const noexcept { return fd_; }
    CliFd release() noexcept {
        const CliFd fd = fd_;
        fd_ = kCliInvalidFd;
        return fd;
    }
    void reset(CliFd fd = kCliInvalidFd) noexcept {
        if (fd_ != kCliInvalidFd) closeFd(fd_);
        fd_ = fd;
    }

private:
    CliFd fd_ = kCliInvalidFd;
};

// 后台监听线程收到的转发 URL 队列（mutex 保护；UI 线程 drain）。
std::mutex g_urlsMutex;
std::vector<std::string> g_pendingUrls;

// v2 控制面的待执行变更命令（监听线程入队，UI 线程 drain 后回填响应）。reply 用
// shared_ptr：监听线程 10s 超时先行返回错误时，UI 稍后回填也不会悬空。
struct PendingControl {
    std::vector<std::string> tokens;
    std::shared_ptr<std::promise<std::string>> reply;
};
std::mutex g_ctlMutex;
std::vector<PendingControl> g_pendingControls;

// CLI 转发握手横幅：主实例 accept 后立刻发，第二实例 connect 后先收并校验。
// 端口文件可能过期（PID 复用 / fd 继承导致别的进程占用该端口），只测 connect
// 成功会把陌生进程当主实例——URL 被吞、进程静默退出（真实踩坑：kill 掉主实例
// 后其 aria2 daemon 子进程继承了监听 socket，新实例转发给它后秒退）。
// v2：新增 "ctl:" 控制请求（带响应）；URL 行语义不变，对 /1 旧实例仍兼容转发。
constexpr std::string_view kCliBanner = "TINYNEXT-CLI/2\n";
constexpr std::string_view kCliBannerPrefix = "TINYNEXT-CLI/";

// 发送带 MSG_NOSIGNAL（POSIX）：对方提前断开时 send 不会 raise SIGPIPE 杀进程。
#ifdef _WIN32
constexpr int kSendFlags = 0;
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

// 控制台输出（agent 帮助 / 控制面响应共用）。Windows 是 GUI 子系统：默认没有
// 有效 stdout 句柄，AttachConsole(ATTACH_PARENT_PROCESS) 挂回拉起它的终端。
void printCliText(std::string_view s) {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == nullptr || hOut == INVALID_HANDLE_VALUE) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (hOut != nullptr && hOut != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hOut, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
    }
#else
    std::cout << s;
    std::cout.flush();
#endif
}

// 全量发送（loopback 小报文理论上一轮询完，仍循环兜底部分写）。
bool sendAll(CliFd fd, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const int n = static_cast<int>(
            ::send(fd, data.data() + off, static_cast<int>(data.size() - off), kSendFlags));
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// 收主实例横幅并解析协议版本。返回：横幅主版本号（1/2...），0 = 无横幅/陌生
// 进程/超时（调用方按「没有可通信的主实例」处理）。
int readBannerVersion(CliFd fd) {
    std::string banner;
    banner.resize(kCliBanner.size());
    std::size_t got = 0;
    while (got < banner.size()) {
        const int n = static_cast<int>(
            ::recv(fd, banner.data() + got, banner.size() - got, 0));
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    banner.resize(got);
    if (!banner.starts_with(kCliBannerPrefix) || banner.size() < kCliBannerPrefix.size() + 1) {
        return 0;
    }
    const char v = banner[kCliBannerPrefix.size()];
    return (v >= '0' && v <= '9') ? v - '0' : 0;
}

void setRecvTimeout(CliFd fd, int ms) {
#ifdef _WIN32
    const DWORD tv = static_cast<DWORD>(ms);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    const timeval tv{.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// 读主实例端口。无效/缺失返回 0。
int primaryPort() {
    int port = 0;
    std::ifstream in(portPath());
    in >> port;
    return (port > 0 && port <= 65535) ? port : 0;
}

#ifdef _WIN32
// WSAStartup/WSACleanup 的 RAII 包裹：作用域结束自动 Cleanup。Winsock 引用计数
// 按进程配对，init 后不 cleanup 会让退出时计数不归零（手动多 return 路径配平
// 又易漏，直接守卫）。
struct WsSession {
    WsSession() { ok_ = ::WSAStartup(MAKEWORD(2, 2), &wsa_) == 0; }
    ~WsSession() { if (ok_) ::WSACleanup(); }
    WsSession(const WsSession&) = delete;
    WsSession& operator=(const WsSession&) = delete;
    explicit operator bool() const { return ok_; }
    WSADATA wsa_{};
    bool ok_ = false;
};
#endif

// 第二实例：把 URL 通过 TCP loopback 直连发到主实例。loopback 上无人监听会立即
// ECONNREFUSED，阻塞 connect 不会卡住。返回是否成功。
bool trySendUrls(const std::vector<std::string>& urls) {
    const int port = primaryPort();
    if (port == 0) return false;
#ifdef _WIN32
    const WsSession wsa;
    if (!wsa) return false;
#endif
    CliSocket fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        return false;
    }
    // 握手：先收横幅校验对方确实是 TinyNext 主实例（端口文件过期时 connect 到的
    // 可能是任何进程）。2s 超时——旧版本主实例没横幅，超时回退 inbox（向后兼容）。
    // URL 转发不限协议小版本（/1 /2 都能收 URL 行）。
    setRecvTimeout(fd.get(), 2000);
    if (readBannerVersion(fd.get()) == 0) return false;
    std::string data;
    for (const auto& u : urls) {
        data += u;
        data += '\n';
    }
    return sendAll(fd.get(), data);
}

// 控制面请求/响应：向主实例发一行 "ctl:..." 并等回包（连接级一问一答）。
// 返回 0 = 成功（response 已填充），1 = 没有可通信的主实例（端口文件过期/
// 陌生进程/无响应），2 = 主实例是 v1 旧版（不支持控制面，需要重启升级）。
int exchangeCtl(const std::string& request, std::string& response) {
    const int port = primaryPort();
    if (port == 0) return 1;
#ifdef _WIN32
    const WsSession wsa;
    if (!wsa) return 1;
#endif
    CliSocket fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd) return 1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        return 1;
    }
    setRecvTimeout(fd.get(), 2000);
    const int ver = readBannerVersion(fd.get());
    if (ver == 0) {  // 超时/陌生进程：端口文件过期（PID 复用 / daemon 继承）
        return 1;
    }
    if (ver < 2) {  // 旧版主实例：不发 ctl 行（会被当 URL 处理报「不支持的源」）
        return 2;
    }
    // 半关写侧：服务端 recv 到 EOF 即开始执行，我们等它的回包。
    if (!sendAll(fd.get(), request)) return 1;
#ifdef _WIN32
    ::shutdown(fd.get(), SD_SEND);
#else
    ::shutdown(fd.get(), SHUT_WR);
#endif
    setRecvTimeout(fd.get(), 15000);  // 服务端内部还有 10s UI 等待，这里留余量
    response.clear();
    char buf[2048];
    for (;;) {
        const int n = static_cast<int>(::recv(fd.get(), buf, sizeof(buf), 0));
        if (n <= 0) break;  // EOF / 超时都收束（超时留下已读部分由调用方判空）
        response.append(buf, static_cast<std::size_t>(n));
    }
    return response.empty() ? 1 : 0;
}

// 执行一条控制请求（监听线程调用）。只读动词（status/list/watch）直接执行：
// snapshot/refreshHealth 线程安全（housekeep 同款用法），窗口缩进托盘时也能应答。
// 变更动词 marshal 到 UI 线程执行（store 约定：任务命令在 UI 线程发起，与卡片
// 操作同一条路径）；UI 沉睡/托盘隐藏时 10s 超时报明确错误。
std::string runControl(const std::vector<std::string>& tokens) {
    if (cli_control::isReadOnlyVerb(tokens[0])) return cli_control::handle(tokens);
    auto reply = std::make_shared<std::promise<std::string>>();
    std::future<std::string> fu = reply->get_future();
    {
        std::lock_guard<std::mutex> lock(g_ctlMutex);
        g_pendingControls.push_back({tokens, reply});
    }
    core::platform::requestUiUpdate();
    if (fu.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        return "error: TinyNext did not answer within 10s — its window may be hidden "
               "to the tray; restore it and retry\n";
    }
    try {
        return fu.get();
    } catch (...) {
        return "error: TinyNext dropped the request\n";
    }
}

// 主实例：后台线程阻塞在 accept 上（队列空就挂起），收到转发 URL 后入队并唤醒
// UI 线程处理。TCP loopback，端口系统分配后写进端口文件供第二实例发现。
// g_listenFd 暴露给 atexit：退出时 shutdown 唤醒 accept 让线程退出（可 join）。
std::atomic<CliFd> g_listenFd{kCliInvalidFd};
std::thread g_listenerThread;

void cliListenerLoop() {
#ifdef _WIN32
    const WsSession wsa;
    if (!wsa) return;
#endif
    CliSocket listenFd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!listenFd) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);  // 系统分配端口
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(listenFd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listenFd.get(), 8) != 0) {
        return;
    }
    g_listenFd.store(listenFd.get());
    sockaddr_in got{};
#ifdef _WIN32
    int len = static_cast<int>(sizeof(got));
#else
    socklen_t len = sizeof(got);
#endif
    ::getsockname(listenFd.get(), reinterpret_cast<sockaddr*>(&got), &len);
    std::ofstream(portPath(), std::ios::trunc) << ntohs(got.sin_port);

    // 线程退出前统一关监听 socket（两处 return 共用；atexit 里 shutdown 只负责
    // 唤醒 accept，释放归本函数）。
    const auto closeListen = [&] {
        g_listenFd.store(kCliInvalidFd);
        listenFd.reset();
    };
    for (;;) {
        CliSocket client(::accept(listenFd.get(), nullptr, nullptr));
        if (!client) {
            if (g_appExiting.load()) { closeListen(); return; }
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // 握手横幅先发（trySendUrls 校验用），再读到对方关闭（URL 转发全关 /
        // 控制请求半关写侧，都在这一步收束）。
        ::send(client.get(), kCliBanner.data(), static_cast<int>(kCliBanner.size()),
               kSendFlags);
        std::string data;
        char buf[1024];
        for (;;) {
            const int n = static_cast<int>(::recv(client.get(), buf, sizeof(buf), 0));
            if (n <= 0) break;
            data.append(buf, static_cast<std::size_t>(n));
        }
        std::vector<std::string> lines;
        std::istringstream ss(data);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty()) lines.push_back(std::move(line));
        }
        // v2 控制请求：单行 "ctl:<verb> [args...]"，执行后把响应写回本连接。
        if (!lines.empty() && lines[0].starts_with("ctl:")) {
            if (g_appExiting.load()) { closeListen(); return; }
            std::vector<std::string> tokens;
            std::istringstream cs(lines[0].substr(4));
            std::string tok;
            while (cs >> tok) tokens.push_back(tok);
            std::string resp = tokens.empty()
                ? std::string("error: empty ctl request\n")
                : runControl(tokens);
            if (!resp.ends_with('\n')) resp += '\n';
            sendAll(client.get(), resp);
            continue;
        }
        if (g_appExiting.load()) { closeListen(); return; }
        if (!lines.empty()) {
            std::lock_guard<std::mutex> lock(g_urlsMutex);
            g_pendingUrls.insert(g_pendingUrls.end(), lines.begin(), lines.end());
            core::platform::requestUiUpdate();
        }
    }
}

} // namespace

// 命令行参数解析在 tinynext.utils::commandLineArgs（headless 与 CLI 共用同一份）：
// Linux 下经动态加载器启动时（run.sh）程序路径不在 argv[0]，见该函数注释。

// Command-line arguments that look like download sources (http(s)/sftp/ED2K
// URLs, magnet:, or a local .torrent path), in order. Parsed once and cached.
// Note: magnet was filtered out here before (a bug) — a second instance passing
// a magnet URL must forward it to the primary just like http(s).
export std::vector<std::string> commandLineUrls() {
    static const std::vector<std::string> cached = [] {
        auto args = commandLineArgs();
        std::erase_if(args, [](const std::string& a) {
            return !(isDownloadableSource(a) || a.ends_with(".torrent"));
        });
        return args;
    }();
    return cached;
}

// `tinynext --mirror url1 url2 ...`：把所有 URL 合并为一个多源任务（首 URL 为主、
// 其余为镜像源，aria2 多源并发分段下载同一文件）。静态缓存一次解析。
export bool commandLineMirrorMode() {
    static const bool cached = [] {
        for (const auto& a : commandLineArgs()) {
            if (a == "--mirror") return true;
        }
        return false;
    }();
    return cached;
}

// `tinynext --restart`：设置页「立即重启」（platform::restartApp）拉起的替换实例
// 的内部标记。抢单实例锁时重试等待旧实例退出（引擎 shutdown 可能数秒），而不是
// 一次失败就转发退出。不是下载源，commandLineUrls/isDownloadableSource 已过滤。
export bool commandLineRestartMode() {
    static const bool cached = [] {
        for (const auto& a : commandLineArgs()) {
            if (a == "--restart") return true;
        }
        return false;
    }();
    return cached;
}

namespace {

// 镜像只能合并且只能合并普通 URL（magnet / .torrent 没有"多源"概念）。
bool isMirrorableUrl(const std::string& u) {
    return isMirrorableSource(u);
}

} // namespace

// 要处理/转发的下载行：--mirror 且 ≥2 个普通 URL 时编成单行
// "mirror:<主URL> <镜像1> <镜像2> ..."（URL 不含空格，空格分隔安全，单行走
// socket/inbox 都不会被拆开）；否则每个 URL 一行（原行为）。
export std::vector<std::string> downloadLines() {
    auto urls = commandLineUrls();
    if (!commandLineMirrorMode() || urls.size() < 2) return urls;
    for (const auto& u : urls) {
        if (!isMirrorableUrl(u)) return urls;  // 混了 magnet/种子：退回逐条任务
    }
    std::string line = "mirror:";
    for (const auto& u : urls) {
        if (line.size() > 7) line += ' ';
        line += u;
    }
    return {line};
}

// `tinynext agent` —— 打印给 AI 的 CLI 使用教学并退出（不进 GUI、不走单实例）。
// 返回 true 表示已输出、调用方应退出进程。Windows 是 GUI 子系统，用
// AttachConsole + WriteFile 写父进程控制台 / 继承的 stdout 句柄。
export bool runAgentHelpIfRequested() {
    const auto args = commandLineArgs();
    if (args.empty()) return false;
    const std::string& first = args.front();
    if (first != "agent" && first != "--agent" && first != "help" &&
        first != "--help" && first != "-h") {
        return false;
    }

    constexpr const char* kHelp = R"(TinyNext — a single-instance GUI downloader with a scriptable CLI.

ADD DOWNLOADS (the GUI auto-starts when it is not running)
  tinynext <url> [more-urls...]          Add download(s); one task per source.
  tinynext add <url> ...                 Same thing ("add" is an optional word).
  tinynext --mirror <url1> <url2> [...]  One task, many sources: url1 is primary, the rest
                                         are mirrors of the SAME file (aria2 splits across
                                         sources, auto-failover). Plain http(s)/sftp
                                         links only (no magnet / .torrent).
  tinynext --headless <url> [...]        Script mode: NO window, TinyNext's own config
                                         (dir / connections) applies, exits 0 when all
                                         downloads finished, 1 on any failure.
  Accepted sources: http:// https:// sftp:// ed2k://|file| magnet:, local .torrent paths.
  Other arguments are ignored. http is used as-is (not upgraded to https). Files land in
  the configured download directory; names come from the URL / torrent / magnet metadata.

QUERY STATE (read-only; needs a running TinyNext — they never open a window)
  tinynext status [--json]               App version/pid, engine health, task counts, speed.
                                         Counts include tasks paused for a GUI choice
                                         (torrent files / media tracks), see below.
  tinynext list [--json] [--state active|done|failed|all]
                                         One line per task: id, state, progress, speed,
                                         size, name. Default filter: all. Tasks waiting for
                                         a GUI choice are marked "# awaiting selection".
  tinynext watch [--json] [--until-idle] [--interval <sec>]
                                         Print status every <sec> (default 2) until Ctrl-C.
                                         --until-idle: machine mode (one compact JSON per
                                         line), exits 0 once nothing is queued/downloading.

OPERATE THE RUNNING APP (task ids come from `tinynext list`)
  tinynext pause <id...> | pause all     Pause active task(s) (they keep the partial file).
  tinynext resume <id...> | resume all   Resume paused task(s). Refused (exit 2) when the
                                         task waits for a torrent-file / media-track
                                         choice — pick it in the GUI first.
  tinynext cancel <id...>                Stop a task; record + partial file stay, retryable.
  tinynext retry <id...>                 Retry failed/cancelled tasks using aria2-next's
                                         persistent state and original task GID.
  tinynext remove <id...>                Delete a task record (files on disk are NOT touched).
  tinynext clear done                    Delete all completed records.
  tinynext quit                          Save session and exit the app.

FOR AI AGENTS
  - Machine output: pass --json to status / list / watch and to the operating commands
    (results arrive as {"ok":bool,"results":[{"action","ok","message"}...]}). Text output
    marks failures with lines starting "error: ".
  - Exit codes: 0 success · 1 TinyNext not running / unreachable / too old · 2 command
    error (bad id, wrong state, ...).
  - Add, then track:  tinynext https://example.com/big.zip
                      tinynext list --json          (find the task id)
                      tinynext watch --until-idle   (blocks until downloads are done)
  - Control commands never launch the app: if it may be closed, add a download first
    (`tinynext <url>` auto-launches) or start TinyNext normally.
  - A TinyNext window minimized to the system tray answers status / list / watch, but
    task operations (pause / resume / retry / remove / quit) only run while its UI loop
    is alive — restore the window and retry. On Windows, forwarding a download
    automatically restores the window first.
  - Per-user state you may inspect: config <configDir>/tinynext.conf, aria2 session
    <configDir>/tinynext.session, engine log <configDir>/tinynext-aria2.log.
    <configDir> = Windows %APPDATA%\TinyNext · macOS ~/Library/Application Support/TinyNext
    · Linux $XDG_CONFIG_HOME/tinynext (fallback ~/.config/tinynext). A portable
    tinynext.conf next to the exe overrides the location.
  - Single-instance internals: lock <temp>/tinynext.lock (POSIX flock; Windows named
    mutex), forwarding over TCP 127.0.0.1 (port in <temp>/tinynext.port, protocol
    TINYNEXT-CLI/2, control requests are "ctl:<verb> ..." lines with a reply), plus the
    <temp>/tinynext.inbox fallback file for early adds.

TROUBLESHOOTING
  - A download did not start: the argument must start with a recognized scheme (above).
  - "TinyNext is not running" while a window is visible: the CLI and the app run as
    different users / sessions — the lock and port file are per-user.
  - --headless conflicts nothing: it spawns its own engine and never touches the GUI.
  - More docs: README.md ("使用本应用") and docs/cli.md in the repository.
)";

    printCliText(kHelp);
    return true;
}

// Try to become the primary instance. A module-level RAII owner keeps the lock
// for the process lifetime and releases its mutex handle / flock fd at teardown.
namespace {

// 锁状态（模块级、非 static 函数内缓存）：--restart 重试成功后要能把结果回写，
// 函数内 static const 写不回。
bool g_lockAttempted = false;
bool g_primaryInstance = false;
#ifdef _WIN32
tinynext::native::UniqueHandle g_singleInstanceMutex;
#else
tinynext::native::UniqueFd g_singleInstanceFd;
#endif

// 单次尝试抢锁（非缓存）：成功时转交给模块级 RAII owner；失败时自动清理本次资源。
bool tryAcquireLockOnce() {
#ifdef _WIN32
    // "Local\" scope: only the same logged-in session sees it.
    tinynext::native::UniqueHandle mutex(
        CreateMutexW(nullptr, FALSE, L"Local\\TinyNext_SingleInstance"));
    if (!mutex) return true;  // 创建失败按主实例继续，别把应用挡在门外
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return false;
    }
    g_singleInstanceMutex = std::move(mutex);
    return true;
#else
    const std::filesystem::path lockPath =
        std::filesystem::temp_directory_path() / "tinynext.lock";
    tinynext::native::UniqueFd fd(
        ::open(lockPath.string().c_str(), O_CREAT | O_RDWR, 0600));
    if (!fd) return true;
    // 不遗传给子进程：否则主实例被杀后，继承了该 fd 的 aria2 daemon 仍持有
    // flock，新实例 acquireSingleInstance 永远失败 → 静默退出、窗口起不来。
    ::fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
    if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) return false;
    g_singleInstanceFd = std::move(fd);
    return true;  // module-level owner keeps the lock until static teardown
#endif
}

} // namespace

export bool acquireSingleInstance() {
    if (!g_lockAttempted) {
        g_lockAttempted = true;
        g_primaryInstance = tryAcquireLockOnce();
    }
    return g_primaryInstance;
}

// --restart（设置页「立即重启」）：旧实例退出前要跑引擎 shutdown（saveSession +
// forceShutdown，可能耗时数秒），锁要等它进程完全退出才释放。新实例因此轮询
// 重试抢锁（默认 50×200ms=10s），拿到即升级为主实例；超时仍拿不到才退回
// 转发并退出（此时旧实例大概率卡死，静默退出比闪双窗口好）。
export bool acquireSingleInstanceWithRetry(int maxAttempts = 50, int intervalMs = 200) {
    if (acquireSingleInstance()) return true;
    for (int i = 0; i < maxAttempts; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        if (tryAcquireLockOnce()) {
            g_primaryInstance = true;
            return true;
        }
    }
    return false;
}

// Best-effort hand-off to a running primary instance. 首选 TCP loopback socket
// 直连（事件驱动，主实例收到即处理）；socket 未就绪（主实例还在启动）时回退写
// inbox 文件，主实例下次唤醒会 drain。
//
// 即使没有任何 URL（用户只是重新点开 app），也要把主实例窗口带回来（仅 Windows）：
// - 窗口可见/最小化 → SetForegroundWindow 前置即可；
// - 窗口已缩到托盘（close_to_tray：eui glfwHideWindow 隐藏，主循环停在 hiddenToTray
//   分支，不渲染也不跑 compose，转发的 URL 会积压）→ SetForegroundWindow 无效，必须
//   触发主实例的托盘「显示」。做法是给 eui 的托盘 message-only 窗口发
//   WM_COMMAND + Show 菜单项 ID：主实例 pollTray 消费 g_show_requested 后走
//   restoreWindowFromTray（glfwRestore + glfwShow + glfwFocus），随即恢复渲染并
//   drain 积压的转发 URL。eui-neo 已锁定 0.5.6，托盘窗口类名 "TRAY" 与首项 Show 的
//   ID_TRAY_FIRST=1000 见 EUI-NEO 0.5.6 的 3rd/tray（TRAY_WINAPI）。
export void forwardToRunningInstance(const std::vector<std::string>& urls) {
    if (!urls.empty()) {
        if (!trySendUrls(urls)) {
            std::ofstream out(inboxPath(), std::ios::app);
            if (out) {
                for (const auto& u : urls) {
                    if (!u.empty()) out << u << '\n';
                }
            }
        }
    }
#ifdef _WIN32
    using FindWindowFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);
    using SetForegroundFn = BOOL(WINAPI*)(HWND);
    using IsVisibleFn = BOOL(WINAPI*)(HWND);
    using PostMessageFn = BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
    static const tinynext::native::UniqueModule user32(LoadLibraryW(L"user32.dll"));
    static const FindWindowFn findWindow = [&]() -> FindWindowFn {
        if (!user32) return nullptr;
        return reinterpret_cast<FindWindowFn>(
            reinterpret_cast<void*>(GetProcAddress(user32.get(), "FindWindowW")));
    }();
    static const SetForegroundFn setForeground = [&]() -> SetForegroundFn {
        if (!user32) return nullptr;
        return reinterpret_cast<SetForegroundFn>(
            reinterpret_cast<void*>(GetProcAddress(user32.get(), "SetForegroundWindow")));
    }();
    static const IsVisibleFn isVisible = [&]() -> IsVisibleFn {
        if (!user32) return nullptr;
        return reinterpret_cast<IsVisibleFn>(
            reinterpret_cast<void*>(GetProcAddress(user32.get(), "IsWindowVisible")));
    }();
    static const PostMessageFn postMessage = [&]() -> PostMessageFn {
        if (!user32) return nullptr;
        return reinterpret_cast<PostMessageFn>(
            reinterpret_cast<void*>(GetProcAddress(user32.get(), "PostMessageW")));
    }();
    if (findWindow && setForeground && isVisible && postMessage) {
        if (HWND h = findWindow(nullptr, L"TinyNext 下载器")) {
            // 主窗口被托盘隐藏（不可见）时，窗口必然伴随托盘（eui 只有
            // hideWindowToTray 会 glfwHideWindow，而它要求 trayAvailable）。隐藏 ⟺
            // 托盘窗口存在，所以这里能找到 "TRAY" 类窗口就触发恢复；找不到说明没缩
            // 托盘，仅 SetForegroundWindow 前置即可。
            if (!isVisible(h)) {
                // eui 3rd/tray (TRAY_WINAPI)：托盘窗口类名 "TRAY"，菜单
                // {"Show","-","Exit"} 首项 id = ID_TRAY_FIRST = 1000。给托盘窗口
                // PostMessage WM_COMMAND 会走 _tray_wnd_proc → eui_tray_show →
                // g_show_requested，主实例下一轮 pollTray 消费并 restoreWindowFromTray。
                if (HWND tray = findWindow(L"TRAY", nullptr)) {
                    postMessage(tray, WM_COMMAND, 1000 /* ID_TRAY_FIRST: Show */, 0);
                }
            }
            setForeground(h);
        }
    }
#endif
}

// Read and clear the inbox; returns any URLs queued by other instances.
export std::vector<std::string> drainInbox() {
    const std::filesystem::path path = inboxPath();
    std::vector<std::string> urls;
    {
        std::ifstream in(path);
        std::string line;
        while (in && std::getline(in, line)) {
            if (!line.empty()) urls.push_back(line);
        }
    }
    // 截断清空；两个实例并发追加时可能丢一条，但 CLI 场景可接受。
    std::ofstream(path, std::ios::trunc).close();
    return urls;
}

// ---- 控制面客户端（`tinynext status/list/watch/pause/...`）----

bool hasToken(const std::vector<std::string>& tokens, std::string_view flag) {
    for (const auto& t : tokens) {
        if (t == flag) return true;
    }
    return false;
}

// 统一处理 exchangeCtl 结果：打印响应并映射退出码（约定见 cli_control 头注释 /
// agent 帮助）：没在跑 / 旧版 / 无响应 → 1；命令错误（任一 "error:" 行或 JSON
// 顶层 "ok": false）→ 2；否则 0。
int ctlExitCode(int rc, const std::string& response, bool jsonMode) {
    if (rc == 1) {
        printCliText("TinyNext is not running.\n"
                     "Start it (tinynext <url> auto-launches it), then retry.\n");
        return 1;
    }
    if (rc == 2) {
        printCliText("TinyNext is running, but that instance predates CLI control — "
                     "restart the app to a newer version.\n");
        return 1;
    }
    if (rc != 0 || response.empty()) {
        printCliText("error: the running TinyNext instance did not respond\n");
        return 1;
    }
    printCliText(response);
    if (jsonMode) {
        try {
            const auto j = nlohmann::json::parse(response);
            return j.value("ok", true) ? 0 : 2;
        } catch (...) {
            return 0;  // 服务端没按 JSON 回：按文本规则再判一遍
        }
    }
    std::istringstream ss(response);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.starts_with("error:")) return 2;
    }
    return 0;
}

// `tinynext watch`：每 interval 秒取一次 status 输出，Ctrl-C 停止。
// --until-idle 强制机器模式（每行一条紧凑 JSON），队列为空且无下载中即退出 0
// （Paused 是用户主动暂停，不算「未完成」）。--json（不带 --until-idle）透传
// status 的 JSON 块。
int runWatch(const std::vector<std::string>& tokens) {
    const bool jsonMode = hasToken(tokens, "--json");
    const bool untilIdle = hasToken(tokens, "--until-idle");
    double interval = 2.0;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == "--interval") {
            try {
                interval = std::clamp(std::stod(tokens[i + 1]), 0.2, 3600.0);
            } catch (...) {}
        }
    }
    const std::string request = (jsonMode || untilIdle)
        ? std::string("ctl:status --json\n")
        : std::string("ctl:status\n");
    for (;;) {
        std::string response;
        const int rc = exchangeCtl(request, response);
        if (rc != 0 || response.empty()) return ctlExitCode(rc, response, jsonMode || untilIdle);
        if (untilIdle) {
            try {
                const auto j = nlohmann::json::parse(response);
                printCliText(j.dump() + "\n");
                const int active = j["tasks"].value("queued", 0) +
                                   j["tasks"].value("downloading", 0);
                if (active == 0) return j.value("ok", true) ? 0 : 2;
            } catch (...) {
                printCliText("error: malformed status response from TinyNext\n");
                return 1;
            }
        } else {
            printCliText(response);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int>(interval * 1000)));
    }
}

// 首参数是控制动词 → 接管进程并永不返回（内部 std::exit）。控制面只与运行中的
// 实例通信：不抢单实例锁、不起 GUI——应用没在跑直接报 "not running" 退出。
// 在 CliBoot 抢锁之前调用。
export void runControlIfRequested() {
    const auto args = commandLineArgs();
    if (args.empty() || !cli_control::isControlVerb(args[0])) return;
    const std::string verb = args[0];
    std::vector<std::string> tokens(args.begin() + 1, args.end());
    if (verb == "watch") std::exit(runWatch(tokens));
    std::string request = "ctl:" + verb;
    for (const auto& t : tokens) {
        request += ' ';
        request += t;
    }
    request += '\n';
    std::string response;
    std::exit(ctlExitCode(exchangeCtl(request, response), response,
                          hasToken(tokens, "--json")));
}

// ---- 应用级接线（经 tinynext.store.tasks 的下载流程）----

// 单实例：CLI 启动参数是否已添加过（processPendingUrls 首次消费）。模块私有。
bool g_cliHandled = false;

// 单实例引导：静态初始化（main 之前）尝试获取锁。第二实例转发 URL 并退出、
// 不闪窗口；主实例正常继续，CLI URL 由 processPendingUrls 添加到下载列表。
// 模块全局的动态初始化先于任何引用 TU 的静态初始化执行。
struct CliBoot {
    CliBoot() {
        // `tinynext agent`：打印 CLI 使用教学并退出，不进 GUI、不走单实例。
        if (runAgentHelpIfRequested()) {
            std::exit(0);
        }
        // `tinynext --headless <url>`：脚本模式，不开窗、下载完退出（exit 0/1）。
        // 必须在抢单实例锁之前接管——headless 独立起自己的 daemon，不与运行中的
        // GUI 冲突、也不转发 URL。
        if (headless::requested()) {
            std::exit(headless::run());
        }
        // 控制面（status/list/watch/pause/...）：只与运行中的实例通信，绝不抢锁、
        // 绝不起 GUI——app 没在跑就报 "not running" 退出（内部 std::exit，不返回）。
        runControlIfRequested();
        // --restart（设置页「立即重启」拉起的替换实例）：旧实例退出要跑引擎
        // shutdown，锁释放有延迟 → 重试等锁；普通启动一次抢不到即转发退出。
        const bool primary = commandLineRestartMode()
            ? acquireSingleInstanceWithRetry()
            : acquireSingleInstance();
        if (!primary) {
            forwardToRunningInstance(downloadLines());
            std::exit(0);
        }
    }
};
CliBoot g_cliBoot;

// 启动 CLI 转发监听（后台线程，幂等）。阻塞在 accept 上，空闲不占任何资源；
// 收到第二实例转发的 URL 时入队并唤醒 UI 线程。在首次 compose 时调用。
export void startCliIpc() {
    static std::atomic<bool> started = false;
    if (started.exchange(true)) return;
    g_listenerThread = std::thread(cliListenerLoop);
    // atexit 先于静态析构：shutdown 监听 socket 把 accept 唤醒，线程看到
    // g_appExiting 退出后 join，避免退出途中线程仍在往 g_pendingUrls 写。
    std::atexit([] {
        // 与 housekeep 的 atexit 顺序不定，这里也置位，保证 accept 被 shutdown
        // 唤醒后第一轮就看到退出标志（否则空转到 housekeep 的 atexit 才停）。
        g_appExiting.store(true);
        const CliFd fd = g_listenFd.load();
        if (fd != kCliInvalidFd) {
#ifdef _WIN32
            ::shutdown(fd, SD_BOTH);
#else
            ::shutdown(fd, SHUT_RDWR);
#endif
        }
        if (g_listenerThread.joinable()) g_listenerThread.join();
    });
}

// 按行启动下载：普通行 = 单 URL 任务；"mirror:<主URL> <镜像...>" 行 = 多源合一
// 任务（downloadLines 的编码，socket / inbox / 自身 CLI 三路共用）。
// 结果消息走状态条（UI 线程调用，与弹窗添加一致）。
void startFromLines(const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        if (line.starts_with("mirror:")) {
            std::vector<std::string> parts;
            std::istringstream ss(line.substr(7));
            std::string tok;
            while (ss >> tok) parts.push_back(tok);
            if (parts.size() >= 2) {
                dl::StartOptions opts;
                opts.mirrors.assign(parts.begin() + 1, parts.end());
                showStatus(g_tasks.startFromUrl(parts[0], opts).message);
            } else if (!parts.empty()) {
                showStatus(g_tasks.startFromUrl(parts[0], 0).message);
            }
            continue;
        }
        showStatus(g_tasks.startFromUrl(line, 0).message);
    }
}

// UI 线程在每次被唤醒时调用（compose 顶部）：首帧加自身命令行 URL，随后处理
// socket 转发的 URL，并兜底 drain inbox 文件（旧版本第二实例 / socket 未就绪时）。
export void processPendingUrls() {
    if (!g_cliHandled) {
        g_cliHandled = true;
        startFromLines(downloadLines());
    }
    std::vector<std::string> urls;
    {
        std::lock_guard<std::mutex> lock(g_urlsMutex);
        urls.swap(g_pendingUrls);
    }
    startFromLines(urls);
    if (std::filesystem::exists(inboxPath())) {
        startFromLines(drainInbox());
    }
    // 控制面变更命令（UI 线程执行，见 cli.cppm 头注释的线程约定）：跑完回填
    // 响应，监听线程把结果发回 CLI 客户端。quit 在回填后 std::exit(0)——
    // atexit 处理器会 shutdown 监听 socket 并 join 监听/housekeep 线程（响应
    // 已先发出），随后静态析构链（g_tasks → 引擎 saveSession + forceShutdown）
    // 与关窗退出 / restartApp 同一条路，会话不丢。
    std::vector<PendingControl> ctrls;
    {
        std::lock_guard<std::mutex> lock(g_ctlMutex);
        ctrls.swap(g_pendingControls);
    }
    for (auto& c : ctrls) {
        if (c.reply) c.reply->set_value(cli_control::handle(c.tokens));
        if (!c.tokens.empty() && c.tokens[0] == "quit") std::exit(0);
    }
}

} // namespace cli
