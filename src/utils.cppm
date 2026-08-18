// utils.cppm — pure string/number helpers shared by every layer (engine-side
// stores, CLI, headless, UI). std only, no eui / no config dependency.
//
// 历史：这些函数原来在 ui/utils.cppm，store 化拆分时下移到这里，让领域层
// （tinynext.store.tasks / headless）不必 import 任何 ui.* 模块。ui/utils.cppm
// 现在只保留布局常量，并 export import 本模块转发（既有 UI 代码不用改）。
module;

// pathFromUtf8/utf8FromPath 在 Windows 需要 MultiByteToWideChar/WideCharToMultiByte。
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// minwindef.h 会定义 min/max 宏，漏掉 NOMINMAX 会让模块 purview 里的 std::max
// 变成宏展开（其它 TU 都定义了，这里保持一致）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

export module tinynext.utils;

import std;

export std::string percentDecode(std::string s) {
    const auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexValue(s[i + 1]);
            const int lo = hexValue(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// 可下载源的 URL 前缀白名单：http(s) / ftp(s) / sftp 直链 + magnet 磁力。
// CLI / TaskStore::startFromUrl / 添加弹窗剪贴板预填 共用（一处维护，避免三处漂移）。
// 本地 .torrent 文件路径不在这里（它不是 URL），由各调用方按扩展名单独放行。
export bool isDownloadableSource(const std::string& s) {
    return s.starts_with("http://") || s.starts_with("https://") ||
           s.starts_with("ftp://") || s.starts_with("ftps://") ||
           s.starts_with("sftp://") || s.starts_with("magnet:");
}

export std::string fileNameFromUrl(const std::string& url) {
    const std::size_t cut = url.find_first_of("?#");
    const std::string base = cut == std::string::npos ? url : url.substr(0, cut);
    const std::size_t slash = base.find_last_of('/');
    std::string name = slash == std::string::npos ? base : base.substr(slash + 1);
    if (name.empty()) {
        name = "download";
    }
    return percentDecode(std::move(name));
}

export std::string formatBytes(std::int64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return std::format("{} B", bytes);
    }
    return std::format("{:.1f} {}", value, kUnits[unit]);
}

export std::string formatSpeed(double bytesPerSecond) {
    if (bytesPerSecond <= 0.0) {
        return "";
    }
    return formatBytes(static_cast<std::int64_t>(bytesPerSecond)) + "/s";
}

export std::string trimText(std::string s) {
    const std::size_t f = s.find_first_not_of(" \t\r\n");
    const std::size_t l = s.find_last_not_of(" \t\r\n");
    return f == std::string::npos ? "" : s.substr(f, l - f + 1);
}

// ---- 路径编码（Windows 关键）----
// Windows 上 std::filesystem::path 的窄字符串构造/提取走系统 ANSI 代码页（中文系统
// 是 GBK）：把 UTF-8 串直接构造 path 会乱码，遇到非法 GBK 字节对还会抛
// ERROR_NO_UNICODE_TRANSLATION（如 b 站中文视频标题）。本应用的字符串约定是
// UTF-8（aria2 JSON-RPC、yt-dlp 输出都是 UTF-8），所以字符串↔path 必须经这两个
// helper 显式按 UTF-8 转换。POSIX 窄字符串天然 UTF-8，直接透传。

// UTF-8 字符串 → path。
export std::filesystem::path pathFromUtf8(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};  // 非法 UTF-8：返回空 path（调用方按空处理）
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        w.data(), n);
    return std::filesystem::path(std::move(w));
#else
    return std::filesystem::path(utf8);
#endif
}

// path → UTF-8 字符串（给 JSON-RPC / 进程参数等 UTF-8 语境用）。
export std::string utf8FromPath(const std::filesystem::path& p) {
#ifdef _WIN32
    const std::wstring w = p.wstring();
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
#else
    return p.string();
#endif
}

// 解析整数并夹取到 [lo, hi]；解析失败返回 fallback。
export int parseIntClamped(const std::string& s, int lo, int hi, int fallback) {
    try {
        return std::clamp(std::stoi(trimText(s)), lo, hi);
    } catch (...) {
        return fallback;
    }
}

// 解析 "512K"/"1M"/"2G" 大小字符串为字节数；失败返回 0。
export std::int64_t parseSizeBytes(const std::string& input) {
    const std::string s = trimText(input);
    if (s.empty()) return 0;
    std::int64_t mult = 1;
    std::string num = s;
    const char last = s.back();
    if (last == 'K' || last == 'k') { mult = 1024; num.pop_back(); }
    else if (last == 'M' || last == 'm') { mult = 1024 * 1024; num.pop_back(); }
    else if (last == 'G' || last == 'g') { mult = 1024LL * 1024 * 1024; num.pop_back(); }
    try {
        const double v = std::stod(num);
        return v <= 0 ? 0 : static_cast<std::int64_t>(v * static_cast<double>(mult));
    } catch (...) {
        return 0;
    }
}

// 把 "1M"/"512K"/"2G" 拆成数值与显示单位（KB/MB）；无/未知后缀按 MB 兜底。
// G 后缀（"2G"）不返回 GB——最小分片下拉只提供 KB/MB，统一换算成 MB 显示
// （"2G" → value "2048"、unit "MB"），保证显示单位恒为下拉可选项，保存时不会
// 再拼回 G 后缀。
export void splitSizeUnit(const std::string& size, std::string& value, std::string& unit) {
    if (size.size() >= 2) {
        switch (size.back()) {
            case 'K': case 'k': value = size.substr(0, size.size() - 1); unit = "KB"; return;
            case 'M': case 'm': value = size.substr(0, size.size() - 1); unit = "MB"; return;
            case 'G': case 'g': {
                // g ≥ 0（解析失败按 0），无需额外夹取。
                double g = 0.0;
                try { g = std::stod(size.substr(0, size.size() - 1)); } catch (...) {}
                value = std::to_string(
                    static_cast<long long>(std::llround(g * 1024.0)));
                unit = "MB";
                return;
            }
            default: break;
        }
    }
    value = size;
    unit = "MB";
}

// 数值 + 显示单位（KB/MB/GB）→ aria2 后缀形式（"1M"/"512K"/"2G"）。
export std::string joinSizeUnit(const std::string& value, const std::string& unit) {
    const char suffix = unit == "KB" ? 'K' : unit == "GB" ? 'G' : 'M';
    return trimText(value) + suffix;
}

// 单位（KB/MB/GB）→ 字节倍率（1024 进制）。
export std::int64_t sizeUnitMultiplier(const std::string& unit) {
    if (unit == "KB") return 1024;
    if (unit == "GB") return 1024LL * 1024 * 1024;
    return 1024LL * 1024;  // MB
}

// 把数值文本 value（单位 fromUnit）按 1024 进制换算到 toUnit，字节量不变。
// 换算结果若非整数则四舍五入到 ≥1 的整数（与整数步进输入对齐；min-split-size
// 只是分片阈值，微小的取整误差无影响）。空/非法输入回退 "1"。
export std::string convertSizeUnit(const std::string& value, const std::string& fromUnit,
                                   const std::string& toUnit) {
    const std::int64_t bytes = parseSizeBytes(joinSizeUnit(trimText(value), fromUnit));
    const std::int64_t mult = sizeUnitMultiplier(toUnit);
    if (bytes <= 0) return "1";
    const long long v = static_cast<long long>((bytes + mult / 2) / mult);
    return std::to_string(std::max(1LL, v));
}
