// cli.cppm — command-line download entry + single-instance detection.
//
// The app owns a per-user single-instance lock (Windows named mutex, POSIX
// flock). A second launch forwards its URL args to the running instance over a
// TCP loopback socket (event-driven: the primary's background thread blocks on
// accept, so it suspends when idle) and exits; if no instance is running, this
// process becomes the primary and adds its own CLI URLs at first compose. The
// old inbox-file path (temp/tinynext.inbox) is kept as a fallback when the
// socket isn't up yet (e.g. the primary is still starting).
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
#else
#include <sys/file.h>    // flock
#include <sys/socket.h>  // socket / bind / listen / accept / recv / send
#include <netinet/in.h>  // sockaddr_in
#include <arpa/inet.h>   // inet_pton
#include <fcntl.h>       // open, O_CREAT/O_RDWR
#include <unistd.h>      // close
#include <cerrno>        // errno / EINTR（accept 失败重试；macOS 不显式引入会报错）
#ifdef __APPLE__
#include <crt_externs.h> // _NSGetArgc/_NSGetArgv
#endif
#endif

// eui 的 UI 唤醒：后台线程收到转发 URL 时调用，让主循环跑一帧（跨线程安全，
// eui 的 network 线程也这么用）。
namespace core::platform { void requestUiUpdate(); }

export module tinynext.cli;

import std;
import tinynext.ui.state;
import tinynext.ui.utils;  // isDownloadableSource（下载源白名单，一处维护）

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

// 后台监听线程收到的转发 URL 队列（mutex 保护；UI 线程 drain）。
std::mutex g_urlsMutex;
std::vector<std::string> g_pendingUrls;

// 第二实例：把 URL 通过 TCP loopback 直连发到主实例。loopback 上无人监听会立即
// ECONNREFUSED，阻塞 connect 不会卡住。返回是否成功。
bool trySendUrls(const std::vector<std::string>& urls) {
    int port = 0;
    {
        std::ifstream in(portPath());
        in >> port;
    }
    if (port <= 0 || port > 65535) return false;
#ifdef _WIN32
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
    const CliFd fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kCliInvalidFd) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeFd(fd);
        return false;
    }
    std::string data;
    for (const auto& u : urls) {
        data += u;
        data += '\n';
    }
    ::send(fd, data.data(), static_cast<int>(data.size()), 0);
    closeFd(fd);
    return true;
}

// 主实例：后台线程阻塞在 accept 上（队列空就挂起），收到转发 URL 后入队并唤醒
// UI 线程处理。TCP loopback，端口系统分配后写进端口文件供第二实例发现。
void cliListenerLoop() {
#ifdef _WIN32
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
#endif
    const CliFd listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd == kCliInvalidFd) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);  // 系统分配端口
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(listenFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listenFd, 8) != 0) {
        closeFd(listenFd);
        return;
    }
    sockaddr_in got{};
#ifdef _WIN32
    int len = static_cast<int>(sizeof(got));
#else
    socklen_t len = sizeof(got);
#endif
    ::getsockname(listenFd, reinterpret_cast<sockaddr*>(&got), &len);
    std::ofstream(portPath(), std::ios::trunc) << ntohs(got.sin_port);

    for (;;) {
        const CliFd client = ::accept(listenFd, nullptr, nullptr);
        if (client == kCliInvalidFd) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        std::string data;
        char buf[1024];
        for (;;) {
            const int n = static_cast<int>(::recv(client, buf, sizeof(buf), 0));
            if (n <= 0) break;
            data.append(buf, static_cast<std::size_t>(n));
        }
        closeFd(client);
        std::vector<std::string> urls;
        std::istringstream ss(data);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty()) urls.push_back(std::move(line));
        }
        if (!urls.empty()) {
            {
                std::lock_guard<std::mutex> lock(g_urlsMutex);
                g_pendingUrls.insert(g_pendingUrls.end(), urls.begin(), urls.end());
            }
            core::platform::requestUiUpdate();
        }
    }
}

} // namespace

// All command-line arguments (excluding the exe path), in order. Parsed once
// and cached; safe to call from a static initializer.
export std::vector<std::string> commandLineArgs() {
    static const std::vector<std::string> cached = [] {
        std::vector<std::string> args;
#ifdef _WIN32
        using CmdToArgvFn = LPWSTR*(WINAPI*)(LPCWSTR, int*);
        static const CmdToArgvFn cmdToArgv = []() -> CmdToArgvFn {
            HMODULE m = LoadLibraryW(L"shell32.dll");
            if (!m) return nullptr;
            return reinterpret_cast<CmdToArgvFn>(
                reinterpret_cast<void*>(GetProcAddress(m, "CommandLineToArgvW")));
        }();
        if (cmdToArgv) {
            int argc = 0;
            LPWSTR* wargv = cmdToArgv(GetCommandLineW(), &argc);
            if (wargv) {
                for (int i = 1; i < argc; ++i) {
                    const std::wstring w(wargv[i]);
                    args.push_back(std::string(w.begin(), w.end()));
                }
                LocalFree(static_cast<HLOCAL>(wargv));  // kernel32, always linked
            }
        }
#elif defined(__APPLE__)
        const int argc = *_NSGetArgc();
        char** argv = *_NSGetArgv();
        for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
#else
        // Linux: /proc/self/cmdline (NUL-separated); argv[0] is the exe path.
        std::ifstream in("/proc/self/cmdline", std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
        std::size_t start = 0;
        while (start < s.size()) {
            const std::size_t end = s.find('\0', start);
            const std::string tok = s.substr(start, end - start);
            if (start > 0) args.push_back(tok);  // skip argv[0]
            if (end == std::string::npos) break;
            start = end + 1;
        }
#endif
        return args;
    }();
    return cached;
}

// Command-line arguments that look like download sources (http(s)/ftp(s)/sftp
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

    constexpr const char* kHelp = R"(TinyNext — a single-instance GUI downloader with a small CLI.

USAGE
  tinynext <http(s)-url> [more-urls...]   Add download(s). The GUI auto-starts if needed.
  tinynext agent                          Print this usage guide (what you are reading now).

RULES
  - http://, https://, ftp://, sftp://, ftps:// URLs, magnet: links, and local .torrent
    file paths are treated as downloads; other arguments are ignored.
  - Single-instance: if TinyNext is already running, the sources are forwarded to the
    running instance and this process exits immediately — a new window is NOT opened.
    The running instance adds the tasks itself.
  - http is used as-is (not upgraded to https). Multiple URLs create separate tasks
    (mirror merging is a UI feature in the add dialog).
  - Files land in the configured download directory (default: the system Downloads folder).
  - The filename is taken from the last path segment of the URL.

EXAMPLES
  tinynext https://example.com/file.zip
  tinynext https://a.example.com/x.bin https://b.example.com/y.tar.gz

TROUBLESHOOTING
  - A download did not start: make sure the URL starts with http:// or https://.
  - Forwarding is done via a file at <temp>/tinynext.inbox — check it to confirm the URL was queued.
)";

#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == nullptr || hOut == INVALID_HANDLE_VALUE) {
        AttachConsole(ATTACH_PARENT_PROCESS);  // 拿到父进程控制台（若存在）
        hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (hOut != nullptr && hOut != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hOut, kHelp, static_cast<DWORD>(std::strlen(kHelp)), &written, nullptr);
    }
#else
    std::cout << kHelp;
    std::cout.flush();
#endif
    return true;
}

// Try to become the primary instance. Returns true if this process owns the
// single-instance lock. The lock (mutex / flock fd) is intentionally never
// released — the OS frees it when the process exits, and keeping it open is
// exactly what holds the single-instance guarantee.
export bool acquireSingleInstance() {
    static const bool primary = [] {
#ifdef _WIN32
        // "Local\" scope: only the same logged-in session sees it.
        HANDLE m = CreateMutexW(nullptr, FALSE, L"Local\\TinyNext_SingleInstance");
        if (!m) return true;  // 创建失败按主实例继续，别把应用挡在门外
        return GetLastError() != ERROR_ALREADY_EXISTS;
#else
        std::error_code ec;
        const std::filesystem::path lockPath =
            std::filesystem::temp_directory_path() / "tinynext.lock";
        const int fd = ::open(lockPath.string().c_str(), O_CREAT | O_RDWR, 0600);
        if (fd < 0) return true;
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            return false;  // 已有一份在跑
        }
        (void)ec;
        return true;  // fd 故意不关：持锁到进程结束
#endif
    }();
    return primary;
}

// Best-effort hand-off to a running primary instance. 首选 TCP loopback socket
// 直连（事件驱动，主实例收到即处理）；socket 未就绪（主实例还在启动）时回退写
// inbox 文件，主实例下次唤醒会 drain。最后把已有窗口切到前台（仅 Windows）。
export void forwardToRunningInstance(const std::vector<std::string>& urls) {
    if (urls.empty()) return;
    if (!trySendUrls(urls)) {
        std::ofstream out(inboxPath(), std::ios::app);
        if (out) {
            for (const auto& u : urls) {
                if (!u.empty()) out << u << '\n';
            }
        }
    }
#ifdef _WIN32
    using FindWindowFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);
    using SetForegroundFn = BOOL(WINAPI*)(HWND);
    static const FindWindowFn findWindow = []() -> FindWindowFn {
        HMODULE m = LoadLibraryW(L"user32.dll");
        if (!m) return nullptr;
        return reinterpret_cast<FindWindowFn>(
            reinterpret_cast<void*>(GetProcAddress(m, "FindWindowW")));
    }();
    static const SetForegroundFn setForeground = []() -> SetForegroundFn {
        HMODULE m = LoadLibraryW(L"user32.dll");
        if (!m) return nullptr;
        return reinterpret_cast<SetForegroundFn>(
            reinterpret_cast<void*>(GetProcAddress(m, "SetForegroundWindow")));
    }();
    if (findWindow && setForeground) {
        if (HWND h = findWindow(nullptr, L"TinyNext 下载器")) {
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

// ---- 应用级接线（依赖 tinynext.ui.state 的下载流程）----

// 单实例引导：静态初始化（main 之前）尝试获取锁。第二实例转发 URL 并退出、
// 不闪窗口；主实例正常继续，CLI URL 由 processPendingUrls 添加到下载列表。
// 模块全局的动态初始化先于任何引用 TU 的静态初始化执行。
struct CliBoot {
    CliBoot() {
        // `tinynext agent`：打印 CLI 使用教学并退出，不进 GUI、不走单实例。
        if (runAgentHelpIfRequested()) {
            std::exit(0);
        }
        if (!acquireSingleInstance()) {
            forwardToRunningInstance(commandLineUrls());
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
    std::thread(cliListenerLoop).detach();
}

// UI 线程在每次被唤醒时调用（compose 顶部）：首帧加自身命令行 URL，随后处理
// socket 转发的 URL，并兜底 drain inbox 文件（旧版本第二实例 / socket 未就绪时）。
export void processPendingUrls() {
    if (!g_cliHandled) {
        g_cliHandled = true;
        for (const auto& u : commandLineUrls()) {
            startDownloadFromUrl(u, 0);
        }
    }
    std::vector<std::string> urls;
    {
        std::lock_guard<std::mutex> lock(g_urlsMutex);
        urls.swap(g_pendingUrls);
    }
    for (auto& u : urls) {
        startDownloadFromUrl(std::move(u), 0);
    }
    if (std::filesystem::exists(inboxPath())) {
        for (const auto& u : drainInbox()) {
            startDownloadFromUrl(u, 0);
        }
    }
}

} // namespace cli
