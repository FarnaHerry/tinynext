// browser_host.cppm — 浏览器扩展 Native Messaging 宿主（先支持 Google Chrome）。
//
// 链路：Chrome 扩展（extension/chrome，MV3）→ chrome.runtime.connectNative →
// Chrome 拉起本程序（argv 带扩展 origin）→ 本模块在 stdio 上按 Chrome Native
// Messaging 协议（u32 长度前缀 + UTF-8 JSON）收发 → 每条 URL 经注入的转发回调
// （cli 的 forwardUrlsFromBrowser：单实例 TCP 直连，失败落 inbox）交给运行中的
// TinyNext；主实例不在时写完 inbox 顺带拉起 GUI 消费。
//
// 宿主模式绝不进 GUI、不抢单实例锁、不起 daemon——它在 CliBoot 最前面接管并
// std::exit，与 --headless / 控制面动词同层。
//
// 注册：registerChromeNativeHost 在 GUI 启动时调用（幂等 best-effort）——把
// Native Messaging 清单写到 <configDir>/browser-host/，再登记到 Chrome：
//   Windows  HKCU\Software\Google\Chrome\NativeMessagingHosts\<name>（默认值=清单路径）
//   Linux    ~/.config/google-chrome/NativeMessagingHosts/<name>.json
//   macOS    ~/Library/Application Support/Google/Chrome/NativeMessagingHosts/<name>.json
//
// 扩展 manifest 带固定 key → 固定扩展 ID（kExtensionOrigin），宿主清单的
// allowed_origins 因此可以预先写死；换扩展 key 必须同步这里。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include "native_resource.hpp"
#else
#include <sys/types.h>
#include <sys/wait.h>    // waitpid（双 fork 收尸中间子进程）
#include <unistd.h>      // fork / setsid / execv / _exit / read / write
#include <fcntl.h>
#include <cerrno>
#ifdef __APPLE__
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif
#include "native_resource.hpp"
#endif

#include "versions.generated.h"  // TINY_APP_VERSION（popup 连接测试展示）

export module tinynext.browser_host;

import std;
import nlohmann.json;
import tinynext.config;
import tinynext.utils;

namespace browser_host {

namespace {

// 扩展 ID 由 manifest.json 的固定 key 派生（SHA256 前 16 字节 → a-p 映射）。
// 与 extension/chrome/manifest.json 的 key 一一对应，换 key 必须同步。
constexpr const char* kExtensionOrigin =
    "chrome-extension://ebhblmhndabomflakjjljpipfolmajia/";
constexpr const char* kHostName = "com.tinynext.host";
// Chrome → 宿主单条消息上限 1MB；留余量做协议错误护栏。
constexpr std::uint32_t kMaxMessageBytes = 4u * 1024u * 1024u;

// ---- 自身可执行文件 / Linux 启动器（与 platform::restartApp 同一候选逻辑：
// mcpp 把 PT_INTERP 指向私有 glibc，部分机器裸 exe 起不来，优先 run.sh）----
// 真实程序路径统一走 utils::programExePath（run.sh 经 ld.so 启动时
// /proc/self/exe 指向加载器本身，必须从 cmdline 还原）。

#ifndef _WIN32
#ifndef __APPLE__
// Linux：exe 旁 run.sh（免安装包）→ 仓库根 run.sh（开发树）→ 系统启动器 → 裸 exe。
std::filesystem::path linuxHostLauncher(const std::filesystem::path& exe) {
    const std::filesystem::path exeDir = exe.parent_path();
    std::error_code ec;
    const std::filesystem::path candidates[] = {
        exeDir / "run.sh",
        (exeDir / "../../../../run.sh").lexically_normal(),
        std::filesystem::path("/usr/bin/tinynext"),
    };
    for (const auto& cand : candidates) {
        if (std::filesystem::exists(cand, ec)) return cand;
    }
    return {};
}
#endif

// 双 fork 拉起 detached 进程：中间子进程被 waitpid 收尸，孙进程 exec 目标并由
// init 收养（宿主进程长生命周期，单 fork 会留僵尸）。fork 与 exec 之间只用
// async-signal-safe 调用。目标固定带 --start-hidden：宿主拉起的 GUI 首帧后
// 直接进托盘（同 CLI 唤起，不弹窗）。
void spawnDetachedUnix(const std::filesystem::path& launcher) {
    const pid_t pid = ::fork();
    if (pid != 0) {
        if (pid > 0) {
            int status = 0;
            pid_t r = 0;
            do {
                r = ::waitpid(pid, &status, 0);
            } while (r < 0 && errno == EINTR);
        }
        return;
    }
    ::setsid();
    const pid_t inner = ::fork();
    if (inner != 0) ::_exit(inner > 0 ? 0 : 127);
    std::string arg0 = launcher.string();
    std::vector<char*> argv;
    argv.push_back(arg0.data());
    argv.push_back(const_cast<char*>("--start-hidden"));
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    ::_exit(127);
}
#endif

// TinyNext 未运行时拉起 GUI（消费刚写入 inbox 的 URL）。
void spawnAppDetached() {
    const std::filesystem::path exe = programExePath();
    if (exe.empty()) return;
#ifdef _WIN32
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = exe.wstring() + L" --start-hidden";
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return;
    }
    // 句柄立刻交 owner 关闭：GUI 子进程自生自灭，父进程不等待。
    tinynext::native::UniqueHandle(pi.hProcess).reset();
    tinynext::native::UniqueHandle(pi.hThread).reset();
#else
#ifdef __APPLE__
    spawnDetachedUnix(exe);
#else
    const std::filesystem::path launcher = linuxHostLauncher(exe);
    spawnDetachedUnix(launcher.empty() ? exe : launcher);
#endif
#endif
}

// ---- Chrome Native Messaging 框架（u32 长度前缀 + JSON；各支持平台均为小端）----

bool readExact(std::FILE* f, unsigned char* buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        const std::size_t r = std::fread(buf + got, 1, n - got, f);
        if (r == 0) return false;
        got += r;
    }
    return true;
}

bool writeAll(std::FILE* f, const unsigned char* buf, std::size_t n) {
    std::size_t put = 0;
    while (put < n) {
        const std::size_t w = std::fwrite(buf + put, 1, n - put, f);
        if (w == 0) return false;
        put += w;
    }
    return true;
}

bool sendReply(const nlohmann::json& reply) {
    const std::string payload = reply.dump();
    if (payload.size() > 64u * 1024u * 1024u) return false;  // Chrome 上限 64MB
    unsigned char header[4] = {
        static_cast<unsigned char>(payload.size() & 0xff),
        static_cast<unsigned char>((payload.size() >> 8) & 0xff),
        static_cast<unsigned char>((payload.size() >> 16) & 0xff),
        static_cast<unsigned char>((payload.size() >> 24) & 0xff),
    };
    return writeAll(stdout, header, 4) &&
           writeAll(stdout,
                    reinterpret_cast<const unsigned char*>(payload.data()),
                    payload.size()) &&
           std::fflush(stdout) == 0;
}

// ---- 宿主清单（Native messaging host manifest）----

// Chrome 启动宿主用的可执行文件路径：Windows/macOS 直接是主程序；Linux 优先
// run.sh（manifest 的 path 允许指向脚本，Chrome 直接 exec 它）。
std::filesystem::path hostExecutableForManifest() {
    const std::filesystem::path exe = programExePath();
    if (exe.empty()) return {};
#if !defined(_WIN32) && !defined(__APPLE__)
    std::error_code ec;
    const std::filesystem::path exeDir = exe.parent_path();
    const std::filesystem::path candidates[] = {
        exeDir / "run.sh",
        (exeDir / "../../../../run.sh").lexically_normal(),
        std::filesystem::path("/usr/bin/tinynext"),
    };
    for (const auto& cand : candidates) {
        if (std::filesystem::exists(cand, ec)) return cand;
    }
#endif
    return exe;
}

nlohmann::json hostManifestJson() {
    nlohmann::json j = nlohmann::json::object();
    j["name"] = kHostName;
    j["description"] = "TinyNext 下载器浏览器扩展宿主";
    j["path"] = utf8FromPath(hostExecutableForManifest());
    j["type"] = "stdio";
    j["allowed_origins"] = nlohmann::json::array({kExtensionOrigin});
    return j;
}

// 清单本体落 <configDir>/browser-host/com.tinynext.host.json，返回落盘路径。
std::filesystem::path writeHostManifest() {
    const std::filesystem::path path =
        cfg::configDir() / "browser-host" / (std::string(kHostName) + ".json");
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return {};
    out << hostManifestJson().dump(2) << '\n';
    out.close();
    return out.good() ? path : std::filesystem::path{};
}

#ifdef _WIN32
// HKCU\Software\Google\Chrome\NativeMessagingHosts\<name> 默认值 = 清单绝对路径。
bool registerManifestInChrome(const std::filesystem::path& manifest) {
    const std::wstring subKey =
        std::wstring(L"Software\\Google\\Chrome\\NativeMessagingHosts\\") +
        std::wstring(kHostName, kHostName + std::char_traits<char>::length(kHostName));
    HKEY raw = nullptr;
    const LONG rc = ::RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0,
                                      nullptr, REG_OPTION_NON_VOLATILE,
                                      KEY_SET_VALUE, nullptr, &raw, nullptr);
    if (rc != ERROR_SUCCESS) return false;
    tinynext::native::UniqueHkey key(raw);
    const std::wstring value = manifest.wstring();
    return ::RegSetValueExW(key.get(), nullptr, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) *
                                               sizeof(wchar_t))) == ERROR_SUCCESS;
}
#else
// Linux/macOS：把清单复制进 Chrome 用户目录（Chrome 只认这两个固定位置）。
bool registerManifestInChrome(const std::filesystem::path& manifest) {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return false;
    std::filesystem::path dir;
#ifdef __APPLE__
    dir = std::filesystem::path(home) / "Library" / "Application Support" /
          "Google" / "Chrome" / "NativeMessagingHosts";
#else
    dir = std::filesystem::path(home) / ".config" / "google-chrome" /
          "NativeMessagingHosts";
#endif
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::copy_file(manifest, dir / manifest.filename(),
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    return !ec;
}
#endif

} // namespace

// 命令行是否为浏览器宿主模式：Chrome 以扩展 origin 作首参拉起宿主，或显式
// --native-host（调试用，协议一致）。
export bool nativeHostRequested() {
    static const bool cached = [] {
        for (const auto& a : commandLineArgs()) {
            if (a == "--native-host") return true;
            if (a.starts_with("chrome-extension://")) return true;
        }
        return false;
    }();
    return cached;
}

// 宿主主循环：stdin 收 Chrome 的长度前缀 JSON，逐条处理并回包；stdin EOF
// （扩展断开 / 浏览器退出）即正常返回。URL 一律先经 forwardUrls（TCP 直连主
// 实例，失败落 inbox）；返回 false 表示主实例不在——此时拉起一次 GUI 消费。
// forwardUrls 由 cli 注入（宿主模块不 import cli，避免模块循环依赖）。
export int runNativeHost(
        std::function<bool(const std::vector<std::string>&)> forwardUrls) {
#ifdef _WIN32
    // GUI 子系统下 Chrome 给 stdin/stdout 挂管道；CRT 默认文本模式会把长度
    // 前缀里的 0x0A 翻译坏，必须切二进制。
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) return 1;
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 1;
#endif
    bool spawnedGui = false;
    for (;;) {
        unsigned char header[4];
        if (!readExact(stdin, header, 4)) return 0;
        const std::uint32_t len = static_cast<std::uint32_t>(header[0]) |
                                  (static_cast<std::uint32_t>(header[1]) << 8) |
                                  (static_cast<std::uint32_t>(header[2]) << 16) |
                                  (static_cast<std::uint32_t>(header[3]) << 24);
        if (len == 0 || len > kMaxMessageBytes) return 1;
        std::string payload(len, '\0');
        if (!readExact(stdin,
                       reinterpret_cast<unsigned char*>(payload.data()), len)) {
            return 1;
        }

        nlohmann::json reply = nlohmann::json::object();
        bool spawnHint = false;
        try {
            const nlohmann::json msg = nlohmann::json::parse(payload);
            if (!msg.is_object()) {
                reply["ok"] = false;
                reply["error"] = "invalid message";
            } else {
                const std::string type = msg.value("type", "download");
                if (type == "ping") {
                    reply["ok"] = true;
                    reply["app"] = "TinyNext";
                    reply["version"] = TINY_APP_VERSION;
                } else if (type == "download") {
                    const std::string url = msg.value("url", "");
                    if (url.empty() || !isDownloadableSource(url)) {
                        reply["ok"] = false;
                        reply["error"] = "unsupported url";
                    } else if (forwardUrls &&
                               forwardUrls(std::vector<std::string>{url})) {
                        reply["ok"] = true;  // 已交给运行中的主实例
                    } else {
                        // 主实例不在：URL 已由回调落 inbox，拉起 GUI 消费。
                        reply["ok"] = true;
                        reply["queued"] = true;
                        spawnHint = true;
                    }
                } else {
                    reply["ok"] = false;
                    reply["error"] = "unknown type";
                }
            }
        } catch (const std::exception& e) {
            reply = nlohmann::json::object();
            reply["ok"] = false;
            reply["error"] = e.what();
        }
        if (!sendReply(reply)) return 1;
        if (spawnHint && !spawnedGui) {
            spawnedGui = true;
            spawnAppDetached();
        }
    }
}

// GUI 启动时调用（幂等 best-effort）：写宿主清单 + 登记 Chrome。失败静默——
// Chrome 未装/目录只读都是正常场景，不打扰用户；扩展连不上时 popup 会提示。
export void registerChromeNativeHost() {
    const std::filesystem::path manifest = writeHostManifest();
    if (manifest.empty()) return;
    (void)registerManifestInChrome(manifest);
}

} // namespace browser_host
