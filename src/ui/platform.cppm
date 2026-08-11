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
#include <shellapi.h>  // Shell_NotifyIconW / NIM_* / NIF_*（原生通知）
#include <dwmapi.h>    // DwmSetWindowAttribute（标题栏沉浸式深色模式）
#include <commdlg.h>   // GetOpenFileNameW（.torrent 文件选择器）
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

// UTF-8 → UTF-16。Windows 通知需要宽字符，逐字节扩宽（旧写法）会把中文变成乱码，
// 必须用 MultiByteToWideChar(CP_UTF8) 正确转码。
std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                            out.data(), n);
    }
    return out;
}

struct NotifyPayload {
    std::wstring title;
    std::wstring message;
};

// 独立线程跑一个 message-only 窗口 + Shell_NotifyIconW 气泡（约 4.5s 后自动移除）。
// 纯 Win32，不 spawn 任何命令行进程——之前用 `powershell -WindowStyle Hidden` 会被
// 杀软/主防当作恶意静默执行而拦截。
DWORD WINAPI notifyThreadProc(LPVOID param) {
    std::unique_ptr<NotifyPayload> payload(static_cast<NotifyPayload*>(param));
    const wchar_t* kClass = L"TinyNext.Notify";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    // 并发通知时类可能已注册（ERROR_CLASS_ALREADY_EXISTS）——此时仍可创建窗口，
    // 但只有真正注册成功的那一程才负责 UnregisterClassW。
    const bool registered = RegisterClassW(&wc) != 0 ||
                            GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

    HWND hwnd = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        if (registered) UnregisterClassW(kClass, wc.hInstance);
        return 1;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_INFO;
    nid.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_INFORMATION));
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout = 4000;
    wcsncpy_s(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), payload->title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, ARRAYSIZE(nid.szInfo), payload->message.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid);

    // 系统会自动收掉气泡；到时移除托盘图标。
    SetTimer(hwnd, 1, 4500, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.wParam == 1) {
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(hwnd, 1);
    DestroyWindow(hwnd);
    if (registered) UnregisterClassW(kClass, wc.hInstance);
    return 0;
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
    // 只当 zenity 不存在时才回退 kdialog：zenity 存在时用户点「取消」退出码非 0，
    // 用 `||` 会让 kdialog 再弹一次。先探测再选一个执行。
    cmd = "if command -v zenity >/dev/null 2>&1; then "
          "zenity --file-selection --directory 2>/dev/null; "
          "else kdialog --getexistingdirectory 2>/dev/null; fi";
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

// 打开文件选择器选一个 .torrent 种子文件；用户取消返回空路径。
//   Windows: GetOpenFileNameW（comdlg32 已链）
//   Linux:   zenity --file-selection（kdialog --getopenfilename 兜底）
//   macOS:   osascript 'choose file'
export std::filesystem::path pickTorrentFile() {
#ifdef _WIN32
    HWND parent = nullptr;
    if (GLFWwindow* ctx = glfwGetCurrentContext()) {
        parent = glfwGetWin32Window(ctx);
    }
    wchar_t szFile[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = L"BitTorrent 种子 (*.torrent)\0*.torrent\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择种子文件";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return {};
    return std::filesystem::path(szFile);
#else
    const char* cmd = nullptr;
#ifdef __APPLE__
    cmd = "osascript -e 'POSIX path of (choose file with prompt \"选择种子文件\")' 2>/dev/null";
#else
    // 与 pickDownloadFolder 同策略：先探测 zenity，缺失才回退 kdialog。
    cmd = "if command -v zenity >/dev/null 2>&1; then "
          "zenity --file-selection --file-filter='BitTorrent种子 *.torrent' 2>/dev/null; "
          "else kdialog --getopenfilename . '*.torrent' 2>/dev/null; fi";
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
        // 归一化成绝对原生路径：destPath 可能来自 aria2 的 files[0].path（正斜杠），
        // 或是先占位后轮询更新的猜测路径；explorer 的 /select 对无效/正斜杠路径
        // 解析不可靠，会回落到桌面。文件真实存在时用 /select 定位它；不存在
        // （占位/已移动）或本身是目录时直接打开目录本身。
        std::error_code ec;
        std::filesystem::path abs = std::filesystem::absolute(path, ec);
        abs = abs.lexically_normal();
        abs.make_preferred();  // / → \（Windows）
        if (std::filesystem::exists(abs, ec)) {
            if (std::filesystem::is_directory(abs, ec)) {
                // 目录（如 BT 多文件根目录）：直接打开它。
                shellExec(nullptr, L"open", abs.wstring().c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
            } else {
                const std::wstring params = L"/select,\"" + abs.wstring() + L"\"";
                shellExec(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr,
                          SW_SHOWNORMAL);
            }
        } else {
            // 文件不存在：打开其所在目录（不会回落到桌面）。
            shellExec(nullptr, L"open", abs.parent_path().wstring().c_str(), nullptr,
                      nullptr, SW_SHOWNORMAL);
        }
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

// 把文件/目录移到系统回收站（默认删除方式：可恢复，不永久删除）。best-effort。
//   Windows: SHFileOperationW + FOF_ALLOWUNDO（真正进回收站，动态加载 shell32）。
//   macOS:   osascript 调 Finder delete（移入废纸篓；路径经 argv 传入，规避
//            AppleScript 源码编码/转义问题）。
//   Linux:   gio trash（trash-put 兜底）。工具缺失返回 false，绝不直接 rm。
export bool moveToTrash(const std::filesystem::path& path) {
#ifdef _WIN32
    using ShFileOpFn = int(WINAPI*)(LPSHFILEOPSTRUCTW);
    static const ShFileOpFn shFileOp = []() -> ShFileOpFn {
        HMODULE m = LoadLibraryW(L"shell32.dll");
        if (!m) return nullptr;
        return reinterpret_cast<ShFileOpFn>(
            reinterpret_cast<void*>(GetProcAddress(m, "SHFileOperationW")));
    }();
    if (!shFileOp) return false;
    // pFrom 要求以双 null 结尾的宽字符串。
    std::wstring wide = path.wstring();
    std::vector<wchar_t> buf(wide.begin(), wide.end());
    buf.push_back(L'\0');
    buf.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = buf.data();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return shFileOp(&op) == 0;
#elif defined(__APPLE__)
    auto shq = [](const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += "'";
        return out;
    };
    return std::system(("osascript -e 'on run argv' "
                        "-e 'tell application \"Finder\" to delete POSIX file (item 1 of argv)' "
                        "-e 'end run' " + shq(path.string()) +
                        " >/dev/null 2>&1").c_str()) == 0;
#else
    auto shq = [](const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += "'";
        return out;
    };
    return std::system(("gio trash " + shq(path.string()) +
                        " 2>/dev/null || trash-put " + shq(path.string()) +
                        " >/dev/null 2>&1").c_str()) == 0;
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

// 让系统原生标题栏跟随深浅主题（Windows 沉浸式深色模式；其他平台 no-op）。
// 无法自绘无边框标题栏（eui-neo 不支持），用系统能力让边框配色跟主题一致。
export void setNativeTheme(bool dark) {
#ifdef _WIN32
    if (GLFWwindow* ctx = glfwGetCurrentContext()) {
        if (HWND hwnd = glfwGetWin32Window(ctx)) {
            const BOOL darkMode = dark ? TRUE : FALSE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                  &darkMode, sizeof(darkMode));
        }
    }
#else
    (void)dark;
#endif
}

// 加载应用图标（assets/icon.ico，来自 eui 默认图标）并设为窗口/任务栏图标。
// Windows 用原生 ICO + LoadImage + WM_SETICON，避免解码 PNG。
export void applyAppIcon() {
#ifdef _WIN32
    if (GLFWwindow* ctx = glfwGetCurrentContext()) {
        HWND hwnd = glfwGetWin32Window(ctx);
        if (!hwnd) return;
        wchar_t exe[MAX_PATH]{};
        const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return;
        const std::filesystem::path exeDir = std::filesystem::path(exe).parent_path();
        // 与 eui 的 assets 解析一致：exeDir/assets + CWD 相对多候选。
        const std::filesystem::path candidates[] = {
            exeDir / "assets" / "icon.ico",
            exeDir / "icon.ico",
            std::filesystem::path("assets") / "icon.ico",
            std::filesystem::path("..") / "assets" / "icon.ico",
            std::filesystem::path("..") / ".." / "assets" / "icon.ico",
        };
        for (const std::filesystem::path& ico : candidates) {
            if (!std::filesystem::exists(ico)) continue;
            HICON hIconBig = static_cast<HICON>(
                LoadImageW(nullptr, ico.c_str(), IMAGE_ICON, 0, 0,
                           LR_LOADFROMFILE | LR_DEFAULTSIZE));
            if (hIconBig) {
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIconBig));
            }
            HICON hIconSmall = static_cast<HICON>(
                LoadImageW(nullptr, ico.c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE));
            if (hIconSmall) {
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIconSmall));
            }
            break;
        }
    }
#else
    // POSIX：GLFW 窗口图标需要解码 PNG→RGBA，暂不做；后续可加。
#endif
}

// 下载完成/失败的系统通知（best-effort：工具缺失时静默，不阻塞 UI 线程）。
//   Windows: 原生 Shell_NotifyIconW 托盘气泡（独立线程 + message-only 窗口，
//            不 spawn 命令行，杀软不拦；UTF-8 → UTF-16 正确转码不乱码）。
//   macOS:   osascript 'display notification'（消息经 argv 传入，规避 AppleScript
//            源码编码/转义问题）。
//   Linux:   notify-send（无则 kdialog --passivepopup 兜底）。
export void notifyDownload(const std::string& title, const std::string& message) {
#ifdef _WIN32
    // 独立线程跑气泡，UI 线程不等待。
    auto* payload = new NotifyPayload{utf8ToWide(title), utf8ToWide(message)};
    HANDLE hThread = CreateThread(nullptr, 0, notifyThreadProc, payload, 0, nullptr);
    if (!hThread) {
        delete payload;
        return;
    }
    CloseHandle(hThread);
#elif defined(__APPLE__)
    // 消息/标题经 argv 传给 osascript（POSIX 单引号引用），避免把中文嵌进
    // AppleScript 源码导致编码/转义问题；& 后台化避免阻塞。
    auto shq = [](const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += "'";
        return out;
    };
    std::system(("osascript -e 'on run argv' "
                 "-e 'display notification (item 1 of argv) with title (item 2 of argv)' "
                 "-e 'end run' " + shq(message) + " " + shq(title) +
                 " >/dev/null 2>&1 &").c_str());
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
