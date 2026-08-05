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
#endif

export module tinynext.config;

import std;
import nlohmann.json;

namespace cfg {

// ---- config file IO ----

namespace {

std::filesystem::path configPath() {
    return std::filesystem::current_path() / "tinynext.conf";
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
    std::ofstream out(configPath(), std::ios::trunc);
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

// ---- OS dark-mode detection (for ThemeMode::System) ----

export bool osDark() {
#ifdef _WIN32
    // AppsUseLightTheme (HKCU\...\Themes\Personalize): 0 = dark, 1 = light.
    // SHGetValueW is loaded from shell32 so no link change is needed.
    using ShGetValueFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD*, void*, DWORD*);
    static const ShGetValueFn shGetValue = []() -> ShGetValueFn {
        HMODULE m = LoadLibraryW(L"shell32.dll");
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
    // Linux best-effort: gtk dark preference from settings.ini.
    if (const char* home = std::getenv("HOME")) {
        for (const char* sub : {"/.config/gtk-3.0/settings.ini",
                                "/.config/gtk-4.0/settings.ini"}) {
            std::ifstream in(std::filesystem::path(home) / sub);
            std::string line;
            while (in && std::getline(in, line)) {
                if (line.find("gtk-application-prefer-dark-theme=1") !=
                    std::string::npos) {
                    return true;
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

export enum class EngineChoice { TinyHttps, Aria2Next };

export EngineChoice engine() {
    const auto j = loadConfig();
    if (j.contains("engine") && j["engine"].is_string()) {
        const std::string v = j["engine"].get<std::string>();
        if (v == "aria2next") return EngineChoice::Aria2Next;
    }
    return EngineChoice::TinyHttps;  // default: zero-external-dependency engine
}

export void setEngine(EngineChoice choice) {
    auto j = loadConfig();
    switch (choice) {
        case EngineChoice::TinyHttps:  j["engine"] = "tinyhttps";  break;
        case EngineChoice::Aria2Next:  j["engine"] = "aria2next";  break;
    }
    saveConfig(j);
}

// ---- aria2 tuning (used by Aria2Engine) ----

export struct Aria2Config {
    int split = 64;                     // --split (分片数)
    int maxConnectionPerServer = 64;    // --max-connection-per-server
    std::string minSplitSize = "1M";    // --min-split-size (aria2 最小 1M)
    std::int64_t maxDownloadLimit = 0;  // 每任务限速 bytes/s；0 = 不限
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
    }
    c.split = std::clamp(c.split, 1, 64);
    c.maxConnectionPerServer = std::clamp(c.maxConnectionPerServer, 1, 64);
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
    j["aria2"] = a;
    saveConfig(j);
}

} // namespace cfg
