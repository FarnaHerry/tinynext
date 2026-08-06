// ui/platform.cppm — OS integration: native folder picker, open file / folder /
// URL, and the high-DPI-awareness boot. No EUI dependency (platform APIs + std).
module;

// GLFW native access（所有平台都需要：剪贴板读写；folder picker 的父 HWND 仅 Windows）。
// GLFWwindow 在此保持不透明（不需要 eui_neo.h）；linkage spec 放在全局片段。
struct GLFWwindow;
extern "C" GLFWwindow* glfwGetCurrentContext();
extern "C" const char* glfwGetClipboardString(GLFWwindow* window);

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>  // BROWSEINFOW for the folder picker
extern "C" HWND glfwGetWin32Window(GLFWwindow* window);
#endif
#include <cstdio>  // popen/fgets/pclose (POSIX) — before import std

export module tinynext.ui.platform;

import std;

namespace {

#ifdef _WIN32
// High-DPI awareness must be declared before any window/GDI object exists.
// The app-main entry's main() does not set it (GLFW never does on Windows), so
// without this the process is DPI-unaware and Windows bitmap-stretches the
// window on scaled displays. A static initializer runs at process load, i.e.
// before main()/glfwInit — safe in any module, static init precedes main().
struct DpiAwarenessBoot {
    DpiAwarenessBoot() {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (DPI_AWARENESS_CONTEXT)-4.
        SetProcessDpiAwarenessContext(reinterpret_cast<DPI_AWARENESS_CONTEXT>(
            static_cast<std::intptr_t>(-4)));
    }
};
DpiAwarenessBoot g_dpiBoot;

// 加载 shell32.dll 的 ShellExecuteW（只加载一次）。openUrl / openFile /
// openContainingFolder 都经它异步拉起浏览器/程序/资源管理器：ShellExecuteW
// 立即返回、不等待目标进程退出。之前用 std::system("explorer …") 会在 UI
// 线程同步等 Explorer 窗口关闭，导致点击后渲染卡住。
using ShellExecuteFn = HINSTANCE(WINAPI*)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
const ShellExecuteFn& shellExecFn() {
    static const ShellExecuteFn fn = []() -> ShellExecuteFn {
        HMODULE m = LoadLibraryW(L"shell32.dll");
        if (!m) return nullptr;
        return reinterpret_cast<ShellExecuteFn>(
            reinterpret_cast<void*>(GetProcAddress(m, "ShellExecuteW")));
    }();
    return fn;
}
#endif

} // namespace

// Open the native folder picker; returns the chosen path or an empty path if
// the user cancelled (caller keeps the manual input).
//   Windows: SHBrowseForFolder (shell32/ole32 loaded dynamically).
//   Linux:   zenity (kdialog fallback).
//   macOS:   osascript 'choose folder'.
export std::filesystem::path pickDownloadFolder() {
#ifdef _WIN32
    using BrowseFn = LPITEMIDLIST(WINAPI*)(BROWSEINFOW*);
    using GetPathFn = BOOL(WINAPI*)(LPCITEMIDLIST, LPWSTR);
    using CoTaskMemFreeFn = void(WINAPI*)(void*);
    static const BrowseFn browse = []() -> BrowseFn {
        HMODULE m = LoadLibraryW(L"shell32.dll");
        if (!m) return nullptr;
        return reinterpret_cast<BrowseFn>(
            reinterpret_cast<void*>(GetProcAddress(m, "SHBrowseForFolderW")));
    }();
    static const GetPathFn getPath = []() -> GetPathFn {
        HMODULE m = LoadLibraryW(L"shell32.dll");
        if (!m) return nullptr;
        return reinterpret_cast<GetPathFn>(
            reinterpret_cast<void*>(GetProcAddress(m, "SHGetPathFromIDListW")));
    }();
    static const CoTaskMemFreeFn coFree = []() -> CoTaskMemFreeFn {
        HMODULE m = LoadLibraryW(L"ole32.dll");
        if (!m) return nullptr;
        return reinterpret_cast<CoTaskMemFreeFn>(
            reinterpret_cast<void*>(GetProcAddress(m, "CoTaskMemFree")));
    }();
    if (!browse || !getPath || !coFree) return {};

    HWND parent = nullptr;
    if (GLFWwindow* ctx = glfwGetCurrentContext()) {
        parent = glfwGetWin32Window(ctx);
    }
    BROWSEINFOW bi{};
    bi.hwndOwner = parent;
    bi.lpszTitle = L"选择下载目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = browse(&bi);
    if (!pidl) return {};
    std::filesystem::path result;
    wchar_t buf[32768];
    if (getPath(pidl, buf)) result = buf;
    coFree(pidl);
    return result;
#else
    // POSIX: shell out to a native picker (works best on a desktop session;
    // headless/absent tools → empty path → manual input).
    const char* cmd = nullptr;
#ifdef __APPLE__
    cmd = "osascript -e 'POSIX path of (choose folder)' 2>/dev/null";
#else
    cmd = "zenity --file-selection --directory 2>/dev/null "
          "|| kdialog --getexistingdirectory 2>/dev/null";
#endif
    FILE* pipe = ::popen(cmd, "r");
    if (!pipe) return {};
    std::string out;
    char buf[4096];
    while (::fgets(buf, sizeof(buf), pipe)) out += buf;
    ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (out.empty()) return {};
    return std::filesystem::path(out);
#endif
}

// 用系统默认程序打开文件 / 打开所在文件夹（立即返回，不阻塞 UI 线程）。
export void openFile(const std::filesystem::path& path) {
#ifdef _WIN32
    if (const ShellExecuteFn shellExec = shellExecFn(); shellExec) {
        const std::wstring wpath = path.wstring();
        shellExec(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    // 兜底：start 启动后 cmd 立即退出，std::system 不会长等。
    std::system(("start \"\" \"" + path.string() + "\"").c_str());
#else
    std::system(("xdg-open \"" + path.string() + "\" >/dev/null 2>&1 &").c_str());
#endif
}

export void openContainingFolder(const std::filesystem::path& path) {
#ifdef _WIN32
    if (const ShellExecuteFn shellExec = shellExecFn(); shellExec) {
        // explorer.exe 直接带 /select 参数；ShellExecuteW 拉起即返回，不等窗口关闭。
        const std::wstring params = L"/select,\"" + path.wstring() + L"\"";
        shellExec(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }
    std::system(("start \"\" explorer /select,\"" + path.string() + "\"").c_str());
#else
    std::system(("xdg-open \"" + path.parent_path().string() + "\" >/dev/null 2>&1 &").c_str());
#endif
}

// 用系统默认浏览器打开 URL（跨平台，不阻塞 UI 线程）。
export void openUrl(const std::string& url) {
#ifdef _WIN32
    // ShellExecuteW 立即返回、不 spawn shell —— 之前用 std::system("start ...")
    // 会在 UI 线程同步等待 cmd/浏览器启动，导致点击链接后卡几秒。
    if (const ShellExecuteFn shellExec = shellExecFn(); shellExec) {
        const std::wstring wurl(url.begin(), url.end());
        shellExec(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    std::system(("start \"\" \"" + url + "\"").c_str());
#elif defined(__APPLE__)
    // open 命令立即返回，shell 开销很小，可接受。
    std::system(("open \"" + url + "\"").c_str());
#else
    std::system(("xdg-open \"" + url + "\" >/dev/null 2>&1 &").c_str());
#endif
}

// 读取系统剪贴板文本（best-effort：无 GLFW 上下文时返回空）。用于打开添加下载
// 弹窗时自动检测剪贴板里的下载链接。GLFW 的字符串在下次剪贴板操作前有效，
// 立即拷贝进 std::string。
export std::string getClipboardText() {
    if (GLFWwindow* ctx = glfwGetCurrentContext()) {
        if (const char* text = glfwGetClipboardString(ctx)) {
            return std::string(text);
        }
    }
    return {};
}

// 下载完成/失败的系统通知（best-effort：工具缺失时静默，不阻塞 UI 线程）。
//   Windows: PowerShell NotifyIcon 气泡（独立进程 + CREATE_NO_WINDOW）。
//   macOS:   osascript 'display notification'。
//   Linux:   notify-send（无则 kdialog --passivepopup 兜底）。
export void notifyDownload(const std::string& title, const std::string& message) {
#ifdef _WIN32
    // PowerShell 单引号字符串：内容里嵌 ' 双写；剔除 cmd 特殊字符，避免破坏
    // -Command "..." 的引号解析（下载文件名的这类字符极少见）。
    auto psArg = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '\'') { out += "''"; continue; }
            if (c == '"' || c == '%' || c == '&' || c == '|' || c == '^') {
                out += ' ';
                continue;
            }
            out += c;
        }
        return out;
    };
    const std::string script =
        "Add-Type -AssemblyName System.Windows.Forms;"
        "$n=New-Object System.Windows.Forms.NotifyIcon;"
        "$n.Icon=[System.Drawing.SystemIcons]::Information;"
        "$n.Visible=$true;"
        "$n.ShowBalloonTip(3000,'" + psArg(title) + "','" + psArg(message) + "',"
        "[System.Windows.Forms.ToolTipIcon]::Info);"
        "Start-Sleep -Seconds 4;$n.Dispose()";
    // 独立进程启动 PowerShell，主进程不等它（避免 UI 卡 4 秒）。
    const std::wstring cmdline =
        L"powershell.exe -NoProfile -WindowStyle Hidden -Command \"" +
        std::wstring(script.begin(), script.end()) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, const_cast<wchar_t*>(cmdline.c_str()), nullptr, nullptr,
                       FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#elif defined(__APPLE__)
    // AppleScript 字符串：内容里的 " 和 \ 转义；& 后台化避免阻塞。
    auto asEsc = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out;
    };
    std::system(("osascript -e 'display notification \"" + asEsc(message) +
                 "\" with title \"" + asEsc(title) + "\"' >/dev/null 2>&1 &").c_str());
#else
    // POSIX shell 单引号引用（' 内嵌 ' 用 '\'' 转义），彻底避免 shell 注入。
    auto shq = [](const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += "'";
        return out;
    };
    std::system(("notify-send -a TinyNext " + shq(title) + " " + shq(message) +
                 " || kdialog --title " + shq(title) + " --passivepopup " + shq(message) +
                 " 3 >/dev/null 2>&1 &").c_str());
#endif
}
