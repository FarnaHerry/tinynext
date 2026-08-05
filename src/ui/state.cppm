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
import tinynext.download_manager;
import tinynext.aria2_engine;
import tinynext.download_engine;
import tinynext.ui.utils;

// ---- download engine ----
// Download engine factory: the UI talks to the abstract interface only; the
// concrete engine is chosen from config ("engine": tinyhttps | aria2next).
export std::unique_ptr<dl::DownloadEngine> createEngine() {
    if (cfg::engine() == cfg::EngineChoice::Aria2Next) {
        return std::make_unique<dl::Aria2Engine>();
    }
    return std::make_unique<dl::TinyHttpsEngine>();
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
// 设置页待提交的编辑值：主题/引擎/路径只在点「保存」时写入配置并生效，
// 点「放弃」回滚到已保存值。主题在选择时即时预览（g_dark），但不落盘。
export cfg::ThemeMode g_pendingTheme = g_themeMode;
export cfg::EngineChoice g_pendingEngine = cfg::engine();
// aria2 参数待提交值（设置页输入框的文本形式）。
export std::string g_aria2SplitText = std::to_string(cfg::aria2Config().split);
export std::string g_aria2ConnText = std::to_string(cfg::aria2Config().maxConnectionPerServer);
export std::string g_aria2MinSplitText = cfg::aria2Config().minSplitSize;
export std::string g_aria2LimitText = std::to_string(cfg::aria2Config().maxDownloadLimit);
// 添加下载弹窗的每任务连接数（默认 = 配置 split 值；空/0 = 配置默认）。
export std::string g_addConnectionsText = std::to_string(cfg::aria2Config().split);

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
export enum class SortMode { Newest, State, Name, Size, Progress };
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

// 校验并启动一个下载；返回是否成功。connections > 0 时作为该任务的每任务
// 连接数传给引擎（0 = 引擎按配置默认）。对话框 / CLI / 单实例 inbox 共用。
export bool startDownloadFromUrl(std::string url, int connections) {
    const std::size_t first = url.find_first_not_of(" \t\r\n");
    const std::size_t last = url.find_last_not_of(" \t\r\n");
    url = first == std::string::npos ? "" : url.substr(first, last - first + 1);
    if (url.empty()) {
        showStatus("请输入下载地址");
        return false;
    }
    if (!url.starts_with("https://")) {
        if (url.starts_with("http://")) {
            // tinyhttps only speaks HTTPS; best-effort upgrade.
            url = "https://" + url.substr(7);
        } else {
            showStatus("仅支持 HTTPS 下载链接");
            return false;
        }
    }

    const std::filesystem::path dest =
        cfg::downloadDir() / fileNameFromUrl(url);
    dl::StartOptions opts;
    opts.connections = connections;
    const std::uint64_t id = g_manager->start(url, dest, opts);
    if (id == 0) {
        showStatus("下载启动失败：引擎不可用");
        return false;
    }
    showStatus(std::format("已开始下载 #{} — {}", id, dest.filename().string()));
    return true;
}

// “添加下载”弹窗提交：URL + 每任务连接数（空/0 = 配置默认）。
export bool addDownload() {
    std::string t = g_addConnectionsText;
    int connections = 0;
    if (!t.empty()) {
        try {
            connections = std::clamp(std::stoi(trimText(t)), 0, 64);
        } catch (...) {
            connections = 0;
        }
    }
    return startDownloadFromUrl(g_urlText, connections);
}
