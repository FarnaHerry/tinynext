// utils.cppm — pure string/number helpers shared by every layer (engine-side
// stores, CLI, headless, UI). std only, no eui / no config dependency.
//
// 历史：这些函数原来在 ui/utils.cppm，store 化拆分时下移到这里，让领域层
// （tinynext.store.tasks / headless）不必 import 任何 ui.* 模块。ui/utils.cppm
// 现在只保留布局常量，并 export import 本模块转发（既有 UI 代码不用改）。
module;

// pathFromUtf8/utf8FromPath 在 Windows 需要 MultiByteToWideChar/WideCharToMultiByte；
// commandLineArgs 在 Windows 需要 GetCommandLineW/CommandLineToArgvW。
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
#include "native_resource.hpp"  // UniqueModule / UniqueLocalAlloc
#elif defined(__APPLE__)
#include <crt_externs.h>  // _NSGetArgc/_NSGetArgv
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

// aria2-next 2.8.x 支持的下载 URL：http(s) / sftp / ED2K file / magnet。
// CLI / TaskStore::startFromUrl / 添加弹窗剪贴板预填 共用（一处维护，避免三处漂移）。
// 本地 .torrent 文件路径不在这里（它不是 URL），由各调用方按扩展名单独放行。
export bool isDownloadableSource(const std::string& s) {
    return s.starts_with("http://") || s.starts_with("https://") ||
           s.starts_with("sftp://") || s.starts_with("magnet:") ||
           s.starts_with("ed2k://|file|");
}

export bool isMirrorableSource(const std::string& s) {
    return s.starts_with("http://") || s.starts_with("https://") ||
           s.starts_with("sftp://");
}

export std::string fileNameFromUrl(const std::string& url) {
    // ED2K file link: ed2k://|file|<name>|<size>|<hash>|/
    if (url.starts_with("ed2k://|file|")) {
        const std::size_t nameStart = std::string_view("ed2k://|file|").size();
        const std::size_t nameEnd = url.find('|', nameStart);
        if (nameEnd != std::string::npos && nameEnd > nameStart) {
            return percentDecode(url.substr(nameStart, nameEnd - nameStart));
        }
    }
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

// UTF-8 边界安全截断：最多 maxBytes 字节，绝不在多字节字符中间切开（切半的
// 字符会让下游 nlohmann::json 解析抛 invalid UTF-8——sanitizeFileName 按字节
// 截 80 导致 b 站中文标题下载失败就是踩的这个）。
export std::string truncateUtf8Bytes(std::string_view s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return std::string(s);
    std::size_t n = maxBytes;
    // s[n] 是切点后第一个字节：若它是续字节（10xxxxxx），说明切在字符中间，
    // 回退到该字符的起始字节，把整个不完整字符丢掉。
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return std::string(s.substr(0, n));
}

export std::string trimText(std::string s) {
    const std::size_t f = s.find_first_not_of(" \t\r\n");
    const std::size_t l = s.find_last_not_of(" \t\r\n");
    return f == std::string::npos ? "" : s.substr(f, l - f + 1);
}

// ---- 路径编码（Windows 关键）----
// Windows 上 std::filesystem::path 的窄字符串构造/提取走系统 ANSI 代码页（中文系统
// 是 GBK）：把 UTF-8 串直接构造 path 会乱码，遇到非法 GBK 字节对还会抛
// ERROR_NO_UNICODE_TRANSLATION（如 URL 里的中文文件名）。本应用的字符串约定是
// UTF-8（aria2 JSON-RPC、UI 输入都是 UTF-8），所以字符串↔path 必须经这两个
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

// ---- 命令行参数 ----
// Linux 专用的小工具：识别「通过动态加载器直接启动」的 cmdline。
#if !defined(_WIN32) && !defined(__APPLE__)
namespace {
// argv[0] 是不是动态加载器本尊（`/lib64/ld-linux-x86-64.so.2`、`ld.so`、musl 的
// `ld-musl-*`）——此时真实程序路径不在 argv[0]，程序是加载器自己 mmap 起来的。
bool looksLikeDynamicLoader(const std::string& token) {
    const std::size_t slash = token.rfind('/');
    const std::string base = token.substr(slash == std::string::npos ? 0 : slash + 1);
    return base.starts_with("ld-linux") || base.starts_with("ld-musl") ||
           base == "ld.so" || base.starts_with("ld.so.");
}

// ld.so 里需要跟一个值的选项（其余都是开关）；`--opt=value` 形式算单个 token。
bool loaderOptionTakesValue(const std::string& opt) {
    return opt == "--library-path" || opt == "--preload" || opt == "--audit" ||
           opt == "--inhibit-rpath" || opt == "--argv0" ||
           opt == "--glibc-hwcaps-mask" || opt == "--glibc-hwcaps-prepend";
}
}  // namespace
#endif

// 全部命令行参数（不含程序自身路径，按原顺序）。静态缓存一次，可从静态初始化调用。
//
// Linux 的坑（/proc/self/cmdline）：**通过动态加载器直接启动**时
// （`ld.so [加载器选项] /path/tinynext <args>`，本仓库的 run.sh 就是这么干的——
// 系统 glibc/Mesa 与 mcpp 工具链 glibc 不兼容，必须走 ld.so 的 --library-path），
// argv[0] 是 ld.so、程序路径变成中间的一个普通参数：
//   [ld.so, --inhibit-rpath, "", --library-path, /usr/lib64:…, /path/tinynext, agent]
// 而 /proc/self/exe 这时候也指向 ld.so（没有真正的 execve），定位不了程序路径。
// 只跳过第 0 个 token 会把 --inhibit-rpath 当首参数，让 agent / status / list /
// --headless / --restart 这些按 args.front() 分派的入口全部失效（下载 URL 不受
// 影响，因为它们扫全量参数）。所以先剥掉加载器自己的选项，再跳过程序路径。
//
// Windows / macOS 走系统 argv，不受影响。
export std::vector<std::string> commandLineArgs() {
    static const std::vector<std::string> cached = [] {
        std::vector<std::string> args;
#ifdef _WIN32
        using CmdToArgvFn = LPWSTR*(WINAPI*)(LPCWSTR, int*);
        static const tinynext::native::UniqueModule shell32(LoadLibraryW(L"shell32.dll"));
        static const CmdToArgvFn cmdToArgv = [&]() -> CmdToArgvFn {
            if (!shell32) return nullptr;
            return reinterpret_cast<CmdToArgvFn>(
                reinterpret_cast<void*>(GetProcAddress(shell32.get(), "CommandLineToArgvW")));
        }();
        if (cmdToArgv) {
            int argc = 0;
            LPWSTR* wargv = cmdToArgv(GetCommandLineW(), &argc);
            if (wargv) {
                tinynext::native::UniqueLocalAlloc argsOwner(static_cast<HLOCAL>(wargv));
                for (int i = 1; i < argc; ++i) {
                    const std::wstring w(wargv[i]);
                    args.push_back(std::string(w.begin(), w.end()));
                }
            }
        }
#elif defined(__APPLE__)
        const int argc = *_NSGetArgc();
        char** argv = *_NSGetArgv();
        for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
#else
        std::vector<std::string> tokens;
        {
            std::ifstream in("/proc/self/cmdline", std::ios::binary);
            std::string s((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
            std::size_t start = 0;
            while (start < s.size()) {
                const std::size_t end = s.find('\0', start);
                tokens.push_back(s.substr(start, end - start));
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
        std::size_t first = 1;  // 默认只跳过 argv[0]
        if (!tokens.empty() && looksLikeDynamicLoader(tokens[0])) {
            std::size_t i = 1;
            while (i < tokens.size() && tokens[i].starts_with("--") &&
                   tokens[i] != "--") {
                i += loaderOptionTakesValue(tokens[i]) ? 2 : 1;
            }
            if (i < tokens.size() && tokens[i] == "--") ++i;
            if (i < tokens.size()) first = i + 1;  // 再跳过程序路径
        }
        for (std::size_t i = first; i < tokens.size(); ++i) args.push_back(tokens[i]);
#endif
        return args;
    }();
    return cached;
}
