// config.cppm — persistent JSON app settings for TinyNext.
// Zero EUI dependency: std + nlohmann::json only. Config file: tinynext.conf
// in the run directory. Keys: "download_dir" (string), "theme_mode"
// ("system"|"dark"|"light"). Missing/malformed values fall back to defaults.
module;

#ifdef _WIN32
// Platform header goes in the global module fragment (before import std;).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdio>  // popen/fgets/pclose（POSIX，osDark 探测系统深色）— before import std
#endif

// 版本宏（TINY_APP_VERSION / TINY_EUI_VERSION / ...）由 scripts/gen-versions.ps1 /
// gen-versions.sh 从根目录 mcpp.toml 生成——版本只在 mcpp.toml 维护，升级后跑一次
// 生成脚本（打包脚本也会自动跑）。不要手工改这个头。
#include "versions.generated.h"

export module tinynext.config;

import std;
import nlohmann.json;

namespace cfg {

// ---- 版本信息（单一代码源：根目录 mcpp.toml）----
// 关于弹窗从这里读版本，不要再在别处硬编码。升级 mcpp.toml 后跑
// scripts/gen-versions.ps1（或 .sh）重新生成 versions.generated.h 即可。
// mcpp 无法在编译期注入版本（defines 不支持变量插值、无包版本宏），
// 生成脚本是"编译时读 mcpp.toml"的最贴近实现。
export constexpr std::string_view kAppVersion = TINY_APP_VERSION;
export constexpr std::string_view kEuiVersion = TINY_EUI_VERSION;

// ---- config file IO ----

// Per-user config directory: Windows %APPDATA%\TinyNext / macOS
// ~/Library/Application Support/TinyNext / Linux $XDG_CONFIG_HOME/tinynext
// (fallback ~/.config/tinynext). 安装版经快捷方式启动时 cwd 可能是 System32（不可写），
// 配置与 aria2 session 不能依赖 cwd。导出给 aria2_engine 存 session 用。
export std::filesystem::path configDir() {
#ifdef _WIN32
    if (const char* a = std::getenv("APPDATA"); a && *a) {
        return std::filesystem::path(a) / "TinyNext";
    }
#elif defined(__APPLE__)
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path(h) / "Library" / "Application Support" / "TinyNext";
    }
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) {
        return std::filesystem::path(x) / "tinynext";
    }
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path(h) / ".config" / "tinynext";
    }
#endif
    return std::filesystem::current_path();
}

namespace {

std::filesystem::path configPath() {
    // 便携版（exe 旁）已有配置 → 继续用它，尊重已有用户；否则用 per-user 目录。
    std::error_code ec;
    const std::filesystem::path cwdConf = std::filesystem::current_path() / "tinynext.conf";
    if (std::filesystem::exists(cwdConf, ec)) return cwdConf;
    return configDir() / "tinynext.conf";
}

nlohmann::json loadConfig() {
    nlohmann::json j = nlohmann::json::object();
    if (std::ifstream in(configPath()); in) {
        try {
            in >> j;
        } catch (...) {
            j = nlohmann::json::object();
        }
        if (!j.is_object()) j = nlohmann::json::object();
    }
    return j;
}

void saveConfig(const nlohmann::json& j) {
    const std::filesystem::path path = configPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);  // per-user 目录可能不存在
    std::ofstream out(path, std::ios::trunc);
    if (out) out << j.dump(2);
}

std::string trimString(std::string s) {
    const std::size_t f = s.find_first_not_of(" \t\r\n");
    const std::size_t l = s.find_last_not_of(" \t\r\n");
    return f == std::string::npos ? "" : s.substr(f, l - f + 1);
}

} // namespace

// ---- download directory ----

export std::filesystem::path defaultDownloadDir() {
#ifdef _WIN32
    // FOLDERID_Downloads via SHGetKnownFolderPath — handles OneDrive-redirected
    // Downloads. shell32/ole32 are loaded dynamically so no link change is needed.
    using ShGetKnownFolderPathFn = HRESULT(WINAPI*)(const GUID&, DWORD, HANDLE, PWSTR*);
    using CoTaskMemFreeFn = void(WINAPI*)(void*);
    static const ShGetKnownFolderPathFn shGet = []() -> ShGetKnownFolderPathFn {
        HMODULE m = LoadLibraryW(L"shell32.dll");
        if (!m) return nullptr;
        return reinterpret_cast<ShGetKnownFolderPathFn>(
            reinterpret_cast<void*>(GetProcAddress(m, "SHGetKnownFolderPath")));
    }();
    static const CoTaskMemFreeFn coFree = []() -> CoTaskMemFreeFn {
        HMODULE m = LoadLibraryW(L"ole32.dll");
        if (!m) return nullptr;
        return reinterpret_cast<CoTaskMemFreeFn>(
            reinterpret_cast<void*>(GetProcAddress(m, "CoTaskMemFree")));
    }();
    if (shGet && coFree) {
        // FOLDERID_Downloads = {374DE290-123F-4565-9164-39C4925E467B}
        static const GUID kDownloads{0x374de290, 0x123f, 0x4565,
            {0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b}};
        PWSTR raw = nullptr;
        if (SUCCEEDED(shGet(kDownloads, 0, nullptr, &raw)) && raw) {
            std::filesystem::path result(raw);
            coFree(raw);
            return result;
        }
    }
    if (const char* user = std::getenv("USERPROFILE")) {
        return std::filesystem::path(user) / "Downloads";
    }
    return std::filesystem::current_path();
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / "Downloads";
    }
    return std::filesystem::current_path();
#else  // Linux / other
    // XDG_DOWNLOAD_DIR from ~/.config/user-dirs.dirs, else $HOME/Downloads.
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path xdg =
            std::filesystem::path(home) / ".config" / "user-dirs.dirs";
        std::ifstream in(xdg);
        std::string line;
        while (in && std::getline(in, line)) {
            if (!line.starts_with("XDG_DOWNLOAD_DIR=")) continue;
            const auto eq = line.find('=');
            std::string val = line.substr(eq + 1);
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.size() - 2);
            }
            if (val.starts_with("$HOME/")) {
                return std::filesystem::path(home) / val.substr(6);
            }
            if (val.starts_with('/')) {
                return std::filesystem::path(val);
            }
        }
        return std::filesystem::path(home) / "Downloads";
    }
    return std::filesystem::current_path();
#endif
}

export std::filesystem::path downloadDir() {
    const auto j = loadConfig();
    if (j.contains("download_dir") && j["download_dir"].is_string()) {
        const std::string saved = trimString(j["download_dir"].get<std::string>());
        if (!saved.empty()) return std::filesystem::path(saved);
    }
    return defaultDownloadDir();
}

export void setDownloadDir(const std::filesystem::path& dir) {
    auto j = loadConfig();
    j["download_dir"] = dir.string();
    saveConfig(j);
}

// ---- theme mode ----

export enum class ThemeMode { System, Dark, Light };

export ThemeMode themeMode() {
    const auto j = loadConfig();
    if (j.contains("theme_mode") && j["theme_mode"].is_string()) {
        const std::string v = j["theme_mode"].get<std::string>();
        if (v == "light") return ThemeMode::Light;
        if (v == "dark") return ThemeMode::Dark;
    }
    return ThemeMode::System;  // default: follow the OS
}

export void setThemeMode(ThemeMode mode) {
    auto j = loadConfig();
    switch (mode) {
        case ThemeMode::Light:  j["theme_mode"] = "light";  break;
        case ThemeMode::Dark:   j["theme_mode"] = "dark";   break;
        case ThemeMode::System: j["theme_mode"] = "system"; break;
    }
    saveConfig(j);
}

// 关闭窗口行为：true = 缩到系统托盘（eui .tray()，Windows/macOS 生效；Linux 因
// eui-neo 配方托盘为 stub 无实际效果，X 仍直接退出）。启动时由 dslAppConfig()
// 读取，改后重启生效。
export bool closeToTray() {
    const auto j = loadConfig();
    if (j.contains("close_to_tray") && j["close_to_tray"].is_boolean()) {
        return j["close_to_tray"].get<bool>();
    }
    return true;  // 默认缩托盘
}

export void setCloseToTray(bool value) {
    auto j = loadConfig();
    j["close_to_tray"] = value;
    saveConfig(j);
}

// ---- OS dark-mode detection (for ThemeMode::System) ----

export bool osDark() {
#ifdef _WIN32
    // AppsUseLightTheme (HKCU\...\Themes\Personalize): 0 = dark, 1 = light.
    // SHGetValueW 是 shlwapi.dll 的导出（不是 shell32），动态加载避免链接 shlwapi。
    using ShGetValueFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD*, void*, DWORD*);
    static const ShGetValueFn shGetValue = []() -> ShGetValueFn {
        HMODULE m = LoadLibraryW(L"shlwapi.dll");
        if (!m) return nullptr;
        return reinterpret_cast<ShGetValueFn>(
            reinterpret_cast<void*>(GetProcAddress(m, "SHGetValueW")));
    }();
    if (shGetValue) {
        DWORD type = 0, value = 1, size = sizeof(value);
        const LSTATUS st = shGetValue(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", &type, &value, &size);
        if (st == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(value)) {
            return value == 0;
        }
    }
    return true;  // default dark
#elif defined(__APPLE__)
    // Best-effort: the plist may be XML; look for AppleInterfaceStyle=Dark.
    if (const char* home = std::getenv("HOME")) {
        std::ifstream in(std::filesystem::path(home) /
                             "Library/Preferences/.GlobalPreferences.plist",
                         std::ios::binary);
        if (in) {
            const std::string content((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            if (content.find("AppleInterfaceStyle") != std::string::npos &&
                content.find("Dark") != std::string::npos) {
                return true;
            }
        }
    }
    return false;  // light
#else
    // Linux best-effort（从最通用到最旧的顺序）。事件驱动：由 theme_watch 在 OS
    // 主题变化时触发调用一次（不再每 2s 轮询 spawn 探测进程）。
    auto shellOut = [](const char* c) -> std::string {
        FILE* pipe = ::popen(c, "r");
        if (!pipe) return {};
        std::string out;
        char buf[128];
        while (::fgets(buf, sizeof(buf), pipe)) out += buf;
        ::pclose(pipe);
        return out;
    };
    // 1. GNOME 42+ / 多数现代桌面：gsettings color-scheme
    //    （"prefer-dark" / "default" / "prefer-light"）。注意很多新系统不写
    //    settings.ini，只有这个 key，所以必须先查。
    const std::string gs = shellOut(
        "gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null");
    if (gs.find("prefer-dark") != std::string::npos) return true;
    if (gs.find("prefer-light") != std::string::npos) return false;
    // 2. gsettings 不可用（无 schema / 非 GNOME）时试 KDE Plasma 6：ColorScheme
    //    值如 "BreezeDark" / "BreezeLight"。GNOME 上 gs="default" 表示未显式设置，
    //    不需要再试 KDE。
    if (gs.empty()) {
        const std::string kde = shellOut(
            "kreadconfig6 --file kdeglobals --group General --key ColorScheme 2>/dev/null");
        if (kde.find("Dark") != std::string::npos) return true;
        if (kde.find("Light") != std::string::npos) return false;
    }
    // 3. 旧 GTK：settings.ini 的 gtk-application-prefer-dark-theme=1，或
    //    gtk-theme-name 含 "dark"（如 Adwaita-dark）。
    if (const char* home = std::getenv("HOME")) {
        for (const char* sub : {"/.config/gtk-3.0/settings.ini",
                                "/.config/gtk-4.0/settings.ini"}) {
            std::ifstream in(std::filesystem::path(home) / sub);
            std::string line;
            while (in && std::getline(in, line)) {
                // 有的环境写 =1，有的写 =true（都是 GTK 布尔值）。
                if (line.find("gtk-application-prefer-dark-theme=1") !=
                        std::string::npos ||
                    line.find("gtk-application-prefer-dark-theme=true") !=
                        std::string::npos) {
                    return true;
                }
                if (line.find("gtk-theme-name=") != std::string::npos &&
                    line.find("dark") != std::string::npos) {
                    return true;  // 如 Adwaita-dark
                }
            }
        }
    }
    return false;  // light
#endif
}

export bool effectiveDark() {
    switch (themeMode()) {
        case ThemeMode::Light:  return false;
        case ThemeMode::Dark:   return true;
        case ThemeMode::System: return osDark();
    }
    return true;
}

// ---- download engine ----
// 纯 aria2-next：TinyHttpsEngine 已移除（2026-08），不再有引擎切换配置。
// 旧配置里的 "engine" 键直接忽略。

// ---- aria2 tuning (used by Aria2Engine) ----

export struct Aria2Config {
    int split = 64;                     // --split (分片数)
    int maxConnectionPerServer = 64;    // --max-connection-per-server
    std::string minSplitSize = "1M";    // --min-split-size (aria2 最小 1M)
    std::int64_t maxDownloadLimit = 0;  // 每任务限速 bytes/s；0 = 不限
    // ---- daemon 级参数（--all-proxy 等，daemon 启动时生效）----
    std::string proxy = "";             // --all-proxy；HTTP/HTTPS（aria2 不支持 SOCKS5）
    std::string noProxy = "";           // --no-proxy；逗号分隔的主机
    int maxTries = 5;                   // --max-tries；0 = 无限重试
    int retryWait = 0;                  // --retry-wait（秒）
    int maxConcurrentDownloads = 5;     // --max-concurrent-downloads
    bool removeControlFile = false;     // --remove-control-file；完成后移除 .aria2
    std::string onDownloadComplete = "";// --on-download-complete；完成后命令，空=不执行
    std::string userAgent = "";         // --user-agent；空 = aria2 默认
    std::string referer = "";           // --referer；空 = 无
    std::string diskCache = "";         // --disk-cache；空 = aria2 默认 16M
    // ---- BitTorrent（daemon 级）----
    int seedTime = 0;                    // --seed-time（秒）；0 = 不限
    double seedRatio = 0.0;              // --seed-ratio；0 = 不限（aria2 默认 1.0）
    int btMaxPeers = 0;                  // --bt-max-peers；0 = aria2 默认
    std::string listenPort = "";         // --listen-port；空 = aria2 默认 6881-6999
    bool btEnableLpd = false;            // --bt-enable-lpd（局域网发现）
    // ---- HTTP（daemon 级）----
    std::string header = "";             // --header；多行 → 拆成多个 --header
    std::string loadCookies = "";        // --load-cookies 文件路径
    std::string saveCookies = "";        // --save-cookies 文件路径
    // ---- 下载行为（daemon 级）----
    std::int64_t maxOverallDownloadLimit = 0;  // --max-overall-download-limit bytes/s；0=不限
    std::string fileAllocation = "";     // --file-allocation（none/trunc/falloc）；空=aria2 默认
    bool autoFileRenaming = true;        // --auto-file-renaming（曾硬编码 true）
    bool allowOverwrite = false;         // --allow-overwrite（曾硬编码 false）
    // ---- 完整性校验（daemon 级）----
    bool checkIntegrity = false;         // --check-integrity
    std::string checksum = "";           // --checksum（"sha-1=<hex>"）
    // ---- ED2K（daemon 级；aria2-next 原生支持电驴）----
    std::string ed2kServers = "";        // --ed2k-server；逗号分隔 HOST:PORT，空=默认
    std::string ed2kListenPort = "";     // --ed2k-listen-port；TCP，空=默认 4662
    std::string ed2kUdpListenPort = "";  // --ed2k-udp-listen-port；Kad，空=默认 4672
    int ed2kUploadSlots = 0;             // --ed2k-upload-slots；0=默认 3
};

export Aria2Config aria2Config() {
    const auto j = loadConfig();
    Aria2Config c;
    if (j.contains("aria2") && j["aria2"].is_object()) {
        const auto& a = j["aria2"];
        if (a.contains("split") && a["split"].is_number_integer()) {
            c.split = a["split"].get<int>();
        }
        if (a.contains("max-connection-per-server") &&
            a["max-connection-per-server"].is_number_integer()) {
            c.maxConnectionPerServer = a["max-connection-per-server"].get<int>();
        }
        if (a.contains("min-split-size") && a["min-split-size"].is_string()) {
            c.minSplitSize = a["min-split-size"].get<std::string>();
        }
        if (a.contains("max-download-limit") &&
            a["max-download-limit"].is_number_integer()) {
            c.maxDownloadLimit = a["max-download-limit"].get<std::int64_t>();
        }
        if (a.contains("proxy") && a["proxy"].is_string()) {
            c.proxy = a["proxy"].get<std::string>();
        }
        if (a.contains("no_proxy") && a["no_proxy"].is_string()) {
            c.noProxy = a["no_proxy"].get<std::string>();
        }
        if (a.contains("max_tries") && a["max_tries"].is_number_integer()) {
            c.maxTries = a["max_tries"].get<int>();
        }
        if (a.contains("retry_wait") && a["retry_wait"].is_number_integer()) {
            c.retryWait = a["retry_wait"].get<int>();
        }
        if (a.contains("max_concurrent_downloads") &&
            a["max_concurrent_downloads"].is_number_integer()) {
            c.maxConcurrentDownloads = a["max_concurrent_downloads"].get<int>();
        }
        if (a.contains("remove_control_file") &&
            a["remove_control_file"].is_boolean()) {
            c.removeControlFile = a["remove_control_file"].get<bool>();
        }
        if (a.contains("on_download_complete") &&
            a["on_download_complete"].is_string()) {
            c.onDownloadComplete = a["on_download_complete"].get<std::string>();
        }
        if (a.contains("user_agent") && a["user_agent"].is_string()) {
            c.userAgent = a["user_agent"].get<std::string>();
        }
        if (a.contains("referer") && a["referer"].is_string()) {
            c.referer = a["referer"].get<std::string>();
        }
        if (a.contains("disk_cache") && a["disk_cache"].is_string()) {
            c.diskCache = a["disk_cache"].get<std::string>();
        }
        if (a.contains("seed_time") && a["seed_time"].is_number_integer()) {
            c.seedTime = a["seed_time"].get<int>();
        }
        if (a.contains("seed_ratio") && a["seed_ratio"].is_number()) {
            c.seedRatio = a["seed_ratio"].get<double>();
        }
        if (a.contains("bt_max_peers") && a["bt_max_peers"].is_number_integer()) {
            c.btMaxPeers = a["bt_max_peers"].get<int>();
        }
        if (a.contains("listen_port") && a["listen_port"].is_string()) {
            c.listenPort = a["listen_port"].get<std::string>();
        }
        if (a.contains("bt_enable_lpd") && a["bt_enable_lpd"].is_boolean()) {
            c.btEnableLpd = a["bt_enable_lpd"].get<bool>();
        }
        if (a.contains("header") && a["header"].is_string()) {
            c.header = a["header"].get<std::string>();
        }
        if (a.contains("load_cookies") && a["load_cookies"].is_string()) {
            c.loadCookies = a["load_cookies"].get<std::string>();
        }
        if (a.contains("save_cookies") && a["save_cookies"].is_string()) {
            c.saveCookies = a["save_cookies"].get<std::string>();
        }
        if (a.contains("max_overall_download_limit") &&
            a["max_overall_download_limit"].is_number_integer()) {
            c.maxOverallDownloadLimit = a["max_overall_download_limit"].get<std::int64_t>();
        }
        if (a.contains("file_allocation") && a["file_allocation"].is_string()) {
            c.fileAllocation = a["file_allocation"].get<std::string>();
        }
        if (a.contains("auto_file_renaming") && a["auto_file_renaming"].is_boolean()) {
            c.autoFileRenaming = a["auto_file_renaming"].get<bool>();
        }
        if (a.contains("allow_overwrite") && a["allow_overwrite"].is_boolean()) {
            c.allowOverwrite = a["allow_overwrite"].get<bool>();
        }
        if (a.contains("check_integrity") && a["check_integrity"].is_boolean()) {
            c.checkIntegrity = a["check_integrity"].get<bool>();
        }
        if (a.contains("checksum") && a["checksum"].is_string()) {
            c.checksum = a["checksum"].get<std::string>();
        }
        if (a.contains("ed2k_servers") && a["ed2k_servers"].is_string()) {
            c.ed2kServers = a["ed2k_servers"].get<std::string>();
        }
        if (a.contains("ed2k_listen_port") && a["ed2k_listen_port"].is_string()) {
            c.ed2kListenPort = a["ed2k_listen_port"].get<std::string>();
        }
        if (a.contains("ed2k_udp_listen_port") && a["ed2k_udp_listen_port"].is_string()) {
            c.ed2kUdpListenPort = a["ed2k_udp_listen_port"].get<std::string>();
        }
        if (a.contains("ed2k_upload_slots") && a["ed2k_upload_slots"].is_number_integer()) {
            c.ed2kUploadSlots = a["ed2k_upload_slots"].get<int>();
        }
    }
    c.split = std::clamp(c.split, 1, 64);
    c.maxConnectionPerServer = std::clamp(c.maxConnectionPerServer, 1, 64);
    c.maxTries = std::clamp(c.maxTries, 0, 100);
    c.retryWait = std::clamp(c.retryWait, 0, 600);
    c.maxConcurrentDownloads = std::clamp(c.maxConcurrentDownloads, 1, 64);
    if (c.maxDownloadLimit < 0) c.maxDownloadLimit = 0;
    c.seedTime = std::clamp(c.seedTime, 0, 100000);
    if (!(c.seedRatio > 0.0)) c.seedRatio = 0.0;  // 负数/NaN 归 0
    c.btMaxPeers = std::clamp(c.btMaxPeers, 0, 10000);
    if (c.maxOverallDownloadLimit < 0) c.maxOverallDownloadLimit = 0;
    c.ed2kUploadSlots = std::clamp(c.ed2kUploadSlots, 0, 100);
    return c;
}

export void setAria2Config(const Aria2Config& c) {
    auto j = loadConfig();
    nlohmann::json a = j.value("aria2", nlohmann::json::object());
    a["split"] = std::clamp(c.split, 1, 64);
    a["max-connection-per-server"] = std::clamp(c.maxConnectionPerServer, 1, 64);
    a["min-split-size"] = c.minSplitSize.empty() ? "1M" : c.minSplitSize;
    a["max-download-limit"] = c.maxDownloadLimit < 0 ? 0 : c.maxDownloadLimit;
    a["proxy"] = c.proxy;
    a["no_proxy"] = c.noProxy;
    a["max_tries"] = std::clamp(c.maxTries, 0, 100);
    a["retry_wait"] = std::clamp(c.retryWait, 0, 600);
    a["max_concurrent_downloads"] = std::clamp(c.maxConcurrentDownloads, 1, 64);
    a["remove_control_file"] = c.removeControlFile;
    a["on_download_complete"] = c.onDownloadComplete;
    a["user_agent"] = c.userAgent;
    a["referer"] = c.referer;
    a["disk_cache"] = c.diskCache;
    a["seed_time"] = std::clamp(c.seedTime, 0, 100000);
    a["seed_ratio"] = c.seedRatio > 0.0 ? c.seedRatio : 0.0;
    a["bt_max_peers"] = std::clamp(c.btMaxPeers, 0, 10000);
    a["listen_port"] = c.listenPort;
    a["bt_enable_lpd"] = c.btEnableLpd;
    a["header"] = c.header;
    a["load_cookies"] = c.loadCookies;
    a["save_cookies"] = c.saveCookies;
    a["max_overall_download_limit"] = c.maxOverallDownloadLimit < 0
        ? 0 : c.maxOverallDownloadLimit;
    a["file_allocation"] = c.fileAllocation;
    a["auto_file_renaming"] = c.autoFileRenaming;
    a["allow_overwrite"] = c.allowOverwrite;
    a["check_integrity"] = c.checkIntegrity;
    a["checksum"] = c.checksum;
    a["ed2k_servers"] = c.ed2kServers;
    a["ed2k_listen_port"] = c.ed2kListenPort;
    a["ed2k_udp_listen_port"] = c.ed2kUdpListenPort;
    a["ed2k_upload_slots"] = std::clamp(c.ed2kUploadSlots, 0, 100);
    j["aria2"] = a;
    saveConfig(j);
}

} // namespace cfg
