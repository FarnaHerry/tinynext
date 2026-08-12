// sha256.cppm — minimal SHA-256 (FIPS 180-4), zero external dependency.
//
// 用途：运行时校验 engines/ 下引擎二进制的完整性（checksums.sha256 只在 CI
// 校验，运行时从不检查——被篡改/放错的二进制会被静默执行）。`import std;` only。
export module tinynext.sha256;

import std;

namespace sha {

namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::uint32_t rotr(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

struct Sha256 {
    std::array<std::uint32_t, 8> h = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> buf{};
    std::uint64_t totalLen = 0;  // 已处理完整 64 字节块的总字节数
    std::size_t bufLen = 0;

    void process(const std::uint8_t* p) {
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(p[i * 4]) << 24) |
                   (std::uint32_t(p[i * 4 + 1]) << 16) |
                   (std::uint32_t(p[i * 4 + 2]) << 8) |
                   (std::uint32_t(p[i * 4 + 3]));
        }
        for (int t = 16; t < 64; ++t) {
            const std::uint32_t s0 =
                rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const std::uint32_t s1 =
                rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int t = 0; t < 64; ++t) {
            const std::uint32_t sigma1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = hh + sigma1 + ch + kK[t] + w[t];
            const std::uint32_t sigma0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sigma0 + maj;
            hh = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a;
            a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(std::span<const std::uint8_t> data) {
        for (const std::uint8_t byte : data) {
            buf[bufLen++] = byte;
            if (bufLen == 64) {
                process(buf.data());
                totalLen += 64;
                bufLen = 0;
            }
        }
    }

    void finalize(std::array<std::uint8_t, 32>& out) {
        // 消息总长（字节）×8 位。totalLen 是完整块，bufLen 是缓冲里未满一块的字节。
        const std::uint64_t bitLen = (totalLen + bufLen) * 8;
        buf[bufLen++] = 0x80;
        if (bufLen > 56) {
            while (bufLen < 64) buf[bufLen++] = 0;
            process(buf.data());
            bufLen = 0;
        }
        while (bufLen < 56) buf[bufLen++] = 0;
        for (int i = 0; i < 8; ++i) {
            buf[56 + i] = static_cast<std::uint8_t>(bitLen >> (56 - i * 8));
        }
        process(buf.data());
        for (int i = 0; i < 8; ++i) {
            out[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
        }
    }
};

std::string toHex(std::span<const std::uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

} // namespace

// 对内存缓冲算 SHA-256，返回小写十六进制（"abc" → ba7816bf…）。
export std::string hexSha256(std::span<const std::uint8_t> data) {
    Sha256 ctx;
    ctx.update(data);
    std::array<std::uint8_t, 32> digest{};
    ctx.finalize(digest);
    return toHex(digest);
}

// 对文件算 SHA-256，返回小写十六进制；读取失败（不存在/不可读）返回空串。
export std::string fileSha256(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    Sha256 ctx;
    std::vector<std::uint8_t> chunk(64 * 1024);
    for (;;) {
        in.read(reinterpret_cast<char*>(chunk.data()),
                static_cast<std::streamsize>(chunk.size()));
        const std::streamsize n = in.gcount();
        if (n > 0) {
            ctx.update(std::span<const std::uint8_t>(chunk.data(),
                                                     static_cast<std::size_t>(n)));
        }
        if (n < static_cast<std::streamsize>(chunk.size())) break;
    }
    std::array<std::uint8_t, 32> digest{};
    ctx.finalize(digest);
    return toHex(digest);
}

} // namespace sha
