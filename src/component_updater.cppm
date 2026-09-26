// component_updater.cppm — aria2-next 引擎的应用内更新。
//
// 领域层模块：不 import 任何 ui.*/eui。所有网络/文件操作都在 updater 自己的
// 工作线程跑（引擎 downloadFile 回调在引擎线程，经条件变量转回工作线程）；
// UI 线程只经 snapshot() 读纯值拷贝。状态变化经注入的 wake 回调唤醒 UI
// （app.cpp 启动时注入 core::platform::requestUiUpdate）。
//
// 更新源（GitHub releases，随 release 发布 sha256 校验文件）：
//   AnInsomniacy/aria2-next —— 资产 aria2-next-<ver>-<os>-<arch>[.exe]，
//   校验文件 aria2-next-<ver>-checksums.sha256。
// 项目无 TLS/HTTP 客户端依赖：HTTPS 下载全部交给运行中的 aria2 daemon
// （dl::DownloadEngine::downloadFile 静默通道，不出下载卡片）。
//
// 更新 aria2-next 自身的顺序：下载 + 校验先做完，替换二进制经
// restartEngine(beforeRespawn) 在「daemon 已停、尚未重拉起」的窗口里执行
// （Windows 运行中的 exe 被文件锁占用）。
module;

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // 版本探测 spawn（CreateProcessW）/ GetModuleFileNameW
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

export module tinynext.component_updater;

import std;
import tinynext.config;          // configDir（版本探测的 stderr 日志）
import tinynext.download_engine; // dl::DownloadEngine（downloadFile/restartEngine）
import tinynext.i18n;            // tr（错误文案）

export namespace updater {

enum class CompStatus {
    Idle,             // 尚未检查
    Checking,         // 正在拉取 latest release 信息
    CheckFailed,      // 检查失败（网络/解析）
    UpToDate,         // 已是最新
    UpdateAvailable,  // 有新版本，可点「立即更新」
    Downloading,      // 下载新二进制/校验文件（progress 0-100）
    Verifying,        // sha256 校验中
    Replacing,        // 停 daemon/替换二进制/重启引擎
    Done,             // 更新完成
    Failed            // 更新失败（error 有原因）
};

// UI 读取用的纯值快照（内部状态有锁 + 工作线程写，导出的只有这份拷贝）。
struct ComponentSnapshot {
    std::string current;   // 当前版本（预热探测填充；探测失败/二进制缺失 = 空）
    std::string latest;    // 检查到的最新版本（未检查 = 空）
    CompStatus status = CompStatus::Idle;
    int progress = 0;      // Downloading 阶段 0-100
    std::string error;     // CheckFailed/Failed 的原因
};

ComponentSnapshot snapshot();
void setWakeUi(std::function<void()> fn);   // app.cpp 注入 requestUiUpdate
void probeAria2Version();   // 跑 aria2-next --version（后台线程调用，可能数秒）
void checkLatest(dl::DownloadEngine& eng);
void startUpdate(dl::DownloadEngine& eng);

} // namespace updater

namespace {

// ---- SHA-256（紧凑 public-domain 实现：只为校验一个文件，不值得引入 crypto 依赖）----

struct Sha256Ctx {
    std::uint32_t state[8];
    std::uint64_t bitlen = 0;
    std::uint8_t data[64]{};
    std::size_t datalen = 0;
};

constexpr std::uint32_t kSha256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

std::uint32_t rotr32(std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); }

void sha256Init(Sha256Ctx& c) {
    c.datalen = 0;
    c.bitlen = 0;
    c.state[0] = 0x6a09e667; c.state[1] = 0xbb67ae85;
    c.state[2] = 0x3c6ef372; c.state[3] = 0xa54ff53a;
    c.state[4] = 0x510e527f; c.state[5] = 0x9b05688c;
    c.state[6] = 0x1f83d9ab; c.state[7] = 0x5be0cd19;
}

void sha256Transform(Sha256Ctx& c, const std::uint8_t* data) {
    std::uint32_t m[64];
    for (int i = 0; i < 16; ++i) {
        m[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24) |
               (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr32(m[i - 15], 7) ^ rotr32(m[i - 15], 18) ^ (m[i - 15] >> 3);
        const std::uint32_t s1 = rotr32(m[i - 2], 17) ^ rotr32(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    std::uint32_t a = c.state[0], b = c.state[1], cc = c.state[2], d = c.state[3];
    std::uint32_t e = c.state[4], f = c.state[5], g = c.state[6], h = c.state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kSha256[i] + m[i];
        const std::uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        const std::uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.state[0] += a; c.state[1] += b; c.state[2] += cc; c.state[3] += d;
    c.state[4] += e; c.state[5] += f; c.state[6] += g; c.state[7] += h;
}

void sha256Update(Sha256Ctx& c, const std::uint8_t* data, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
        c.data[c.datalen++] = data[i];
        if (c.datalen == 64) {
            sha256Transform(c, c.data);
            c.bitlen += 512;
            c.datalen = 0;
        }
    }
}

void sha256Final(Sha256Ctx& c, std::uint8_t* hash) {
    std::size_t i = c.datalen;
    c.bitlen += c.datalen * 8;
    c.data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) c.data[i++] = 0;
        sha256Transform(c, c.data);
        i = 0;
    }
    while (i < 56) c.data[i++] = 0;
    for (int j = 0; j < 8; ++j) c.data[63 - j] = (c.bitlen >> (j * 8)) & 0xff;
    sha256Transform(c, c.data);
    for (int j = 0; j < 8; ++j) {
        hash[j * 4 + 0] = (c.state[j] >> 24) & 0xff;
        hash[j * 4 + 1] = (c.state[j] >> 16) & 0xff;
        hash[j * 4 + 2] = (c.state[j] >> 8) & 0xff;
        hash[j * 4 + 3] = c.state[j] & 0xff;
    }
}

std::string sha256FileHex(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    Sha256Ctx c;
    sha256Init(c);
    std::uint8_t buf[65536];
    while (in) {
        in.read(reinterpret_cast<char*>(buf), sizeof(buf));
        if (const auto n = in.gcount(); n > 0) {
            sha256Update(c, buf, static_cast<std::size_t>(n));
        }
    }
    std::uint8_t hash[32];
    sha256Final(c, hash);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint8_t b : hash) {
        out += kHex[b >> 4];
        out += kHex[b & 0x0f];
    }
    return out;
}

// sha256sum 格式行："<64hex> <space>[*| ]<filename>"。按文件名取 hash（小写）。
std::string extractHashFor(const std::filesystem::path& sumFile, const std::string& asset) {
    std::ifstream in(sumFile);
    if (!in) return {};
    std::string line;
    while (std::getline(in, line)) {
        const auto sp = line.find_first_of(" \t");
        if (sp == std::string::npos || sp != 64) continue;  // hash 固定 64 位
        std::string name = line.substr(sp + 1);
        if (const auto ns = name.find_first_not_of(" \t*"); ns != std::string::npos) {
            name = name.substr(ns);
        } else {
            continue;
        }
        while (!name.empty() &&
               (name.back() == '\r' || name.back() == '\n' || name.back() == ' ')) {
            name.pop_back();
        }
        if (name == asset) {
            std::string hash = line.substr(0, 64);
            std::ranges::transform(hash, hash.begin(),
                                   [](unsigned char ch) { return std::tolower(ch); });
            return hash;
        }
    }
    return {};
}

// ---- 组件元数据（仅 aria2-next）----

constexpr const char* kRepo = "AnInsomniacy/aria2-next";
constexpr const char* kBinary = "aria2-next";

std::string stripLeadingV(std::string tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    return tag;
}

// 发布资产名按编译期平台映射（我们只发 win64 / linux-x86_64 / macos-arm64）。
std::string assetNameOf(const std::string& ver) {
#ifdef _WIN32
    return "aria2-next-" + ver + "-windows-x86_64.exe";
#elif defined(__APPLE__)
    return "aria2-next-" + ver + "-macos-arm64";
#else
    return "aria2-next-" + ver + "-linux-x86_64";
#endif
}

std::string checksumAssetOf(const std::string& ver) {
    return "aria2-next-" + ver + "-checksums.sha256";
}

std::string apiUrl() {
    return std::string("https://api.github.com/repos/") + kRepo + "/releases/latest";
}

std::string downloadUrlOf(const std::string& tag, const std::string& asset) {
    return std::string("https://github.com/") + kRepo + "/releases/download/" +
           tag + "/" + asset;
}

// dotted numeric 版本比较（前导 v 已去）：2.6.7 < 2.6.8，前导 0 段按数值处理
// （"08"=8）。返回 -1/0/1。
int compareVersions(const std::string& a, const std::string& b) {
    auto splitNum = [](const std::string& s) {
        std::vector<long long> out;
        std::size_t start = 0;
        for (;;) {
            const auto dot = s.find('.', start);
            const auto part = s.substr(start, dot == std::string::npos
                                                  ? std::string::npos : dot - start);
            long long v = 0;
            for (const char ch : part) {
                if (ch < '0' || ch > '9') break;
                v = v * 10 + (ch - '0');
            }
            out.push_back(v);
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return out;
    };
    const auto va = splitNum(a);
    const auto vb = splitNum(b);
    for (std::size_t i = 0; i < std::max(va.size(), vb.size()); ++i) {
        const long long x = i < va.size() ? va[i] : 0;
        const long long y = i < vb.size() ? vb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// "Aria2 Next version 2.6.2\n..." → "2.6.2"。
std::string parseVersionAfterMarker(const std::string& out, std::string_view marker) {
    const auto pos = out.find(marker);
    if (pos == std::string::npos) return {};
    const std::size_t start = pos + marker.size();
    const auto end = out.find_first_of(" \r\n\t", start);
    return out.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// ---- 进程探测辅助（自旧 video_resolver 移植，仅 --version 探测用）----

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

// spawn 进程并捕获 stdout（stderr 重定向到 stderrFile 供报错），带超时强杀。
struct CapturedProc {
    int exitCode = -1;
    std::string out;        // stdout
    bool timedOut = false;
};

CapturedProc runCapture(const std::string& exe,
                        const std::vector<std::string>& args,
                        const std::filesystem::path& stderrFile,
                        int timeoutSec) {
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

// 在 engines/ 下找外部工具二进制（与 aria2_engine 的查找顺序一致）：先
// <exeDir>/engines/，回退 <cwd>/engines/；POSIX 上再回退系统 PATH 里的同名工具
// （Linux/macOS 用户常已用包管理器装好 aria2，不必再放一份到 engines/）。
// Windows 自动补 .exe。
std::string findEngineBinary(const char* baseName) {
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

// ---- 状态 ----

struct CompState {
    std::string current;
    std::string latest;
    std::string tag;    // release tag（含前导 v；拼下载 URL 用）
    updater::CompStatus status = updater::CompStatus::Idle;
    int progress = 0;
    std::string error;
    bool busy = false;  // 有操作在进行（检查/更新），忽略重复点击
};

std::mutex g_mu;
CompState g_state;
std::function<void()> g_wakeUi;

void wakeUi() {
    if (g_wakeUi) g_wakeUi();
}

// 锁内改状态、锁外唤醒 UI。
template <typename F>
void mutate(F&& fn) {
    {
        std::lock_guard lock(g_mu);
        fn(g_state);
    }
    wakeUi();
}

// 在工作线程上同步等待引擎静默下载完成（downloadFile 的回调在引擎线程触发，
// 经条件变量转回来）。onProgress 写进状态供 UI 显示百分比。
bool downloadSync(dl::DownloadEngine& eng, const std::string& url,
                  const std::filesystem::path& dest, std::string& errOut) {
    std::mutex mu;
    std::condition_variable cv;
    bool finished = false;
    bool ok = false;
    std::string err;
    eng.downloadFile(url, dest,
                     [](int p) {
                         mutate([p](CompState& s) { s.progress = p; });
                     },
                     [&](bool success, std::string e) {
                         {
                             std::lock_guard lock(mu);
                             finished = true;
                             ok = success;
                             err = std::move(e);
                         }
                         cv.notify_one();
                     });
    std::unique_lock lock(mu);
    cv.wait(lock, [&] { return finished; });
    errOut = std::move(err);
    return ok;
}

// 原子替换目标二进制：先 copy 到 <name>.new，再 remove + rename（POSIX rename
// 可直接覆盖；Windows 目标存在时 rename 会失败，所以先删）。调用方保证 daemon
// 已停（由 restartEngine 窗口保证）。返回错误串。
std::string replaceBinary(const std::filesystem::path& src,
                          const std::filesystem::path& target) {
    std::error_code ec;
    const std::filesystem::path staged =
        target.parent_path() / (target.filename().string() + ".new");
    std::filesystem::copy_file(src, staged,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return ec.message();
    // Windows 上进程刚 terminate，文件锁释放可能有毫秒级延迟：短暂重试。
    for (int attempt = 0; attempt < 30; ++attempt) {
        std::filesystem::remove(target, ec);
        if (!ec || !std::filesystem::exists(target, ec)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (std::filesystem::exists(target, ec)) return tr("comp.error.replace");
    std::filesystem::rename(staged, target, ec);
    if (ec) return ec.message();
#ifndef _WIN32
    // 下载落盘的文件没有执行位。
    std::filesystem::permissions(target,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ec);
#endif
    return {};
}

// 更新完成后重探引擎版本（--version），刷新状态的 current。后台线程调用。
std::string probeVersion() {
    const std::string exe = findEngineBinary(kBinary);
    if (exe.empty()) return {};
    const auto p = runCapture(exe, {"--version"},
                              cfg::configDir() / "tinynext-version-probe.log", 30);
    if (p.exitCode != 0) return {};
    return parseVersionAfterMarker(p.out, "version ");
}

} // namespace

namespace updater {

ComponentSnapshot snapshot() {
    std::lock_guard lock(g_mu);
    return ComponentSnapshot{g_state.current, g_state.latest, g_state.status,
                             g_state.progress, g_state.error};
}

void setWakeUi(std::function<void()> fn) {
    std::lock_guard lock(g_mu);
    g_wakeUi = std::move(fn);
}

// 预热线程调用：跑 aria2-next --version 填充当前版本。
void probeAria2Version() {
    mutate([](CompState& s) { s.current = probeVersion(); });
}

void checkLatest(dl::DownloadEngine& eng) {
    {
        std::lock_guard lock(g_mu);
        if (g_state.busy) return;
        g_state.busy = true;
        g_state.status = CompStatus::Checking;
        g_state.error.clear();
        g_state.progress = 0;
    }
    wakeUi();
    // eng 是 g_tasks 持有的长命对象（与进程同寿），引用捕获安全。
    std::thread([&eng] {
        const std::filesystem::path tmp = std::filesystem::temp_directory_path() /
            "tinynext-update" / "aria2-next-latest.json";
        std::error_code ec;
        std::filesystem::create_directories(tmp.parent_path(), ec);
        std::string err;
        if (!downloadSync(eng, apiUrl(), tmp, err)) {
            mutate([&](CompState& s) {
                s.busy = false;
                s.status = CompStatus::CheckFailed;
                s.error = std::move(err);
            });
            return;
        }
        std::string tag;
        try {
            std::ifstream in(tmp, std::ios::binary);
            const std::string body((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            tag = nlohmann::json::parse(body).value("tag_name", "");
        } catch (...) {}
        if (tag.empty()) {
            mutate([](CompState& s) {
                s.busy = false;
                s.status = CompStatus::CheckFailed;
                s.error = tr("comp.error.parse");
            });
            return;
        }
        const std::string ver = stripLeadingV(tag);
        mutate([&](CompState& s) {
            s.busy = false;
            s.latest = ver;
            s.tag = tag;
            // current 为空（二进制缺失/探测失败）也允许更新：能顺便装上。
            s.status = (s.current.empty() || compareVersions(ver, s.current) > 0)
                           ? CompStatus::UpdateAvailable
                           : CompStatus::UpToDate;
        });
    }).detach();
}

void startUpdate(dl::DownloadEngine& eng) {
    std::string ver;
    std::string tag;
    {
        std::lock_guard lock(g_mu);
        if (g_state.busy || g_state.tag.empty()) return;
        g_state.busy = true;
        g_state.status = CompStatus::Downloading;
        g_state.progress = 0;
        g_state.error.clear();
        ver = g_state.latest;
        tag = g_state.tag;
    }
    wakeUi();
    std::thread([&eng, ver, tag] {
        auto failWith = [](std::string e) {
            mutate([&](CompState& s) {
                s.busy = false;
                s.status = CompStatus::Failed;
                s.error = std::move(e);
            });
        };
        const std::string asset = assetNameOf(ver);
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "tinynext-update";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path binFile = dir / asset;
        const std::filesystem::path sumFile = dir / checksumAssetOf(ver);

        // 1) 下载新二进制 + 校验文件。
        std::string err;
        if (!downloadSync(eng, downloadUrlOf(tag, asset), binFile, err)) {
            failWith(std::move(err));
            return;
        }
        if (!downloadSync(eng, downloadUrlOf(tag, checksumAssetOf(ver)),
                          sumFile, err)) {
            failWith(std::move(err));
            return;
        }

        // 2) sha256 校验（不符绝不替换）。
        mutate([](CompState& s) { s.status = CompStatus::Verifying; });
        const std::string expect = extractHashFor(sumFile, asset);
        const std::string actual = sha256FileHex(binFile);
        if (expect.empty() || actual.empty() || expect != actual) {
            failWith(tr("comp.error.checksum"));
            return;
        }

        // 3) 目标二进制与可写性（/usr/bin 等系统目录不可写 → 提示包管理器升级）。
        const std::string targetStr = findEngineBinary(kBinary);
        if (targetStr.empty()) {
            failWith(tr("comp.error.no_binary"));
            return;
        }
        const std::filesystem::path target(targetStr);
        const std::filesystem::path probe =
            target.parent_path() / (asset + ".write-test");
        {
            std::ofstream t(probe, std::ios::binary);
            if (!t) {
                failWith(tr("comp.error.readonly"));
                return;
            }
        }
        std::filesystem::remove(probe, ec);

        // 4) 替换：经 restartEngine 的 beforeRespawn 窗口（daemon 已停、尚未
        //    重拉起）换文件并自动重拉起。
        mutate([](CompState& s) { s.status = CompStatus::Replacing; });
        {
            std::mutex mu;
            std::condition_variable cv;
            bool finished = false;
            bool ok = false;
            eng.restartEngine(
                [&](bool success) {
                    {
                        std::lock_guard lock(mu);
                        finished = true;
                        ok = success;
                    }
                    cv.notify_one();
                },
                [&] { return replaceBinary(binFile, target).empty(); });
            std::unique_lock lock(mu);
            cv.wait_for(lock, std::chrono::seconds(60), [&] { return finished; });
            if (!ok) {
                failWith(tr("comp.error.restart"));
                return;
            }
        }

        // 5) 重探版本（--version），刷新 current；探测失败就用 latest 顶上。
        std::string current = probeVersion();
        if (current.empty()) current = ver;
        mutate([&](CompState& s) {
            s.busy = false;
            s.status = CompStatus::Done;
            s.progress = 100;
            s.current = std::move(current);
        });
    }).detach();
}

} // namespace updater
