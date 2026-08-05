// ui/platform.cppm — OS integration: native folder picker, open file / folder /
// URL, and the high-DPI-awareness boot. No EUI dependency (platform APIs + std).
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>  // BROWSEINFOW for the folder picker
// GLFW native access for the folder picker's parent HWND. GLFWwindow is opaque
// here (no eui_neo.h needed); linkage spec at namespace scope (global fragment).
struct GLFWwindow;
extern "C" GLFWwindow* glfwGetCurrentContext();
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

// 用系统默认程序打开文件 / 打开所在文件夹（后台执行，避免阻塞 UI 线程）。
export void openFile(const std::filesystem::path& path) {
#ifdef _WIN32
    std::system(("explorer \"" + path.string() + "\"").c_str());
#else
    std::system(("xdg-open \"" + path.string() + "\" >/dev/null 2>&1 &").c_str());
#endif
}

export void openContainingFolder(const std::filesystem::path& path) {
#ifdef _WIN32
    std::system(("explorer /select,\"" + path.string() + "\"").c_str());
#else
    std::system(("xdg-open \"" + path.parent_path().string() + "\" >/dev/null 2>&1 &").c_str());
#endif
}

// 用系统默认浏览器打开 URL（跨平台，不阻塞 UI 线程）。
export void openUrl(const std::string& url) {
#ifdef _WIN32
    // ShellExecuteW 立即返回、不 spawn shell —— 之前用 std::system("start ...")
    // 会在 UI 线程同步等待 cmd/浏览器启动，导致点击链接后卡几秒。
    using ShellExecuteFn = HINSTANCE(WINAPI*)(HWND, LPCWSTR, LPCWSTR,
                                              LPCWSTR, LPCWSTR, INT);
    static const ShellExecuteFn shellExec = []() -> ShellExecuteFn {
        HMODULE m = LoadLibraryW(L"shell32.dll");
        if (!m) return nullptr;
        return reinterpret_cast<ShellExecuteFn>(
            reinterpret_cast<void*>(GetProcAddress(m, "ShellExecuteW")));
    }();
    if (shellExec) {
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
