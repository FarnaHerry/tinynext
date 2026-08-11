// ui/utils.cppm — pure helpers + the shared UI scale factor (kUI).
// No EUI dependency: std only.
export module tinynext.ui.utils;

import std;

// ---- UI 缩放 ----
// eui-neo 0.5.6 起 DslAppConfig::uiScale(kUI) 原生放大整个逻辑坐标系（布局+字号），
// 本项目不再自乘系数。所有尺寸按“设计逻辑像素”直接书写，由 eui 按
// dpiScale*uiScale 统一放大；kUI 仍是唯一的缩放旋钮（传给 uiScale + 决定窗口物理尺寸）。
export constexpr float kUI = 1.4f;

// 布局尺寸都按“设计逻辑像素”书写，并尽量用 screen.width/height 推算，
// 随窗口缩放自适应。
// 当前仅卡片的右边缘与窗口边之间留 kRightMargin（图标栏占满左缘、卡片顶/底贴齐）。
export constexpr float kRightMargin = 6.0f;
export constexpr float kInputHeight = 26.0f;
export constexpr float kPagerHeight = 24.0f;        // 翻页行高
export constexpr float kCardHeight = 68.0f;         // 卡片高
export constexpr float kCardPad = 10.0f;
export constexpr float kCardGap = 6.0f;
export constexpr float kCardIconW = 22.0f;          // 卡片操作图标按钮边长
export constexpr float kCardIconGap = 4.0f;
export constexpr float kRailWidth = 40.0f;          // 大侧边栏（图标栏）宽
export constexpr float kSubSidebarWidth = 96.0f;    // 下载页内任务列表子侧边栏宽
// 岛屿卡片布局：外层"岛"卡片之间的间距 / 大卡内边距 / 岛卡片圆角。
export constexpr float kIslandGap = 2.0f;
export constexpr float kIslandVInset = 6.0f;   // 岛卡距窗口上/下的空隙（卡片感）
export constexpr float kPanelPad = 10.0f;
export constexpr float kIslandRadius = 10.0f;

// ---- string / number helpers ----

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

// 可下载源的 URL 前缀白名单：http(s) / ftp(s) / sftp 直链 + magnet 磁力。
// CLI / startDownloadFromUrl / 添加弹窗剪贴板预填 共用（一处维护，避免三处漂移）。
// 本地 .torrent 文件路径不在这里（它不是 URL），由各调用方按扩展名单独放行。
export bool isDownloadableSource(const std::string& s) {
    return s.starts_with("http://") || s.starts_with("https://") ||
           s.starts_with("ftp://") || s.starts_with("ftps://") ||
           s.starts_with("sftp://") || s.starts_with("magnet:");
}

export std::string fileNameFromUrl(const std::string& url) {
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

export std::string trimText(std::string s) {
    const std::size_t f = s.find_first_not_of(" \t\r\n");
    const std::size_t l = s.find_last_not_of(" \t\r\n");
    return f == std::string::npos ? "" : s.substr(f, l - f + 1);
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
