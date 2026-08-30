// component_updater.cppm — 组件（外部二进制依赖）的应用内更新：aria2-next /
// yt-dlp。ffmpeg 需要编译、随应用版本走，不在此更新（设置页只显示版本）。
//
// 领域层模块：不 import 任何 ui.*/eui。所有网络/文件操作都在 updater 自己的
// 工作线程跑（引擎 downloadFile 回调在引擎线程，经条件变量转回工作线程）；
// UI 线程只经 snapshot() 读纯值拷贝。状态变化经注入的 wake 回调唤醒 UI
// （app.cpp 启动时注入 core::platform::requestUiUpdate）。
//
// 更新源（GitHub releases，两边都随 release 发布 sha256 校验文件）：
//   aria2-next: AnInsomniacy/aria2-next —— 资产 aria2-next-<ver>-<os>-<arch>[.exe]，
//               校验文件 aria2-next-<ver>-checksums.sha256；
//   yt-dlp:     yt-dlp/yt-dlp —— 资产 yt-dlp.exe / yt-dlp_linux / yt-dlp_macos，
//               校验文件 SHA2-256SUMS。
// 项目无 TLS/HTTP 客户端依赖：HTTPS 下载全部交给运行中的 aria2 daemon
// （dl::DownloadEngine::downloadFile 静默通道，不出下载卡片）。
//
// 更新 aria2-next 自身的顺序：下载 + 校验先做完，替换二进制经
// restartEngine(beforeRespawn) 在「daemon 已停、尚未重拉起」的窗口里执行
// （Windows 运行中的 exe 被文件锁占用）；yt-dlp 非常驻进程，直接原子替换。
module;

#include <nlohmann/json.hpp>

export module tinynext.component_updater;

import std;
import tinynext.config;          // configDir（版本探测的 stderr 日志）
import tinynext.download_engine; // dl::DownloadEngine（downloadFile/restartEngine）
import tinynext.i18n;            // tr（错误文案）
import tinynext.video_resolver;  // findEngineBinary / runCapture / ytDlpVersion

export namespace updater {

enum class Component { Aria2, YtDlp };

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

ComponentSnapshot snapshot(Component c);
void setWakeUi(std::function<void()> fn);   // app.cpp 注入 requestUiUpdate
void setCurrentVersion(Component c, std::string version);  // 预热探测写入
void probeAria2Version();   // 跑 aria2-next --version（后台线程调用，可能数秒）
void checkLatest(dl::DownloadEngine& eng, Component c);
void startUpdate(dl::DownloadEngine& eng, Component c);

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

// ---- 组件元数据 ----

using updater::Component;

int compIndex(Component c) { return c == Component::Aria2 ? 0 : 1; }

const char* repoOf(Component c) {
    return c == Component::Aria2 ? "AnInsomniacy/aria2-next" : "yt-dlp/yt-dlp";
}

const char* binaryOf(Component c) {
    return c == Component::Aria2 ? "aria2-next" : "yt-dlp";
}

std::string stripLeadingV(std::string tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    return tag;
}

// 发布资产名按编译期平台映射（我们只发 win64 / linux-x86_64 / macos-arm64）。
std::string assetNameOf(Component c, const std::string& ver) {
    if (c == Component::Aria2) {
#ifdef _WIN32
        return "aria2-next-" + ver + "-windows-x86_64.exe";
#elif defined(__APPLE__)
        return "aria2-next-" + ver + "-macos-arm64";
#else
        return "aria2-next-" + ver + "-linux-x86_64";
#endif
    }
#ifdef _WIN32
    return "yt-dlp.exe";
#elif defined(__APPLE__)
    return "yt-dlp_macos";
#else
    return "yt-dlp_linux";
#endif
}

std::string checksumAssetOf(Component c, const std::string& ver) {
    return c == Component::Aria2 ? "aria2-next-" + ver + "-checksums.sha256"
                                 : "SHA2-256SUMS";
}

std::string apiUrlOf(Component c) {
    return std::string("https://api.github.com/repos/") + repoOf(c) + "/releases/latest";
}

std::string downloadUrlOf(Component c, const std::string& tag, const std::string& asset) {
    return std::string("https://github.com/") + repoOf(c) + "/releases/download/" +
           tag + "/" + asset;
}

// dotted numeric 版本比较（前导 v 已去）：2.6.7 < 2.6.8；yt-dlp 的日期版
// （2026.08.19）同样适用，前导 0 段按数值处理（"08"=8）。返回 -1/0/1。
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

// "Aria2 Next version 2.6.2\n..." → "2.6.2"（yt-dlp --version 直接出版本号，
// 不需要过这个解析）。
std::string parseVersionAfterMarker(const std::string& out, std::string_view marker) {
    const auto pos = out.find(marker);
    if (pos == std::string::npos) return {};
    const std::size_t start = pos + marker.size();
    const auto end = out.find_first_of(" \r\n\t", start);
    return out.substr(start, end == std::string::npos ? std::string::npos : end - start);
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
CompState g_states[2];
std::function<void()> g_wakeUi;

void wakeUi() {
    if (g_wakeUi) g_wakeUi();
}

// 锁内改状态、锁外唤醒 UI。
template <typename F>
void mutate(Component c, F&& fn) {
    {
        std::lock_guard lock(g_mu);
        fn(g_states[compIndex(c)]);
    }
    wakeUi();
}

// 在工作线程上同步等待引擎静默下载完成（downloadFile 的回调在引擎线程触发，
// 经条件变量转回来）。onProgress 写进状态供 UI 显示百分比。
bool downloadSync(dl::DownloadEngine& eng, const std::string& url,
                  const std::filesystem::path& dest, Component c, std::string& errOut) {
    std::mutex mu;
    std::condition_variable cv;
    bool finished = false;
    bool ok = false;
    std::string err;
    eng.downloadFile(url, dest,
                     [c](int p) {
                         mutate(c, [p](CompState& s) { s.progress = p; });
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
// 可直接覆盖；Windows 目标存在时 rename 会失败，所以先删）。调用方保证目标
// 进程已停（aria2 daemon 由 restartEngine 窗口保证；yt-dlp 非常驻）。返回错误串。
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

// 更新完成后重探组件版本（--version），刷新状态的 current。后台线程调用。
std::string probeVersion(Component c) {
    const std::string exe = video::findEngineBinary(binaryOf(c));
    if (exe.empty()) return {};
    const auto p = video::runCapture(exe, {"--version"},
                                     cfg::configDir() / "tinynext-version-probe.log", 30);
    if (p.exitCode != 0) return {};
    if (c == Component::Aria2) return parseVersionAfterMarker(p.out, "version ");
    std::string v = p.out;
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
    return v;
}

} // namespace

namespace updater {

ComponentSnapshot snapshot(Component c) {
    std::lock_guard lock(g_mu);
    const auto& s = g_states[compIndex(c)];
    return ComponentSnapshot{s.current, s.latest, s.status, s.progress, s.error};
}

void setWakeUi(std::function<void()> fn) {
    std::lock_guard lock(g_mu);
    g_wakeUi = std::move(fn);
}

void setCurrentVersion(Component c, std::string version) {
    mutate(c, [&](CompState& s) { s.current = std::move(version); });
}

// 预热线程调用：跑 aria2-next --version 填充当前版本（yt-dlp 的 current 由
// video::probeVideoToolVersions 探好后经 setCurrentVersion 写入）。
void probeAria2Version() {
    setCurrentVersion(Component::Aria2, probeVersion(Component::Aria2));
}

void checkLatest(dl::DownloadEngine& eng, Component c) {
    {
        std::lock_guard lock(g_mu);
        auto& s = g_states[compIndex(c)];
        if (s.busy) return;
        s.busy = true;
        s.status = CompStatus::Checking;
        s.error.clear();
        s.progress = 0;
    }
    wakeUi();
    // eng 是 g_tasks 持有的长命对象（与进程同寿），引用捕获安全。
    std::thread([&eng, c] {
        const std::filesystem::path tmp = std::filesystem::temp_directory_path() /
            "tinynext-update" / (std::string(binaryOf(c)) + "-latest.json");
        std::error_code ec;
        std::filesystem::create_directories(tmp.parent_path(), ec);
        std::string err;
        if (!downloadSync(eng, apiUrlOf(c), tmp, c, err)) {
            mutate(c, [&](CompState& s) {
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
            mutate(c, [](CompState& s) {
                s.busy = false;
                s.status = CompStatus::CheckFailed;
                s.error = tr("comp.error.parse");
            });
            return;
        }
        const std::string ver = stripLeadingV(tag);
        mutate(c, [&](CompState& s) {
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

void startUpdate(dl::DownloadEngine& eng, Component c) {
    std::string ver;
    std::string tag;
    {
        std::lock_guard lock(g_mu);
        auto& s = g_states[compIndex(c)];
        if (s.busy || s.tag.empty()) return;
        s.busy = true;
        s.status = CompStatus::Downloading;
        s.progress = 0;
        s.error.clear();
        ver = s.latest;
        tag = s.tag;
    }
    wakeUi();
    std::thread([&eng, c, ver, tag] {
        auto failWith = [c](std::string e) {
            mutate(c, [&](CompState& s) {
                s.busy = false;
                s.status = CompStatus::Failed;
                s.error = std::move(e);
            });
        };
        const std::string asset = assetNameOf(c, ver);
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "tinynext-update";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path binFile = dir / asset;
        const std::filesystem::path sumFile = dir / checksumAssetOf(c, ver);

        // 1) 下载新二进制 + 校验文件。
        std::string err;
        if (!downloadSync(eng, downloadUrlOf(c, tag, asset), binFile, c, err)) {
            failWith(std::move(err));
            return;
        }
        if (!downloadSync(eng, downloadUrlOf(c, tag, checksumAssetOf(c, ver)),
                          sumFile, c, err)) {
            failWith(std::move(err));
            return;
        }

        // 2) sha256 校验（不符绝不替换）。
        mutate(c, [](CompState& s) { s.status = CompStatus::Verifying; });
        const std::string expect = extractHashFor(sumFile, asset);
        const std::string actual = sha256FileHex(binFile);
        if (expect.empty() || actual.empty() || expect != actual) {
            failWith(tr("comp.error.checksum"));
            return;
        }

        // 3) 目标二进制与可写性（/usr/bin 等系统目录不可写 → 提示包管理器升级）。
        const std::string targetStr = video::findEngineBinary(binaryOf(c));
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

        // 4) 替换：aria2-next 经 restartEngine 的 beforeRespawn 窗口（daemon 已停）
        //    换文件并自动重拉起；yt-dlp 非常驻进程，直接换。
        mutate(c, [](CompState& s) { s.status = CompStatus::Replacing; });
        if (c == Component::Aria2) {
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
        } else {
            if (const std::string e = replaceBinary(binFile, target); !e.empty()) {
                failWith(e);
                return;
            }
        }

        // 5) 重探版本（--version），刷新 current；探测失败就用 latest 顶上。
        std::string current = probeVersion(c);
        if (current.empty()) current = ver;
        mutate(c, [&](CompState& s) {
            s.busy = false;
            s.status = CompStatus::Done;
            s.progress = 100;
            s.current = std::move(current);
        });
    }).detach();
}

} // namespace updater
