// ui/state.cppm — shared mutable UI state + the download engine + the
// add-download flow. All UI modules import this and read/write the same
// globals directly (module-scope exported variables have a unique entity
// across importers, so `import tinynext.ui.state;` shares one g_page, etc.).
//
// Boot-order note: this module's dynamic initializers (g_manager = createEngine(),
// the *_Text globals reading config) run before any importing TU's static
// initializers. That is fine for the primary instance; a secondary instance
// briefly creates an engine object before the CLI boot exits it — harmless.
export module tinynext.ui.state;

import std;
import tinynext.config;
import tinynext.aria2_engine;
import tinynext.download_engine;
import tinynext.ui.utils;
import tinynext.ui.platform;

// ---- download engine ----
// 纯 aria2-next 引擎（TinyHttpsEngine 已移除）：UI 只面向抽象接口 dl::DownloadEngine。
export std::unique_ptr<dl::DownloadEngine> createEngine() {
    return std::make_unique<dl::Aria2Engine>();
}

export std::unique_ptr<dl::DownloadEngine> g_manager = createEngine();

// ---- page / status / dialogs ----

export enum class Page { Downloads, Settings };
export Page g_page_view = Page::Downloads;  // 默认打开下载列表

export std::string g_urlText;
export std::string g_statusMessage;
export float g_statusTimer = 0.0f;
export bool g_addOpen = false;   // “添加下载”弹窗是否打开
export bool g_aboutOpen = false; // “关于”弹窗是否打开
// 设置页“下载路径”输入框内容；初始化为有效下载目录（持久化优先）。
export std::string g_downloadDirText = cfg::downloadDir().string();

export void showStatus(std::string message) {
    g_statusMessage = std::move(message);
    g_statusTimer = 4.0f;
}

// ---- theme mode / settings pending ----
// 主题三态：跟随系统 / 深色 / 浅色，持久化在 tinynext.conf 的 theme_mode。
// g_dark 是当前生效的深色布尔（System 模式时由 cfg::osDark() 实时跟随）。
export cfg::ThemeMode g_themeMode = cfg::themeMode();
export bool g_dark = cfg::effectiveDark();
export float g_systemThemeTimer = 0.0f;  // System 模式下的 OS 主题轮询计时
// 设置页待提交的编辑值：主题/路径只在点「保存」时写入配置并生效，
// 点「放弃」回滚到已保存值。主题在选择时即时预览（g_dark），但不落盘。
export cfg::ThemeMode g_pendingTheme = g_themeMode;
// aria2 参数待提交值（设置页输入框的文本形式）。
export std::string g_aria2SplitText = std::to_string(cfg::aria2Config().split);
export std::string g_aria2ConnText = std::to_string(cfg::aria2Config().maxConnectionPerServer);
export std::string g_aria2MinSplitText = cfg::aria2Config().minSplitSize;
export std::string g_aria2LimitText =
    std::to_string(cfg::aria2Config().maxDownloadLimit / 1024);  // KB/s
// daemon 级参数待提交值。
export std::string g_proxyText = cfg::aria2Config().proxy;
export std::string g_noProxyText = cfg::aria2Config().noProxy;
export std::string g_maxTriesText = std::to_string(cfg::aria2Config().maxTries);
export std::string g_retryWaitText = std::to_string(cfg::aria2Config().retryWait);
export std::string g_maxConcurrentText =
    std::to_string(cfg::aria2Config().maxConcurrentDownloads);
export bool g_removeControlFile = cfg::aria2Config().removeControlFile;
export std::string g_onCompleteText = cfg::aria2Config().onDownloadComplete;
export std::string g_userAgentText = cfg::aria2Config().userAgent;
export std::string g_refererText = cfg::aria2Config().referer;
export std::string g_diskCacheText = cfg::aria2Config().diskCache;
// 添加下载弹窗的每任务连接数（默认 = 配置 split 值；空/0 = 配置默认）。
export std::string g_addConnectionsText = std::to_string(cfg::aria2Config().split);
// 添加下载弹窗的每任务高级选项：优先级（0=默认，选择器索引）、重命名、限速、目录。
export int g_addPriority = 0;
export bool g_addPriorityOpen = false;
export std::string g_addRenameText;
export std::string g_addLimitText;
export std::string g_addDirText;

// ---- list filter / pagination / sort ----

export enum class Filter { All, Active, Done };

// 下载列表：状态筛选 + 分页。snapshot() 最新在前，先按筛选收窄，再按
// 当前页切片。切换筛选或分页大小时回到第 1 页。
export Filter g_filter = Filter::All;
export int g_page = 1;
export int g_pageSize = 5;
export bool g_pageSizeOpen = false;  // 分页大小下拉是否展开
export constexpr int kPageSizes[] = {5, 10, 20, 50, 100};

// 下载列表排序。切换排序回到第 1 页。
export enum class SortMode { Newest, State, Name, Size, Progress, Priority };
export SortMode g_sort = SortMode::Newest;
export bool g_sortOpen = false;  // 排序下拉是否展开

// 单实例：CLI 启动参数是否已添加过；inbox 轮询计时。
export bool g_cliHandled = false;
export float g_inboxTimer = 0.0f;

// "下载中" = 排队/进行/暂停；"已完成" = 完成/失败/已取消。
export bool stateMatches(Filter filter, dl::State state) {
    switch (filter) {
        case Filter::All: return true;
        case Filter::Active:
            return state == dl::State::Queued || state == dl::State::Downloading ||
                   state == dl::State::Paused;
        case Filter::Done:
            return state == dl::State::Done || state == dl::State::Failed ||
                   state == dl::State::Cancelled;
    }
    return true;
}

export int pageSizeIndex() {
    for (int i = 0; i < 5; ++i) {
        if (kPageSizes[i] == g_pageSize) return i;
    }
    return 0;  // 5
}

// ---- add-download flow ----

// 优先级选择器索引 → aria2 priority 数值（0=默认）。aria2 的 priority 范围
// 1..100、数值方向待随包二进制验证；若相反只需改这个函数的返回值。
export int priorityValueFromPicker(int index) {
    switch (index) {
        case 1: return 3;   // 高
        case 2: return 2;   // 中
        case 3: return 1;   // 低
        default: return 0;  // 默认
    }
}

// 校验并启动一个下载；返回是否成功。完整的每任务选项在 opts 里（连接数/优先级/
// 重命名/限速/目录）。对话框 / CLI / 单实例 inbox 共用。
export bool startDownloadFromUrl(std::string url, const dl::StartOptions& opts) {
    const std::size_t first = url.find_first_not_of(" \t\r\n");
    const std::size_t last = url.find_last_not_of(" \t\r\n");
    url = first == std::string::npos ? "" : url.substr(first, last - first + 1);
    if (url.empty()) {
        showStatus("请输入下载地址");
        return false;
    }

    const bool magnet = url.starts_with("magnet:");
    // aria2 原生支持 http/https/magnet；http 不再强制升级为 https（那是 tinyhttps 的限制）。
    if (!url.starts_with("http://") && !url.starts_with("https://") && !magnet) {
        showStatus("仅支持 http(s) 链接或 magnet: 磁力链接");
        return false;
    }
    // 下载目录：opts.dirOverride 覆盖（相对路径按配置目录解析）。
    std::filesystem::path dir = opts.dirOverride.empty()
        ? cfg::downloadDir()
        : opts.dirOverride;
    if (dir.is_relative()) dir = cfg::downloadDir() / dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    // 文件名：优先重命名；磁力没有 URL 文件名，用占位名（拿到元数据后引擎会更新）。
    std::string name = opts.outputName.empty()
        ? (magnet ? "magnet" : fileNameFromUrl(url))
        : opts.outputName;
    if (name.empty()) name = "magnet";

    const std::filesystem::path dest = dir / name;
    const std::uint64_t id = g_manager->start(url, dest, opts);
    if (id == 0) {
        showStatus("下载启动失败：引擎不可用");
        return false;
    }
    showStatus(std::format("已开始下载 #{} — {}", id, name));
    return true;
}

// 兼容重载：仅 URL + 连接数（CLI / inbox 用），其余选项取默认。
export bool startDownloadFromUrl(std::string url, int connections) {
    dl::StartOptions opts;
    opts.connections = connections;
    return startDownloadFromUrl(std::move(url), opts);
}

// “添加下载”弹窗提交：URL + 每任务高级选项（连接数/优先级/重命名/限速/目录）。
export bool addDownload() {
    dl::StartOptions opts;
    std::string t = g_addConnectionsText;
    if (!t.empty()) {
        try {
            opts.connections = std::clamp(std::stoi(trimText(t)), 0, 64);
        } catch (...) {
            opts.connections = 0;
        }
    }
    opts.priority = priorityValueFromPicker(g_addPriority);
    opts.outputName = trimText(g_addRenameText);
    opts.dirOverride = trimText(g_addDirText);
    if (!trimText(g_addLimitText).empty()) {
        opts.limitBps = static_cast<std::int64_t>(
            parseIntClamped(g_addLimitText, 0, 1000000, 0)) * 1024;
    }
    return startDownloadFromUrl(g_urlText, opts);
}

// 每帧检查任务状态迁移：仅当任务从进行中（排队/下载/暂停）迁移到 Done/Failed 时
// 发系统通知（避免会话恢复等历史状态误触发）。由 app.cpp 的 onFrame 调用。
export void checkDownloadNotifications() {
    static std::unordered_map<std::uint64_t, dl::State> lastStates;
    const auto tasks = g_manager->snapshot();
    std::unordered_set<std::uint64_t> seen;
    seen.reserve(tasks.size());
    for (const auto& t : tasks) {
        seen.insert(t.id);
        const auto it = lastStates.find(t.id);
        if (it != lastStates.end()) {
            const dl::State prev = it->second;
            const bool wasActive = prev == dl::State::Queued ||
                                   prev == dl::State::Downloading ||
                                   prev == dl::State::Paused;
            if (wasActive && prev != t.state) {
                const std::string name = fileNameFromUrl(t.url);
                if (t.state == dl::State::Done) {
                    notifyDownload("下载完成", name + " 已下载完成");
                } else if (t.state == dl::State::Failed) {
                    notifyDownload("下载失败", name + " 下载失败");
                }
            }
        }
        lastStates[t.id] = t.state;
    }
    // 清掉已从列表移除的任务记录，避免 map 无限增长。
    for (auto it = lastStates.begin(); it != lastStates.end();) {
        if (seen.count(it->first) == 0) {
            it = lastStates.erase(it);
        } else {
            ++it;
        }
    }
}
