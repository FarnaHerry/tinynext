// cli.cppm — command-line download entry + single-instance detection.
//
// The app owns a per-user single-instance lock (Windows named mutex, POSIX
// flock). A second launch either forwards its URL args to the running instance
// through a small inbox file (temp/tinynext.inbox, one URL per line) and exits,
// or — if no instance is running — becomes the primary and adds its own CLI
// URLs at first compose. The primary polls the inbox periodically.
module;

#ifdef _WIN32
// Windows API for the named mutex / command-line parsing. LEAN_AND_MEAN keeps
// winsock.h out (the app uses winsock2 directly for the aria2 RPC socket).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/file.h>    // flock
#include <fcntl.h>       // open, O_CREAT/O_RDWR
#include <unistd.h>      // close
#ifdef __APPLE__
#include <crt_externs.h> // _NSGetArgc/_NSGetArgv
#endif
#endif

export module tinynext.cli;

import std;
import tinynext.ui.state;

namespace cli {

namespace {

std::filesystem::path inboxPath() {
    return std::filesystem::temp_directory_path() / "tinynext.inbox";
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

// Command-line arguments that look like download URLs (https:// or http://),
// in order. Parsed once and cached.
export std::vector<std::string> commandLineUrls() {
    static const std::vector<std::string> cached = [] {
        auto args = commandLineArgs();
        // Keep only arguments that look like download URLs.
        std::erase_if(args, [](const std::string& a) {
            return !(a.starts_with("https://") || a.starts_with("http://"));
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
  - Only arguments starting with http:// or https:// are treated as downloads; ignore the rest.
  - Single-instance: if TinyNext is already running, the URLs are forwarded to the running
    instance and this process exits immediately — a new window is NOT opened. The running
    instance adds the tasks itself.
  - HTTP, HTTPS and magnet: links are supported; http is used as-is (not upgraded).
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

// Best-effort hand-off to a running primary instance: queue the URLs in the
// inbox and try to raise its window (Windows only; POSIX apps just receive the
// task on the primary's next inbox poll).
export void forwardToRunningInstance(const std::vector<std::string>& urls) {
    if (urls.empty()) return;
    {
        std::ofstream out(inboxPath(), std::ios::app);
        if (!out) return;
        for (const auto& u : urls) {
            if (!u.empty()) out << u << '\n';
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
// 不闪窗口；主实例正常继续，CLI URL 由 handleCliAndInbox 添加到下载列表。
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

// 每帧调用：主实例首次把自身 CLI URL 加进下载列表，之后周期性轮询 inbox
// 取其他实例转发的 URL（~0.5s 一次文件读取）。
export void handleCliAndInbox(float deltaSeconds) {
    if (!g_cliHandled) {
        g_cliHandled = true;
        for (const auto& u : commandLineUrls()) {
            startDownloadFromUrl(u, 0);
        }
    }
    g_inboxTimer += deltaSeconds;
    if (g_inboxTimer >= 0.5f) {
        g_inboxTimer = 0.0f;
        for (const auto& u : drainInbox()) {
            startDownloadFromUrl(u, 0);
        }
    }
}

} // namespace cli
