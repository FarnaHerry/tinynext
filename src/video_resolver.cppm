// video_resolver.cppm — 视频网页地址解析器（yt-dlp 外挂进程）。
//
// 职责：把 bilibili 等视频网页链接解析成可下载的音视频流直链。aria2-next 仍是
// 唯一下载引擎——本模块只「解析」，spawn engines/yt-dlp(.exe) 跑
// `--dump-single-json`，从 stdout 读 JSON、拆出标题与各画质流地址 + 所需 HTTP 头
// （b 站 CDN 强制 Referer，否则 403）。解析结果是纯数据，交给 TaskStore/engine 下载。
//
// 领域层模块：不 import 任何 ui.*/eui。resolveVideoUrl 同步阻塞（yt-dlp 是
// PyInstaller onefile，冷启动可能 10-30s），UI 层须用 core::async 调到后台线程。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>   // pid_t
#include <sys/wait.h>    // waitpid, WNOHANG
#include <signal.h>      // kill, SIGKILL
#include <spawn.h>       // posix_spawn
#include <fcntl.h>       // fcntl, O_NONBLOCK, open
#include <unistd.h>      // pipe/read/close/usleep
#ifdef __APPLE__
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif
// macOS 的 <unistd.h> 不声明 environ（glibc 会）；posix_spawn 需要，这里显式声明。
extern char** environ;
#endif

export module tinynext.video_resolver;

import std;
import nlohmann.json;
import tinynext.config;   // cfg::configDir（cookie 临时文件 / stderr 日志）
import tinynext.i18n;     // tr（错误文案按语言）
import tinynext.utils;    // utf8FromPath（yt-dlp 的 -o 路径要 UTF-8 串）

namespace video {

// 单个流的 HTTP 头（来自 yt-dlp format.http_headers）。
export struct StreamHeaders {
    std::string userAgent;            // User-Agent
    std::string referer;              // Referer（b 站必须）
    std::vector<std::string> extra;   // 其余 "Key: Value" 行（如 Cookie）
};

// 一个可下载画质选项。合流（音视频同文件）时 audioUrl 为空、免合并。
export struct VideoFormat {
    std::string formatId;             // yt-dlp format_id
    std::string label;                // 显示名（format_note 或分辨率，如 "1080P 高清"）
    std::string ext;                  // 容器后缀（mp4/flv/m4s…），合流单文件命名用
    std::string vcodec;               // 视频编码（avc1/hev1/av01…），同高去重时优先 avc1
    int height = 0;                   // 视频高度（排序用）；0 = 未知
    std::int64_t filesizeApprox = -1; // 估算大小；-1 = 未知
    std::string videoUrl;             // 视频流直链（合流时即完整文件）
    std::string audioUrl;             // 音频流直链；空 = 合流（单文件，免 ffmpeg）
    std::string audioExt;             // 配对音频流容器（mp4/m4a/webm…）：合并时决定是否转码
    std::string audioCodec;           // 配对音频流编码（aac/opus/vorbis…）：mp4 装得下则 copy
    bool rangeBootstrap = false;      // 流所在 CDN 拒绝对开放式 Range（如 YouTube 的
                                      // googlevideo 对 bytes=0- 的首次请求回 403）：下载时须
                                      // 以有限分段 Range 引导再续拉，且只能单连接。
    StreamHeaders headers;
};

export struct VideoInfo {
    std::string title;
    std::string thumbnailUrl;
    std::string webpageUrl;
    std::vector<VideoFormat> formats;
};

export struct ResolveResult {
    bool ok = false;
    bool canceled = false;        // 用户主动取消（与失败区分，UI 不弹错误）
    std::string error;            // 失败原因（UI 直接显示）
    std::optional<VideoInfo> info;
};

// yt-dlp 下载任务的实时进度。所有字段都是原子变量，可在任意线程安全读写。
export struct YtDlpProgress {
    std::atomic<bool> started{false};    // 进程已启动
    std::atomic<bool> finished{false};   // 进程已退出（成功或失败）
    std::atomic<bool> canceled{false};   // 用户主动取消
    std::atomic<bool> ok{false};         // 退出码 0
    std::atomic<bool> merging{false};    // 进入 yt-dlp [Merger] 阶段（DASH 合并中）
    std::atomic<double> percent{0.0};    // [download] 百分比（0-100）
    std::atomic<std::int64_t> speedBps{0};
    std::atomic<std::int64_t> downloadedBytes{0};
    std::atomic<std::int64_t> totalBytes{0};
    std::string error;                   // 非原子：仅在 finished 后读
    std::string outputPath;              // 最终输出文件路径
};

namespace {

// 进程捕获结果。
struct CapturedProc {
    int exitCode = -1;
    std::string out;        // stdout（JSON）
    bool timedOut = false;
    bool canceled = false;  // 用户主动取消
};

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
// CreateProcessW 命令行参数加引号（含空格/特殊字符时）。
std::wstring quoteArg(const std::string& s) {
    std::wstring w = utf8ToWide(s);
    std::wstring out = L"\"";
    for (wchar_t c : w) {
        if (c == L'"') out += L"\\\"";
        else out += c;
    }
    out += L"\"";
    return out;
}
#endif

// spawn 进程并捕获 stdout（stderr 重定向到 stderrFile 供报错）。带超时：轮询
// 管道 + 进程退出，超时则强杀。cancel 非空时轮询其中置位即主动终止进程（供取消
// 操作用）。返回 stdout 全文与退出码。
CapturedProc runCapture(const std::string& exe,
                        const std::vector<std::string>& args,
                        const std::filesystem::path& stderrFile,
                        int timeoutSec,
                        std::atomic<bool>* cancel = nullptr) {
    CapturedProc result;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return result;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);  // 读端不遗传

    // stderr → 文件（可继承句柄）。
    HANDLE errFile = CreateFileW(utf8ToWide(stderrFile.string()).c_str(),
                                 GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstring cmd = quoteArg(exe);
    for (const auto& a : args) { cmd += L" "; cmd += quoteArg(a); }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = errFile != INVALID_HANDLE_VALUE ? errFile : writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);  // 父进程关闭写端，才能读到 EOF
    if (errFile != INVALID_HANDLE_VALUE) CloseHandle(errFile);
    if (!ok) { CloseHandle(readPipe); return result; }

    const DWORD deadline = GetTickCount() + (DWORD)timeoutSec * 1000;
    bool exited = false;
    for (;;) {
        if (cancel && cancel->load()) {
            TerminateProcess(pi.hProcess, 1);
            result.canceled = true;
            break;
        }
        DWORD avail = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[8192];
            DWORD got = 0;
            const DWORD want = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
            if (ReadFile(readPipe, buf, want, &got, nullptr) && got > 0) {
                result.out.append(buf, got);
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) { exited = true; break; }
        if (GetTickCount() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            result.timedOut = true;
            break;
        }
        Sleep(15);
    }
    // 进程退出后再尽力排空管道里剩余数据。
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        char buf[8192];
        DWORD got = 0;
        const DWORD want = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
        if (!ReadFile(readPipe, buf, want, &got, nullptr) || got == 0) break;
        result.out.append(buf, got);
    }
    if (exited) {
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exitCode = (int)code;
    }
    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return result;
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) return result;
    const int errFd = open(stderrFile.string().c_str(),
                           O_WRONLY | O_CREAT | O_TRUNC, 0644);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    if (errFd >= 0) posix_spawn_file_actions_adddup2(&fa, errFd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);

    std::vector<std::string> argStorage;
    argStorage.push_back(exe);
    for (const auto& a : args) argStorage.push_back(a);
    std::vector<char*> argv;
    argv.reserve(argStorage.size() + 1);
    for (auto& s : argStorage) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (errFd >= 0) close(errFd);
    if (rc != 0) { close(pipefd[0]); return result; }

    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    int status = 0;
    bool exited = false;
    for (;;) {
        if (cancel && cancel->load()) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.canceled = true;
            break;
        }
        char buf[8192];
        const ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) { result.out.append(buf, (std::size_t)n); continue; }
        if (waitpid(pid, &status, WNOHANG) == pid) { exited = true; break; }
        if (std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.timedOut = true;
            break;
        }
        usleep(15000);
    }
    // 排空剩余。
    for (;;) {
        char buf[8192];
        const ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;
        result.out.append(buf, (std::size_t)n);
    }
    close(pipefd[0]);
    if (exited && WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    return result;
#endif
}

// 解析 yt-dlp stderr 进度行，更新共享进度结构。
void parseProgressLine(const std::string& line, std::shared_ptr<YtDlpProgress> prog) {
    // [download]  12.3% of  500.00MiB at  862.37KiB/s ETA 02:09:52
    if (line.find("[download]") != std::string::npos) {
        // 百分比
        std::size_t pctPos = line.find('%');
        if (pctPos != std::string::npos) {
            const std::size_t start = line.rfind(' ', pctPos - 1);
            if (start != std::string::npos) {
                try { prog->percent.store(std::stod(line.substr(start + 1))); }
                catch (...) {}
            }
        }
        // 速度
        const std::size_t atPos = line.find(" at ");
        if (atPos != std::string::npos) {
            const std::size_t slashPos = line.find("/s", atPos + 4);
            if (slashPos != std::string::npos) {
                std::string speedStr = line.substr(atPos + 4, slashPos - atPos - 4);
                speedStr.erase(0, speedStr.find_first_not_of(" \t"));
                double speedVal = 0;
                try { speedVal = std::stod(speedStr); } catch (...) {}
                if (speedStr.rfind("GiB") != std::string::npos || speedStr.rfind("G") != std::string::npos) {
                    prog->speedBps.store(static_cast<std::int64_t>(speedVal * 1024.0 * 1024.0 * 1024.0));
                } else if (speedStr.rfind("MiB") != std::string::npos || speedStr.rfind("M") != std::string::npos) {
                    prog->speedBps.store(static_cast<std::int64_t>(speedVal * 1024.0 * 1024.0));
                } else if (speedStr.rfind("KiB") != std::string::npos || speedStr.rfind("k") != std::string::npos) {
                    prog->speedBps.store(static_cast<std::int64_t>(speedVal * 1024.0));
                } else {
                    prog->speedBps.store(static_cast<std::int64_t>(speedVal));
                }
            }
        }
        // 总量
        const std::size_t ofPos = line.find(" of ");
        if (ofPos != std::string::npos) {
            const std::size_t atPos2 = line.find(" at ", ofPos);
            std::string sizeStr = atPos2 != std::string::npos
                ? line.substr(ofPos + 4, atPos2 - ofPos - 4)
                : line.substr(ofPos + 4);
            sizeStr.erase(0, sizeStr.find_first_not_of(" \t"));
            double sizeVal = 0;
            try { sizeVal = std::stod(sizeStr); } catch (...) {}
            std::int64_t total = 0;
            if (sizeStr.rfind("GiB") != std::string::npos) total = static_cast<std::int64_t>(sizeVal * 1024.0 * 1024.0 * 1024.0);
            else if (sizeStr.rfind("MiB") != std::string::npos) total = static_cast<std::int64_t>(sizeVal * 1024.0 * 1024.0);
            else if (sizeStr.rfind("KiB") != std::string::npos) total = static_cast<std::int64_t>(sizeVal * 1024.0);
            else total = static_cast<std::int64_t>(sizeVal);
            if (total > 0) prog->totalBytes.store(total);
        }
        // 已下载 = 百分比 × 总量
        const std::int64_t tb = prog->totalBytes.load();
        if (tb > 0) {
            prog->downloadedBytes.store(static_cast<std::int64_t>(tb * prog->percent.load() / 100.0));
        }
    }
    // [Merger] Merging formats into "..."
    else if (line.find("[Merger]") != std::string::npos) {
        prog->merging.store(true);
        const std::size_t q1 = line.find('"');
        if (q1 != std::string::npos) {
            const std::size_t q2 = line.find('"', q1 + 1);
            if (q2 != std::string::npos) {
                prog->outputPath = line.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }
    // [download] Destination: "..."
    else if (line.find("Destination:") != std::string::npos) {
        const std::size_t q1 = line.find('"');
        if (q1 != std::string::npos) {
            const std::size_t q2 = line.find('"', q1 + 1);
            if (q2 != std::string::npos && prog->outputPath.empty()) {
                prog->outputPath = line.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }
}

// 带实时进度解析的子进程执行器：spawn 进程，逐行读取 stdout+stderr，解析进度行，
// 同时写入日志文件。支持取消（prog->canceled）。跨平台（Win32 / POSIX）。
void runCaptureYtDlp(const std::string& exe,
                     const std::vector<std::string>& args,
                     const std::filesystem::path& logFile,
                     std::shared_ptr<YtDlpProgress> prog) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        prog->finished.store(true); prog->ok.store(false); prog->error = "pipe creation failed";
        return;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE log = CreateFileW(utf8ToWide(logFile.string()).c_str(),
                             GENERIC_WRITE, FILE_SHARE_READ, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstring cmd = quoteArg(exe);
    for (const auto& a : args) { cmd += L" "; cmd += quoteArg(a); }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) { CloseHandle(readPipe); prog->finished.store(true); prog->ok.store(false); prog->error = "spawn failed"; return; }

    std::string lineBuf;
    bool exited = false;
    for (;;) {
        if (prog->canceled.load()) {
            TerminateProcess(pi.hProcess, 1);
            prog->finished.store(true);
            break;
        }
        DWORD avail = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[8192];
            DWORD got = 0;
            const DWORD want = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
            if (ReadFile(readPipe, buf, want, &got, nullptr) && got > 0) {
                // 写入日志
                log = CreateFileW(utf8ToWide(logFile.string()).c_str(),
                                  FILE_WRITE_DATA, FILE_SHARE_READ, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (log != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    WriteFile(log, buf, got, &written, nullptr);
                    CloseHandle(log);
                }
                // 分行解析
                for (DWORD i = 0; i < got; ++i) {
                    if (buf[i] == '\n') {
                        if (!lineBuf.empty()) {
                            parseProgressLine(lineBuf, prog);
                            lineBuf.clear();
                        }
                    } else if (buf[i] != '\r') {
                        lineBuf += buf[i];
                    }
                }
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) { exited = true; break; }
        Sleep(15);
    }
    // 排空剩余管道数据
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        char buf[8192]; DWORD got = 0;
        const DWORD want = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
        if (!ReadFile(readPipe, buf, want, &got, nullptr) || got == 0) break;
        for (DWORD i = 0; i < got; ++i) {
            if (buf[i] == '\n' && !lineBuf.empty()) { parseProgressLine(lineBuf, prog); lineBuf.clear(); }
            else if (buf[i] != '\r') lineBuf += buf[i];
        }
    }
    // 最后一行
    if (!lineBuf.empty()) parseProgressLine(lineBuf, prog);

    DWORD code = 1;
    if (exited) GetExitCodeProcess(pi.hProcess, &code);
    prog->finished.store(true);
    prog->ok.store(code == 0);
    if (code != 0 && prog->error.empty()) prog->error = "yt-dlp exit code " + std::to_string(code);
    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        prog->finished.store(true); prog->ok.store(false); prog->error = "pipe creation failed";
        return;
    }
    const int logFd = open(logFile.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);

    std::vector<std::string> argStorage;
    argStorage.push_back(exe);
    for (const auto& a : args) argStorage.push_back(a);
    std::vector<char*> argv;
    argv.reserve(argStorage.size() + 1);
    for (auto& s : argStorage) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (logFd >= 0) close(logFd);
    if (rc != 0) { close(pipefd[0]); prog->finished.store(true); prog->ok.store(false); prog->error = "spawn failed"; return; }

    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    std::string lineBuf;
    int status = 0;
    bool exited = false;
    for (;;) {
        if (prog->canceled.load()) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            prog->finished.store(true);
            break;
        }
        char buf[8192];
        const ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
            // 写入日志
            const int logFdW = open(logFile.string().c_str(), O_WRONLY | O_APPEND, 0644);
            if (logFdW >= 0) { write(logFdW, buf, (std::size_t)n); close(logFdW); }
            // 分行解析
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') { if (!lineBuf.empty()) { parseProgressLine(lineBuf, prog); lineBuf.clear(); } }
                else if (buf[i] != '\r') lineBuf += buf[i];
            }
            continue;
        }
        if (waitpid(pid, &status, WNOHANG) == pid) { exited = true; break; }
        usleep(15000);
    }
    // 排空
    for (;;) {
        char buf[8192];
        const ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n' && !lineBuf.empty()) { parseProgressLine(lineBuf, prog); lineBuf.clear(); }
            else if (buf[i] != '\r') lineBuf += buf[i];
        }
    }
    if (!lineBuf.empty()) parseProgressLine(lineBuf, prog);
    close(pipefd[0]);

    const bool exitedOk = exited && WIFEXITED(status);
    prog->finished.store(true);
    prog->ok.store(exitedOk && WEXITSTATUS(status) == 0);
    if (!exitedOk && prog->error.empty()) prog->error = "yt-dlp exited abnormally";
#endif
}

// 把 SESSDATA 写成 Netscape cookie 文件（yt-dlp --cookies 用）。返回路径；空值返回空。
std::filesystem::path writeCookieFile(const std::string& sessdata) {
    if (sessdata.empty()) return {};
    const std::filesystem::path p = cfg::configDir() / "tinynext-yt-dlp-cookies.txt";
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return {};
    out << "# Netscape HTTP Cookie File\n"
        << ".bilibili.com\tTRUE\t/\tFALSE\t2147483647\tSESSDATA\t" << sessdata << "\n";
    return p;
}

// 从 yt-dlp format 的 http_headers 对象拆出 UA / Referer / 其余头。
StreamHeaders parseHeaders(const nlohmann::json& h) {
    StreamHeaders sh;
    if (!h.is_object()) return sh;
    for (auto it = h.begin(); it != h.end(); ++it) {
        if (!it.value().is_string()) continue;
        const std::string key = it.key();
        const std::string val = it.value().get<std::string>();
        std::string lower = key;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower == "user-agent") sh.userAgent = val;
        else if (lower == "referer") sh.referer = val;
        else sh.extra.push_back(key + ": " + val);
    }
    return sh;
}

// yt-dlp 的 JSON 大量字段是显式 null（如 "filesize": null）——nlohmann 的
// .value() 只对「键不存在」回退默认，键在而值为 null 会 get<T>() 抛
// type_error。统一经此助手取值：不存在 / null / 类型不符 都回退默认。
template <typename T>
T jsonValue(const nlohmann::json& j, const char* key, T fallback) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    } catch (...) {
        return fallback;
    }
}

// 解析 yt-dlp -J 的 JSON 成 VideoInfo。失败返回错误串。
ResolveResult parseJson(const std::string& text) {
    ResolveResult rr;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        rr.error = std::string(tr("vres.invalid_data")) + e.what();
        return rr;
    }

    VideoInfo info;
    info.title = jsonValue<std::string>(j, "title", "");
    info.thumbnailUrl = jsonValue<std::string>(j, "thumbnail", "");
    info.webpageUrl = jsonValue<std::string>(j, "webpage_url", "");

    // 先挑出最佳音频流（DASH 配对用）：最高 abr，退化最高 tbr。
    const auto fmtIt = j.find("formats");
    const nlohmann::json formats =
        (fmtIt != j.end() && fmtIt->is_array()) ? *fmtIt : nlohmann::json::array();
    const nlohmann::json* bestAudio = nullptr;
    double bestAudioScore = -1.0;
    for (const auto& f : formats) {
        const std::string vcodec = jsonValue<std::string>(f, "vcodec", "none");
        const std::string acodec = jsonValue<std::string>(f, "acodec", "none");
        const bool audioOnly = (vcodec == "none" || vcodec.empty()) && !acodec.empty() && acodec != "none";
        const auto urlIt = f.find("url");
        if (!audioOnly || urlIt == f.end() || !urlIt->is_string()) continue;
        const double abr = jsonValue<double>(f, "abr", 0.0);
        const double score = abr > 0 ? abr : jsonValue<double>(f, "tbr", 0.0);
        if (score > bestAudioScore) { bestAudioScore = score; bestAudio = &f; }
    }

    for (const auto& f : formats) {
        const auto urlIt = f.find("url");
        if (urlIt == f.end() || !urlIt->is_string()) continue;
        const std::string vcodec = jsonValue<std::string>(f, "vcodec", "none");
        const std::string acodec = jsonValue<std::string>(f, "acodec", "none");
        const bool hasVideo = !vcodec.empty() && vcodec != "none";
        const bool hasAudio = !acodec.empty() && acodec != "none";
        if (!hasVideo) continue;  // 音频流不单独成选项（配对进视频选项）

        VideoFormat vf;
        vf.formatId = jsonValue<std::string>(f, "format_id", "");
        vf.ext = jsonValue<std::string>(f, "ext", "");
        vf.vcodec = vcodec;
        vf.videoUrl = urlIt->get<std::string>();
        vf.height = jsonValue<int>(f, "height", 0);
        vf.label = jsonValue<std::string>(f, "format_note", "");
        if (vf.label.empty()) vf.label = jsonValue<std::string>(f, "resolution", "");
        if (vf.label.empty() && vf.height > 0) vf.label = std::to_string(vf.height) + "P";
        if (vf.label.empty()) vf.label = vf.formatId;
        vf.filesizeApprox = jsonValue<std::int64_t>(f, "filesize", (std::int64_t)-1);
        if (vf.filesizeApprox < 0) vf.filesizeApprox = jsonValue<std::int64_t>(f, "filesize_approx", (std::int64_t)-1);
        const auto urlLikeGoogleCdn = [](const std::string& u) {
            return u.find("googlevideo.com") != std::string::npos;
        };
        vf.rangeBootstrap = urlLikeGoogleCdn(vf.videoUrl) || urlLikeGoogleCdn(vf.audioUrl);
        const auto hdrIt = f.find("http_headers");
        vf.headers = parseHeaders(hdrIt != f.end() && hdrIt->is_object()
                                      ? *hdrIt : nlohmann::json::object());
        // 视频流但无音轨（DASH）：配最佳音频流；合流则 audioUrl 留空（免合并）。
        // 音频的容器/编码一并记下——合并时判断能否 -c:a copy 进 mp4（aac 可、
        // YouTube 的 opus/vorbis 不能，需转 aac）。
        if (!hasAudio && bestAudio != nullptr) {
            vf.audioUrl = (*bestAudio)["url"].get<std::string>();
            vf.audioExt = jsonValue<std::string>(*bestAudio, "ext", "");
            vf.audioCodec = jsonValue<std::string>(*bestAudio, "acodec", "");
        }
        info.formats.push_back(std::move(vf));
    }

    // 按清晰度从高到低排序（同高按估算大小）。
    std::sort(info.formats.begin(), info.formats.end(),
              [](const VideoFormat& a, const VideoFormat& b) {
                  if (a.height != b.height) return a.height > b.height;
                  return a.filesizeApprox > b.filesizeApprox;
              });

    // 同一清晰度多种编码（b 站 avc1/hev1/av01 各一条）：只留一条，否则画质列表
    // 出现三条同名「1080P」。优先 avc1（H.264 兼容性最好），其次 hev1/hvc1，
    // 最后 av01 等；同编码取估算体积更大者（码率更高）。height 未知（0）不参与
    // 去重。
    auto codecRank = [](const std::string& vc) {
        if (vc.rfind("avc1", 0) == 0) return 0;
        if (vc.rfind("hev1", 0) == 0 || vc.rfind("hvc1", 0) == 0) return 1;
        return 2;
    };
    std::vector<VideoFormat> deduped;
    deduped.reserve(info.formats.size());
    for (auto& f : info.formats) {
        if (f.height <= 0) { deduped.push_back(std::move(f)); continue; }
        auto it = std::find_if(deduped.begin(), deduped.end(),
                               [&](const VideoFormat& e) { return e.height == f.height; });
        if (it == deduped.end()) { deduped.push_back(std::move(f)); continue; }
        const int newRank = codecRank(f.vcodec);
        const int oldRank = codecRank(it->vcodec);
        if (newRank < oldRank ||
            (newRank == oldRank && f.filesizeApprox > it->filesizeApprox)) {
            *it = std::move(f);
        }
    }
    info.formats = std::move(deduped);

    if (info.formats.empty()) {
        rr.error = tr("vres.no_stream_login");
        return rr;
    }
    rr.ok = true;
    rr.info = std::move(info);
    return rr;
}

} // namespace

// 前向声明（findEngineBinary 定义在后面，但 startYtDlpDownload 要先调用它）
export std::string findEngineBinary(const char* baseName);

// 启动 yt-dlp 下载（含 --downloader aria2c 委托 aria2 分片下载 + yt-dlp 自行 DASH
// 合并）。进程跑在独立后台线程，进度实时写入 prog。返回 true 表示成功启动。
export bool startYtDlpDownload(const std::string& url,
                               const std::string& jsRuntime,
                               const std::string& outName,
                               const std::filesystem::path& dir,
                               const std::string& userAgent,
                               const std::string& referer,
                               const std::filesystem::path& logFile,
                               std::shared_ptr<YtDlpProgress> prog) {
    const std::string exe = findEngineBinary("yt-dlp");
    if (exe.empty()) {
        prog->finished.store(true);
        prog->ok.store(false);
        prog->error = tr("vres.resolver_not_found");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::create_directories(logFile.parent_path(), ec);

    std::vector<std::string> args = {
        "--no-warnings", "--no-check-certificates", "--no-update",
        "--no-playlist", "--newline", "--progress", "--no-colors",
    };
    if (!jsRuntime.empty()) {
        args.push_back("--js-runtimes");
        args.push_back(jsRuntime);
    }
    // --downloader: 找 aria2c 或 aria2-next
    args.push_back("--downloader");
    std::string aria2c = findEngineBinary("aria2c");
    if (aria2c.empty()) aria2c = findEngineBinary("aria2-next");
    if (aria2c.empty()) aria2c = "aria2c";
    args.push_back(aria2c);
    // downloder-args 从配置读取分片数/连接数，与设置页「直链下载」栏一致
    const cfg::Aria2Config a2cfg = cfg::aria2Config();
    const std::string dlArgs = "aria2c:-x " + std::to_string(a2cfg.maxConnectionPerServer)
        + " -s " + std::to_string(a2cfg.split)
        + " --enable-rpc=false";
    args.push_back("--downloader-args");
    args.push_back(dlArgs);
    // 代理透传（与设置页「网络 → 代理地址」一致）
    const std::string proxy = a2cfg.proxy;
    if (!proxy.empty()) {
        args.push_back("--proxy");
        args.push_back(proxy);
    }
    // 请求头
    if (!userAgent.empty()) {
        args.push_back("--add-header");
        args.push_back("User-Agent: " + userAgent);
    }
    if (!referer.empty()) {
        args.push_back("--add-header");
        args.push_back("Referer: " + referer);
    }
    // 输出路径
    args.push_back("--paths");
    args.push_back(utf8FromPath(dir));
    args.push_back("-o");
    args.push_back(outName);
    args.push_back(url);

    std::thread([exe, args, logFile, prog] {
        prog->started.store(true);
        runCaptureYtDlp(exe, args, logFile, prog);
    }).detach();
    return true;
}

// 在 engines/ 下找外部工具二进制（复用 aria2 的 engineExePath 思路）：先
// <exeDir>/engines/，回退 <cwd>/engines/；POSIX 上再回退系统 PATH 里的同名工具
// （Linux/macOS 用户常已用包管理器装好 ffmpeg/yt-dlp，不必再放一份到 engines/）。
// Windows 自动补 .exe。导出给 video_merge（定位 ffmpeg）复用。
export std::string findEngineBinary(const char* baseName) {
    std::filesystem::path exeDir;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) exeDir = std::filesystem::path(buf).parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) exeDir = std::filesystem::path(buf).parent_path();
#else
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) exeDir = self.parent_path();
#endif
    std::string name = baseName;
#ifdef _WIN32
    name += ".exe";
#endif
    for (const std::filesystem::path& base : {exeDir, std::filesystem::current_path()}) {
        if (base.empty()) continue;
        const std::filesystem::path candidate = base / "engines" / name;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate.string();
    }
#ifndef _WIN32
    // 系统安装回退：/usr/bin、/usr/local/bin、/opt/homebrew/bin（macOS）。
    for (const char* dir : {"/usr/bin", "/usr/local/bin", "/opt/homebrew/bin"}) {
        const std::filesystem::path candidate = std::filesystem::path(dir) / name;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate.string();
    }
#endif
    return {};
}

// —— yt-dlp 的 JavaScript runtime（解 YouTube PO Token / JS challenge）——
// 2026 起 YouTube 强制要求 JS runtime，否则 player response playability 直接
// ERROR → "Video unavailable" / 原生下载失败。b 站等不需要，不受影响。
// 拼 `--js-runtimes <spec>`，其中 spec 是 `runtime:executable_path` 形式（yt-dlp
// 直接使用给定的可执行文件，不再自行在 PATH 里搜索）。优先配置手动指定的路径；
// 否则自动探测 PATH 里的 node/deno/quickjs/bun 可执行文件。

// 在 PATH 里搜索 JS runtime 可执行文件，返回 "runtime:executable_path" 规格。
std::string detectJsRuntimeSpecOnPath() {
#ifdef _WIN32
    const char* suffix = ".exe";
    const char sep = ';';
#else
    const char* suffix = "";
    const char sep = ':';
#endif
    // (yt-dlp runtime 名, 常见可执行文件基底)，按偏好序。
    const std::array<std::pair<const char*, const char*>, 5> candidates = {{
        {"node", "node"},
        {"deno", "deno"},
        {"bun", "bun"},
        {"quickjs", "qjs"},
        {"quickjs", "quickjs"},
    }};
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return {};
    std::vector<std::string> dirs;
    {
        std::string_view sv(pathEnv);
        std::size_t start = 0;
        for (;;) {
            const std::size_t end = sv.find(sep, start);
            dirs.emplace_back(sv.substr(start,
                end == std::string_view::npos ? std::string_view::npos : end - start));
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
    }
    for (const auto& [runtime, base] : candidates) {
        for (const auto& dir : dirs) {
            if (dir.empty()) continue;
            std::error_code ec;
            const std::filesystem::path p =
                std::filesystem::path(dir) / (std::string(base) + suffix);
            if (std::filesystem::exists(p, ec)) {
                return std::string(runtime) + ":" + utf8FromPath(p);
            }
        }
    }
    return {};
}

// 辅助函数：从可执行文件路径或基名检测运行时类型。
// 返回 yt-dlp 的 runtime 名称（如 "node"、"deno" 等），未知则返回空字符串。
std::string detectRuntimeFromPath(const std::filesystem::path& p) {
    // 用小写 stem（去扩展名）精确匹配常用 runtime 可执行文件名，避免
    // "notepad.exe" 之类含 "node" 子串的误判。
    const std::string stem = p.stem().string();
    std::string lower = stem;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<unsigned char>(std::tolower(c)); });

    if (lower == "node") return "node";
    if (lower == "deno") return "deno";
    if (lower == "bun") return "bun";
    if (lower == "qjs" || lower == "quickjs") return "quickjs";
    return {};
}

// 辅助函数：在目录中搜索 JS runtime 可执行文件。
// 返回 "runtime:executable_path" 规格，找不到则返回空字符串。
std::string detectRuntimeInDirectory(const std::filesystem::path& dir) {
    if (!std::filesystem::is_directory(dir)) return {};

#ifdef _WIN32
    const char* suffix = ".exe";
#else
    const char* suffix = "";
#endif
    // (yt-dlp runtime 名, 常见可执行文件基底)，按偏好序。
    const std::array<std::pair<const char*, const char*>, 5> candidates = {{
        {"node", "node"},
        {"deno", "deno"},
        {"bun", "bun"},
        {"quickjs", "qjs"},
        {"quickjs", "quickjs"},
    }};
    for (const auto& [runtime, base] : candidates) {
        const std::filesystem::path candidate = dir / (std::string(base) + suffix);
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return std::string(runtime) + ":" + utf8FromPath(candidate);
        }
    }
    return {};
}

// 规范化 JS runtime 规格：从配置项或自动探测得到可直接传给 yt-dlp 的 "runtime:path" 规格。
export std::string jsRuntimeSpec() {
    const std::string configured = cfg::videoConfig().jsRuntime;
    if (configured.empty()) return detectJsRuntimeSpecOnPath();

    // 基本格式验证：冒号只能出现在开头（且后面不能有冒号或无效字符）
    const std::size_t colonPos = configured.find(':');

    const std::filesystem::path candidate = pathFromUtf8(configured);
    std::error_code ec;

    // 情况1：配置的是显式规格（如 "node:C:\\tools\\node.exe"）
    if (colonPos == 0) {
        // 已经是 "runtime:path" 格式，直接返回
        return configured;
    }

    // 情况2：配置的是文件路径
    if (std::filesystem::is_regular_file(candidate, ec)) {
        // 检测运行时类型，而不是假设为 node
        const std::string runtime = detectRuntimeFromPath(candidate);
        if (!runtime.empty()) {
            return runtime + ":" + utf8FromPath(candidate);
        }
        // 未知文件类型，尝试作为 node 处理（向后兼容）
        return "node:" + utf8FromPath(candidate);
    }

    // 情况3：配置的是目录路径
    if (std::filesystem::is_directory(candidate, ec)) {
        const std::string found = detectRuntimeInDirectory(candidate);
        if (!found.empty()) {
            return found;
        }
        // 目录中找不到运行时，返回空（触发自动检测）
        return {};
    }

    // 情况4：配置的是运行时名称（如 "node"、"deno"）或无效路径
    // 检查是否是已知的运行时名称
    std::string lowerConfigured = configured;
    std::transform(lowerConfigured.begin(), lowerConfigured.end(), lowerConfigured.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lowerConfigured == "node" || lowerConfigured == "deno" || lowerConfigured == "bun" ||
        lowerConfigured == "quickjs" || lowerConfigured == "qjs") {
        // 已知运行时名称，直接返回（yt-dlp 会在 PATH 中搜索）
        return configured;
    }

    // 其他情况：可能是路径但不存在，或无效值，直接返回原始值让 yt-dlp 尝试
    return configured;
}

// 往 yt-dlp 命令 args 注入 `--js-runtimes <spec>`（有可用 runtime 才注入）。
void appendJsRuntimeArgs(std::vector<std::string>& args) {
    const std::string spec = jsRuntimeSpec();
    if (spec.empty()) return;
    args.push_back("--js-runtimes");
    args.push_back(spec);
}

// spawn 进程并把 stdout+stderr 重定向到日志文件，阻塞等待退出（带超时强杀）。cancel
// 非空时轮询其中置位即主动终止进程（供取消操作用）。返回退出码；超时/启动失败 -1。
export int runProcessLogged(const std::string& exe,
                            const std::vector<std::string>& args,
                            const std::filesystem::path& logFile,
                            int timeoutSec,
                            std::atomic<bool>* cancel = nullptr) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(utf8ToWide(logFile.string()).c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring cmd = quoteArg(exe);
    for (const auto& a : args) { cmd += L" "; cmd += quoteArg(a); }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log != INVALID_HANDLE_VALUE ? log : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = si.hStdOutput;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) return -1;
    int code = -1;
    const DWORD deadline = GetTickCount() + (DWORD)timeoutSec * 1000;
    for (;;) {
        if (cancel && cancel->load()) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) {
            DWORD c = 1;
            GetExitCodeProcess(pi.hProcess, &c);
            code = (int)c;
            break;
        }
        if (GetTickCount() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
#else
    const int logFd = open(logFile.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (logFd >= 0) {
        posix_spawn_file_actions_adddup2(&fa, logFd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, logFd, STDERR_FILENO);
    }
    std::vector<std::string> argStorage;
    argStorage.push_back(exe);
    for (const auto& a : args) argStorage.push_back(a);
    std::vector<char*> argv;
    argv.reserve(argStorage.size() + 1);
    for (auto& s : argStorage) argv.push_back(s.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (logFd >= 0) close(logFd);
    if (rc != 0) return -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    int status = 0;
    for (;;) {
        if (cancel && cancel->load()) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        usleep(50000);
    }
#endif
}

// 解析视频网页地址 → 标题 + 各画质流。sessdata 为空走匿名。proxy 非空时加
// --proxy 参数。cancel 置位时终止进程并返回 canceled=true。同步阻塞。
export ResolveResult resolveVideoUrl(const std::string& url, const std::string& sessdata,
                                     const std::string& proxy = "",
                                     std::atomic<bool>* cancel = nullptr) {
    ResolveResult rr;
    const std::string exe = findEngineBinary("yt-dlp");
    if (exe.empty()) {
        rr.error = tr("vres.resolver_not_found");
        return rr;
    }

    std::vector<std::string> args = {
        "--no-warnings", "--no-check-certificates",
        "--dump-single-json", "--no-update", "--no-playlist",
    };
    appendJsRuntimeArgs(args);
    if (!proxy.empty()) {
        args.push_back("--proxy");
        args.push_back(proxy);
    }
    // SESSDATA 只对 bilibili 主机有效，其它站点（YouTube 等）不挂 cookie 走匿名。
    const bool isBilibili = url.find("bilibili.com") != std::string::npos;
    const std::filesystem::path cookieFile =
        isBilibili ? writeCookieFile(sessdata) : std::filesystem::path{};
    if (!cookieFile.empty()) {
        args.push_back("--cookies");
        args.push_back(cookieFile.string());
    }
    args.push_back(url);

    const std::filesystem::path errFile = cfg::configDir() / "tinynext-yt-dlp-stderr.log";
    const CapturedProc proc = runCapture(exe, args, errFile, 60, cancel);
    if (proc.canceled) {
        rr.canceled = true;
        return rr;
    }
    if (proc.timedOut) {
        rr.error = tr("vres.timeout");
        return rr;
    }
    if (proc.exitCode != 0) {
        // 读 stderr 日志尾部作为错误原因。
        std::string errTail;
        std::ifstream in(errFile, std::ios::binary);
        if (in) {
            std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (all.size() > 512) all = all.substr(all.size() - 512);
            // 去掉控制字符，避免 UI 显示乱码。
            for (auto& c : all) { if ((unsigned char)c < 0x09) c = ' '; }
            errTail = all;
        }
        rr.error = errTail.empty()
            ? tr("vres.failed_login")
            : std::string(tr("vres.failed_prefix")) + errTail;
        return rr;
    }
    return parseJson(proc.out);
}

} // namespace video
