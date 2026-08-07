// ui/utils.cppm — pure helpers + the shared UI scale factor (kUI / S()).
// No EUI dependency: std only.
export module tinynext.ui.utils;

import std;

// ---- UI 缩放 ----
// eui 没有全局缩放开关（components::button 自带 .scale() 但只作用于组件按钮，
// 覆盖不了大量自绘控件），所以这里用一个统一系数放大所有尺寸/字号/间距。
// 想整体改大改小，只调 kUI 这一个数。
export constexpr float kUI = 1.4f;
export constexpr float S(float v) { return v * kUI; }

// 布局尺寸都按“逻辑像素”（= 窗口屏幕像素）设计，经 S() 放大 kUI 倍，
// 并尽量用 screen.width/height 推算，随窗口缩放自适应。
// 当前仅卡片的右边缘与窗口边之间留 kRightMargin（图标栏占满左缘、卡片顶/底贴齐）。
export constexpr float kRightMargin = S(6.0f);
export constexpr float kInputHeight = S(26.0f);
export constexpr float kPagerHeight = S(24.0f);        // 翻页行高
export constexpr float kCardHeight = S(68.0f);         // 卡片高
export constexpr float kCardPad = S(10.0f);
export constexpr float kCardGap = S(6.0f);
export constexpr float kCardIconW = S(22.0f);          // 卡片操作图标按钮边长
export constexpr float kCardIconGap = S(4.0f);
export constexpr float kRailWidth = S(40.0f);          // 大侧边栏（图标栏）宽
export constexpr float kSubSidebarWidth = S(96.0f);    // 下载页内任务列表子侧边栏宽
// 岛屿卡片布局：外层"岛"卡片之间的间距 / 大卡内边距 / 岛卡片圆角。
export constexpr float kIslandGap = S(2.0f);
export constexpr float kIslandVInset = S(6.0f);   // 岛卡距窗口上/下的空隙（卡片感）
export constexpr float kPanelPad = S(10.0f);
export constexpr float kIslandRadius = S(10.0f);

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

// 把 "1M"/"512K"/"2G" 拆成数值与显示单位（KB/MB/GB）；无/未知后缀按 MB 兜底。
export void splitSizeUnit(const std::string& size, std::string& value, std::string& unit) {
    if (size.size() >= 2) {
        switch (size.back()) {
            case 'K': case 'k': value = size.substr(0, size.size() - 1); unit = "KB"; return;
            case 'M': case 'm': value = size.substr(0, size.size() - 1); unit = "MB"; return;
            case 'G': case 'g': value = size.substr(0, size.size() - 1); unit = "GB"; return;
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
