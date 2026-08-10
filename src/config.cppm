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

export module tinynext.config;

import std;
import nlohmann.json;

namespace cfg {

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
    }
    c.split = std::clamp(c.split, 1, 64);
    c.maxConnectionPerServer = std::clamp(c.maxConnectionPerServer, 1, 64);
    c.maxTries = std::clamp(c.maxTries, 0, 100);
    c.retryWait = std::clamp(c.retryWait, 0, 600);
    c.maxConcurrentDownloads = std::clamp(c.maxConcurrentDownloads, 1, 64);
    if (c.maxDownloadLimit < 0) c.maxDownloadLimit = 0;
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
    j["aria2"] = a;
    saveConfig(j);
}

} // namespace cfg
