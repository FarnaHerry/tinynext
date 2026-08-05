// aria2_engine.cpp — implementation unit for tinynext.aria2_engine.
//
// Spawns the bundled engines/aria2-next.exe (resolved next to the app exe,
// falling back to the working directory) with --enable-rpc, then drives it via
// plain-HTTP JSON-RPC on 127.0.0.1:<random-port>. tinyhttps's send() only
// speaks HTTPS, so the tiny RPC POST is written directly on
// mcpplibs::tinyhttps::Socket. No worker threads: every operation is a
// synchronous RPC on the UI thread.

module;

#ifdef _WIN32
// windows.h is used for CreateProcess/TerminateProcess/HANDLE. LEAN_AND_MEAN
// keeps it from pulling winsock.h (tinyhttps uses winsock2 internally).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
// POSIX (macOS + Linux). Each header must be included explicitly: glibc pulls
// them in transitively so Linux "works by accident", but the macOS SDK does
// not — pid_t/waitpid/kill/posix_spawn all need their own header.
#include <sys/types.h>   // pid_t
#include <sys/wait.h>    // waitpid, WNOHANG
#include <signal.h>      // kill, SIGTERM, SIGKILL
#include <spawn.h>       // posix_spawn
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h> // _NSGetExecutablePath
#endif
// macOS's <unistd.h> does not declare `environ` (glibc does); the symbol still
// exists in the system libc, so declare it here for the posix_spawn call below.
extern char** environ;
#endif

module tinynext.aria2_engine;

import std;
import mcpplibs.tinyhttps;
import nlohmann.json;
import tinynext.config;

namespace dl {

namespace {

// ---- minimal HTTP/JSON-RPC client for the local daemon ----

bool writeAll(mcpplibs::tinyhttps::Socket& sock, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        const int n = sock.write(data + sent, len - sent);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

// Read one HTTP response (Connection: close) from the socket; returns the body.
std::string readHttpResponse(mcpplibs::tinyhttps::Socket& sock) {
    std::string header;
    header.reserve(512);
    while (header.find("\r\n\r\n") == std::string::npos) {
        if (!sock.wait_readable(5000)) return {};
        char c;
        const int n = sock.read(&c, 1);
        if (n <= 0) return {};
        header.push_back(c);
        if (header.size() > 65536) return {};
    }

    // Parse Content-Length (case-insensitive).
    std::int64_t contentLength = -1;
    std::size_t pos = 0;
    while ((pos = header.find("\r\n", pos)) != std::string::npos) {
        pos += 2;
        const std::size_t end = header.find("\r\n", pos);
        if (end == std::string::npos) break;
        const std::string line = header.substr(pos, end - pos);
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        for (auto& ch : key) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
        }
        if (key == "content-length") {
            contentLength = 0;
            for (char ch : line.substr(colon + 1)) {
                if (ch >= '0' && ch <= '9') {
                    contentLength = contentLength * 10 + (ch - '0');
                }
            }
            break;
        }
    }

    const std::size_t bodyStart = header.find("\r\n\r\n") + 4;
    std::string body = header.substr(bodyStart);
    while (contentLength < 0 || static_cast<std::int64_t>(body.size()) < contentLength) {
        if (!sock.wait_readable(5000)) break;
        char buf[8192];
        const int n = sock.read(buf, sizeof(buf));
        if (n <= 0) break;
        body.append(buf, static_cast<std::size_t>(n));
    }
    if (contentLength >= 0 && static_cast<std::int64_t>(body.size()) > contentLength) {
        body.resize(static_cast<std::size_t>(contentLength));
    }
    return body;
}

std::string httpPost(int port, const std::string& body) {
    mcpplibs::tinyhttps::Socket sock;
    if (!sock.connect("127.0.0.1", port, 3000)) return {};
    const std::string request =
        "POST /jsonrpc HTTP/1.1\r\n"
        "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    if (!writeAll(sock, request.data(), static_cast<int>(request.size()))) return {};
    return readHttpResponse(sock);
}

// JSON-RPC call; throws std::runtime_error on transport/JSON/error responses.
nlohmann::json rpcCall(int port, const std::string& secret,
                       const std::string& method, const nlohmann::json& params) {
    nlohmann::json request = nlohmann::json::object();
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = method;
    nlohmann::json fullParams = nlohmann::json::array();
    fullParams.push_back("token:" + secret);
    if (params.is_array()) {
        for (const auto& p : params) fullParams.push_back(p);
    } else {
        fullParams.push_back(params);
    }
    request["params"] = fullParams;

    const std::string raw = httpPost(port, request.dump());
    if (raw.empty()) throw std::runtime_error("rpc: no response from daemon");
    nlohmann::json response = nlohmann::json::parse(raw);
    if (response.contains("error")) {
        throw std::runtime_error("rpc error: " + response["error"].dump());
    }
    return response["result"];
}

std::string randomHex(std::size_t n) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) s.push_back(kHex[dist(rng)]);
    return s;
}

int pickFreePort() {
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(20000, 50000);
    for (int attempt = 0; attempt < 16; ++attempt) {
        const int candidate = dist(rng);
        mcpplibs::tinyhttps::Socket probe;
        if (!probe.connect("127.0.0.1", candidate, 300)) {
            return candidate;  // nothing listening → free
        }
    }
    return 16800;
}

std::string engineExePath() {
    std::filesystem::path exeDir;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        exeDir = std::filesystem::path(buf).parent_path();
    }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        exeDir = std::filesystem::path(buf).parent_path();
    }
#else  // Linux
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) exeDir = self.parent_path();
#endif
    // 二进制名：Windows 带 .exe，unix 不带。
    const char* name = "aria2-next.exe";
#ifndef _WIN32
    name = "aria2-next";
#endif
    for (const std::filesystem::path& base : {exeDir, std::filesystem::current_path()}) {
        if (base.empty()) continue;
        const std::filesystem::path candidate = base / "engines" / name;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate.string();
    }
    return {};
}

// 跨平台进程助手。processHandle_ 在 Windows 上是 HANDLE，POSIX 上是 pid_t
// （经 intptr_t 存放）。
bool processExited(void* handle) {
    if (!handle) return true;
#ifdef _WIN32
    return WaitForSingleObject(static_cast<HANDLE>(handle), 0) == WAIT_OBJECT_0;
#else
    int status = 0;
    return ::waitpid(static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle)),
                     &status, WNOHANG) > 0;
#endif
}

void terminateProcess(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    TerminateProcess(static_cast<HANDLE>(handle), 0);
    CloseHandle(static_cast<HANDLE>(handle));
#else
    const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle));
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {  // ~1s 宽限
        if (::waitpid(pid, nullptr, WNOHANG) > 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
#endif
}

} // namespace

struct Aria2Engine::Task {
    std::uint64_t id;
    std::string gid;
    std::string url;
    std::filesystem::path destPath;
    State state = State::Queued;
    std::int64_t totalBytes = -1;
    std::int64_t downloadedBytes = 0;
    std::string error;
    double speedBps = 0.0;
    int connections = 1;          // numConnections from tellStatus
};

Aria2Engine::Aria2Engine() {
    // Winsock for the local RPC socket (tinyhttps never calls WSAStartup itself).
    mcpplibs::tinyhttps::Socket::platform_init();
}

Aria2Engine::~Aria2Engine() {
    shutdown();
    mcpplibs::tinyhttps::Socket::platform_cleanup();
}

bool Aria2Engine::ensureDaemon() const {
    if (daemonSpawned_) return port_ != 0;

    const std::string exe = engineExePath();
    if (exe.empty()) return false;

    const int port = pickFreePort();
    const std::string secret = randomHex(16);

#ifdef _WIN32
    const cfg::Aria2Config a2 = cfg::aria2Config();
    const std::wstring wExe = std::filesystem::path(exe).wstring();
    std::wstring cmdLine =
        L"\"" + wExe + L"\" --enable-rpc --rpc-listen-all=false "
        L"--rpc-listen-port=" + std::to_wstring(port) + L" "
        L"--rpc-secret=" + std::wstring(secret.begin(), secret.end()) + L" "
        L"--max-connection-per-server=" + std::to_wstring(a2.maxConnectionPerServer) + L" "
        L"--split=" + std::to_wstring(a2.split) + L" "
        L"--min-split-size=" + std::wstring(a2.minSplitSize.begin(), a2.minSplitSize.end()) + L" "
        L"--console-log-level=warn --allow-overwrite=false "
        L"--auto-file-renaming=false";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(wExe.c_str(), &cmdLine[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    processHandle_ = pi.hProcess;
#else
    const cfg::Aria2Config a2 = cfg::aria2Config();
    std::vector<std::string> args = {
        exe, "--enable-rpc", "--rpc-listen-all=false",
        "--rpc-listen-port=" + std::to_string(port),
        "--rpc-secret=" + secret,
        "--max-connection-per-server=" + std::to_string(a2.maxConnectionPerServer),
        "--split=" + std::to_string(a2.split),
        "--min-split-size=" + a2.minSplitSize,
        "--console-log-level=warn", "--allow-overwrite=false",
        "--auto-file-renaming=false"};
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    if (::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
        return false;
    }
    processHandle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
#endif

    // Wait until the RPC endpoint answers (up to ~3 s). If the spawned process
    // exits immediately (e.g. invalid args), fail fast instead of hanging the
    // UI thread on the full deadline.
    port_ = port;
    secret_ = secret;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (processHandle_ && processExited(processHandle_)) {
            break;  // daemon died right after spawn (e.g. invalid args)
        }
        try {
            rpcCall(port_, secret_, "aria2.getVersion", nlohmann::json::array());
            daemonSpawned_ = true;
            return true;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    if (processHandle_) {
        terminateProcess(processHandle_);
        processHandle_ = nullptr;
    }
    port_ = 0;
    secret_.clear();
    return false;
}

std::uint64_t Aria2Engine::start(const std::string& url, const std::filesystem::path& destPath,
                                 const StartOptions& options) {
    if (!ensureDaemon()) return 0;

    auto task = std::make_shared<Task>();
    task->id = nextId_++;
    task->url = url;
    task->destPath = makeUniqueDest(destPath);

    const cfg::Aria2Config a2 = cfg::aria2Config();
    // 每任务连接数覆盖：>0 时同时覆盖 split 和 max-connection-per-server。
    const int connections = options.connections > 0
        ? std::clamp(options.connections, 1, 64)
        : a2.split;
    nlohmann::json optionsJ = nlohmann::json::object();
    optionsJ["dir"] = task->destPath.parent_path().string();
    optionsJ["out"] = task->destPath.filename().string();
    optionsJ["split"] = std::to_string(connections);
    optionsJ["max-connection-per-server"] = std::to_string(connections);
    optionsJ["min-split-size"] = a2.minSplitSize;
    optionsJ["continue"] = "true";
    if (a2.maxDownloadLimit > 0) {
        optionsJ["max-download-limit"] = std::to_string(a2.maxDownloadLimit);
    }

    // The `options` name below conflicts with the StartOptions parameter, so
    // the RPC params use the local `optionsJ` object.
    nlohmann::json params = nlohmann::json::array();
    params.push_back(nlohmann::json::array({url}));
    params.push_back(optionsJ);

    try {
        const nlohmann::json result = rpcCall(port_, secret_, "aria2.addUri", params);
        task->gid = result.get<std::string>();
    } catch (const std::exception& e) {
        task->state = State::Failed;
        task->error = e.what();
        tasks_.push_back(task);
        return task->id;
    }

    tasks_.push_back(task);
    return task->id;
}

std::shared_ptr<Aria2Engine::Task> Aria2Engine::findTask(std::uint64_t id) const {
    for (const auto& task : tasks_) {
        if (task->id == id) return task;
    }
    return nullptr;
}

void Aria2Engine::cancel(std::uint64_t id) {
    auto task = findTask(id);
    if (!task || !daemonSpawned_) return;
    try {
        rpcCall(port_, secret_, "aria2.remove", nlohmann::json::array({task->gid}));
    } catch (...) {
        try {
            rpcCall(port_, secret_, "aria2.forceRemove", nlohmann::json::array({task->gid}));
        } catch (...) {}
    }
    task->state = State::Cancelled;
    task->speedBps = 0.0;
}

void Aria2Engine::remove(std::uint64_t id) {
    auto task = findTask(id);
    if (!task) return;
    if (daemonSpawned_) {
        try {
            rpcCall(port_, secret_, "aria2.remove", nlohmann::json::array({task->gid}));
        } catch (...) {}
    }
    std::erase_if(tasks_, [&](const std::shared_ptr<Task>& t) { return t->id == id; });
}

void Aria2Engine::pause(std::uint64_t id) {
    auto task = findTask(id);
    if (!task || !daemonSpawned_) return;
    if (task->state != State::Queued && task->state != State::Downloading) return;
    try {
        rpcCall(port_, secret_, "aria2.pause", nlohmann::json::array({task->gid}));
        task->state = State::Paused;
        task->speedBps = 0.0;
    } catch (...) {}
}

void Aria2Engine::resume(std::uint64_t id) {
    auto task = findTask(id);
    if (!task || !daemonSpawned_) return;
    if (task->state != State::Paused) return;
    try {
        rpcCall(port_, secret_, "aria2.unpause", nlohmann::json::array({task->gid}));
        task->state = State::Downloading;
    } catch (...) {}
}

void Aria2Engine::pauseAll() {
    if (!daemonSpawned_) return;
    // 全部挂起：aria2.pauseAll 会把所有活动/等待任务置为 paused；本地状态同步
    // 更新。已有 paused 的调用 pauseAll 后仍为 paused，本地判断不变。
    try {
        rpcCall(port_, secret_, "aria2.pauseAll", nlohmann::json::array());
        for (const auto& task : tasks_) {
            if (task->state == State::Queued || task->state == State::Downloading) {
                task->state = State::Paused;
                task->speedBps = 0.0;
            }
        }
    } catch (...) {}
}

void Aria2Engine::resumeAll() {
    if (!daemonSpawned_) return;
    try {
        rpcCall(port_, secret_, "aria2.unpauseAll", nlohmann::json::array());
        for (const auto& task : tasks_) {
            if (task->state == State::Paused) {
                task->state = State::Downloading;
            }
        }
    } catch (...) {}
}

void Aria2Engine::refreshStates() const {
    if (!daemonSpawned_) return;
    for (const auto& task : tasks_) {
        const State s = task->state;
        if (s != State::Queued && s != State::Downloading && s != State::Paused) continue;
        try {
            const nlohmann::json st =
                rpcCall(port_, secret_, "aria2.tellStatus", nlohmann::json::array({task->gid}));
            const std::string status = st.value("status", "active");

            auto strI64 = [&](const char* key) -> std::int64_t {
                const std::string v = st.value(key, "0");
                try { return std::stoll(v); } catch (...) { return 0; }
            };
            task->totalBytes = strI64("totalLength");
            task->downloadedBytes = strI64("completedLength");
            try { task->speedBps = std::stod(st.value("downloadSpeed", "0")); }
            catch (...) { task->speedBps = 0.0; }
            // aria2-next reports the field as "connections"; original aria2 uses
            // "numConnections". Accept both so the engine works with either.
            const std::string connField =
                st.value("connections", st.value("numConnections", "1"));
            try { task->connections = std::max(1, std::stoi(connField)); }
            catch (...) { task->connections = 1; }

            if (status == "complete") {
                task->state = State::Done;
                task->speedBps = 0.0;
            } else if (status == "error") {
                task->state = State::Failed;
                task->error = st.value("errorMessage", "download error");
                task->speedBps = 0.0;
            } else if (status == "removed") {
                task->state = State::Cancelled;
                task->speedBps = 0.0;
            } else if (status == "waiting") {
                task->state = State::Queued;
            } else if (status == "paused") {
                task->state = State::Paused;
                task->speedBps = 0.0;
            } else {  // "active"
                task->state = State::Downloading;
            }
        } catch (...) {
            // Transient RPC failure — leave state as-is, retry next poll.
        }
    }
}

std::vector<TaskView> Aria2Engine::snapshot() const {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPoll_ >= std::chrono::milliseconds(200)) {
        lastPoll_ = now;
        refreshStates();
    }
    std::vector<TaskView> out;
    out.reserve(tasks_.size());
    for (auto it = tasks_.rbegin(); it != tasks_.rend(); ++it) {
        const Task& task = **it;
        out.push_back(TaskView{task.id, task.url, task.destPath, task.state,
                               task.totalBytes, task.downloadedBytes,
                               task.error, task.speedBps, task.connections});
    }
    return out;
}

bool Aria2Engine::busy() const {
    for (const auto& task : tasks_) {
        const State s = task->state;
        if (s == State::Queued || s == State::Downloading || s == State::Paused) {
            return true;
        }
    }
    return false;
}

void Aria2Engine::shutdown() {
    if (daemonSpawned_) {
        try {
            rpcCall(port_, secret_, "aria2.forceShutdown", nlohmann::json::array());
        } catch (...) {}
        if (processHandle_) {
            terminateProcess(processHandle_);
            processHandle_ = nullptr;
        }
        daemonSpawned_ = false;
        port_ = 0;
        secret_.clear();
    }
    tasks_.clear();
}

std::filesystem::path Aria2Engine::makeUniqueDest(const std::filesystem::path& dest) const {
    std::vector<std::filesystem::path> reserved;
    reserved.reserve(tasks_.size());
    for (const auto& task : tasks_) {
        reserved.push_back(task->destPath);
    }

    auto taken = [&](const std::filesystem::path& candidate) {
        return std::filesystem::exists(candidate) ||
               std::ranges::count(reserved, candidate) > 0;
    };
    if (!taken(dest)) return dest;

    const std::filesystem::path parent = dest.parent_path();
    const std::string stem = dest.stem().string();
    const std::string ext = dest.extension().string();
    for (int i = 1;; ++i) {
        const auto candidate = parent / (stem + " (" + std::to_string(i) + ")" + ext);
        if (!taken(candidate)) return candidate;
    }
}

} // namespace dl
