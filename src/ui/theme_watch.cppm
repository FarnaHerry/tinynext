// ui/theme_watch.cppm — event-driven OS theme-change watcher.
//
// 取代 app.cpp 里旧的每帧 2s 轮询 cfg::osDark()。后台线程阻塞在操作系统自身的
// 主题变化通知上（零轮询、零子进程）：
//   * Linux   — inotify：~/.config/kdeglobals（KDE）、~/.config/dconf/user
//               （GNOME gsettings color-scheme）、gtk-3.0/4.0 的 settings.ini（GTK）
//   * Windows — message-only 窗口收 WM_SETTINGCHANGE / "ImmersiveColorSet" 广播
//   * macOS   — kqueue（EVFILT_VNODE）监听 ~/Library/Preferences/.GlobalPreferences.plist
//               （best-effort：cfprefsd 可能延迟刷盘；理想方案是
//               NSDistributedNotificationCenter，但那需要一小段 ObjC，本工程纯 C++ 模块）
//
// 线程模型：watcher 线程只置位 std::atomic<bool> g_themeDirty，不碰任何 UI 状态；
// 渲染线程每帧用 themeChangePending() 消费该 flag，命中时调一次 cfg::osDark()。
// g_dark 仍在渲染线程读写，无数据竞争。

module;

#if defined(__linux__)
#include <fcntl.h>          // fcntl / FD_CLOEXEC
#include <poll.h>           // poll / struct pollfd
#include <sys/inotify.h>    // inotify / struct inotify_event
#include <unistd.h>         // pipe / read / write / close
#elif defined(__APPLE__)
#include <cerrno>           // errno / EINTR（宏，import std 不导出宏）
#include <fcntl.h>          // O_EVTONLY / FD_CLOEXEC
#include <sys/event.h>      // kqueue / kevent / EV_SET / EVFILT_VNODE / NOTE_*
#include <unistd.h>         // pipe / read / write / close
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// eui 的 UI 唤醒：主题变化时唤醒 UI 一帧，让 compose 消费标志并重绘（跨线程安全）。
namespace core::platform { void requestUiUpdate(); }

export module tinynext.ui.theme_watch;

import std;

namespace {

// 主题变化待处理标记：watcher 线程置位，渲染线程 exchange 消费。
std::atomic<bool> g_themeDirty{false};

// watcher 线程专用：置位 + 唤醒 UI 一帧（否则 UI 睡眠时主题变化不会被消费）。
void markThemeDirty() {
    g_themeDirty.store(true);
    core::platform::requestUiUpdate();
}

// 后台 watcher 线程（std::jthread：进程退出时自动请求停止并 join）。
std::jthread g_watcher;

#if defined(__linux__) || defined(__APPLE__)
// 把 pipe 两端设成 close-on-exec，避免泄漏给 popen 子进程。（Windows 无 POSIX
// fcntl，走 PostThreadMessage 停止，不需要 pipe。）
void setCloexec(int fds[2]) {
    for (int fd : {fds[0], fds[1]}) {
        const int fl = ::fcntl(fd, F_GETFD);
        if (fl >= 0) ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
}
#endif

#if defined(__linux__)

void watchLoop(std::stop_token st) {
    int stopPipe[2];
    if (::pipe(stopPipe) != 0) return;
    setCloexec(stopPipe);
    // 停止：jthread 析构触发 request_stop → 向 pipe 写字节 → poll 返回。
    std::stop_callback stopCb(st, [stopPipe] {
        ssize_t r = ::write(stopPipe[1], "x", 1);
        (void)r;
    });

    const int inotifyFd = ::inotify_init1(IN_CLOEXEC);
    if (inotifyFd < 0) {
        ::close(stopPipe[0]);
        ::close(stopPipe[1]);
        return;
    }

    // 观察目录 → 关注的文件名。任一匹配事件（创建/改写/原子改名落位）都可能是主题
    // 变化，置 flag 让渲染线程重读 osDark()（重读本身全量探测，正确即可）。
    struct Watch {
        int wd;
        std::vector<std::string> names;
    };
    std::vector<Watch> watches;
    if (const char* home = std::getenv("HOME")) {
        const std::string base = std::string(home) + "/.config";
        const auto addDir = [&](const std::string& dir,
                                std::initializer_list<const char*> names) {
            const int wd = ::inotify_add_watch(
                inotifyFd, dir.c_str(),
                IN_CREATE | IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO);
            if (wd >= 0) {
                watches.push_back(
                    {wd, std::vector<std::string>(names.begin(), names.end())});
            }
        };
        addDir(base, {"kdeglobals"});              // KDE color scheme
        addDir(base + "/dconf", {"user"});         // GNOME gsettings color-scheme
        addDir(base + "/gtk-3.0", {"settings.ini"});
        addDir(base + "/gtk-4.0", {"settings.ini"});
    }

    struct pollfd fds[2];
    fds[0] = {inotifyFd, POLLIN, 0};
    fds[1] = {stopPipe[0], POLLIN, 0};

    std::array<char, 4096> buf;
    while (!st.stop_requested()) {
        const int rc = ::poll(fds, 2, -1);
        if (rc <= 0) continue;  // EINTR / 未知错误 → 继续等
        if (fds[1].revents & POLLIN) break;  // 收到停止信号
        if (!(fds[0].revents & POLLIN)) continue;
        const ssize_t n = ::read(inotifyFd, buf.data(),
                                 static_cast<size_t>(buf.size()));
        if (n <= 0) continue;
        ssize_t off = 0;
        while (off < n) {
            const auto* ev =
                reinterpret_cast<const inotify_event*>(buf.data() + off);
            if (ev->len > 0) {
                const std::string name(ev->name);
                for (const auto& w : watches) {
                    if (w.wd != ev->wd) continue;
                    for (const auto& target : w.names) {
                        if (target == name) {
                            markThemeDirty();
                            break;
                        }
                    }
                }
            }
            off += static_cast<ssize_t>(sizeof(inotify_event)) + ev->len;
        }
    }
    ::close(inotifyFd);
    ::close(stopPipe[0]);
    ::close(stopPipe[1]);
}

#elif defined(__APPLE__)

void watchLoop(std::stop_token st) {
    int stopPipe[2];
    if (::pipe(stopPipe) != 0) return;
    setCloexec(stopPipe);
    std::stop_callback stopCb(st, [stopPipe] {
        ssize_t r = ::write(stopPipe[1], "x", 1);
        (void)r;
    });

    const int kq = ::kqueue();
    if (kq < 0) {
        ::close(stopPipe[0]);
        ::close(stopPipe[1]);
        return;
    }

    const char* home = std::getenv("HOME");
    const std::string plist = home
        ? std::string(home) + "/Library/Preferences/.GlobalPreferences.plist"
        : std::string();
    // AppleInterfaceStyle 写在这里；cfprefsd 可能延迟刷盘，best-effort。文件不
    // 存在时只阻塞在 stop pipe 上（此时 osDark() 默认浅色）。
    int fd = -1;
    if (!plist.empty()) fd = ::open(plist.c_str(), O_EVTONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct kevent ev;
        EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_VNODE,
               EV_ADD | EV_ENABLE | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_RENAME, 0, nullptr);
        ::kevent(kq, &ev, 1, nullptr, 0, nullptr);
    }
    struct kevent evPipe;
    EV_SET(&evPipe, static_cast<uintptr_t>(stopPipe[0]), EVFILT_READ,
           EV_ADD | EV_ENABLE, 0, 0, nullptr);
    ::kevent(kq, &evPipe, 1, nullptr, 0, nullptr);

    struct kevent evs[8];
    while (!st.stop_requested()) {
        const int n = ::kevent(kq, nullptr, 0, evs, 8, nullptr);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (evs[i].filter == EVFILT_VNODE) {
                markThemeDirty();
                // 文件被原子替换（rename/delete）：重挂 watch 到新 inode。
                if (evs[i].fflags & (NOTE_DELETE | NOTE_RENAME)) {
                    ::close(fd);
                    fd = ::open(plist.c_str(), O_EVTONLY | O_CLOEXEC);
                    if (fd >= 0) {
                        struct kevent ev;
                        EV_SET(&ev, static_cast<uintptr_t>(fd), EVFILT_VNODE,
                               EV_ADD | EV_ENABLE | EV_CLEAR,
                               NOTE_WRITE | NOTE_DELETE | NOTE_RENAME, 0,
                               nullptr);
                        ::kevent(kq, &ev, 1, nullptr, 0, nullptr);
                    }
                }
            } else if (evs[i].filter == EVFILT_READ) {
                return;  // stop pipe 有数据 → 停止
            }
        }
    }
    if (fd >= 0) ::close(fd);
    ::close(kq);
    ::close(stopPipe[0]);
    ::close(stopPipe[1]);
}

#elif defined(_WIN32)

LRESULT CALLBACK themeWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // WM_SETTINGCHANGE 是 sent 广播，由本窗口的 wndproc 收到（GetMessage 只负责 pump）。
    if (msg == WM_SETTINGCHANGE) markThemeDirty();
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void watchLoop(std::stop_token st) {
    const DWORD tid = ::GetCurrentThreadId();
    // 停止：PostThreadMessage(WM_QUIT) → GetMessage 返回 0 → 退出循环。
    std::stop_callback stopCb(st, [tid] { ::PostThreadMessageW(tid, WM_QUIT, 0, 0); });

    WNDCLASSW wc{};
    wc.lpfnWndProc = themeWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TinyNextThemeWatcher";
    ::RegisterClassW(&wc);

    // 隐藏 message-only 窗口：系统把 WM_SETTINGCHANGE / "ImmersiveColorSet" 广播到
    // 所有顶层窗口，message-only 窗口同样能收到（Chromium 等通用做法）。
    const HWND hwnd = ::CreateWindowExW(
        0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
        wc.hInstance, nullptr);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // 只 pump；WM_SETTINGCHANGE 由 themeWndProc 处理。
    }
    if (hwnd) ::DestroyWindow(hwnd);
}

#endif

}  // namespace

// 启动主题 watcher（幂等：已启动则直接返回）。进程退出时 jthread 自动 join。
export void startThemeWatcher() {
    if (g_watcher.joinable()) return;
    g_watcher = std::jthread(watchLoop);
}

// 是否有待处理的主题变化事件（每帧调用一次，原子清掉标记）。返回 true 时调用方
// 应重读一次 cfg::osDark()。
export bool themeChangePending() {
    return g_themeDirty.exchange(false);
}
