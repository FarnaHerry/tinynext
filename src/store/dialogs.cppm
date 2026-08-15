// store/dialogs.cppm — 视图 store：各弹窗（添加下载 / 镜像源 / 删除确认 / 关于）
// 的打开状态与未提交输入，以及弹窗的提交动作（addDownload / requestDelete）。
// 不 import eui：弹窗的"状态机"与渲染分离，downloads_page 只负责画。
//
// 提交动作 = 读本 store 的草稿 → 组 dl::StartOptions → 调领域 store
// （g_tasks.startFromUrl）→ 把 StartResult.message 交给 store.ui 的 showStatus。
export module tinynext.store.dialogs;

import std;
import tinynext.config;
import tinynext.download_engine;
import tinynext.store.tasks;
import tinynext.store.ui;
import tinynext.utils;

// ---- 弹窗开关 / 通用 ----

export bool g_addOpen = false;   // “添加下载”弹窗是否打开
export bool g_aboutOpen = false; // “关于”弹窗是否打开
export std::string g_urlText;    // 添加弹窗的 URL 输入（直链 tab；可多行配镜像）

// ---- 镜像源管理弹窗 ----
// g_mirrorTaskId 选中任务；g_mirrorAddText 待添加的镜像源。
export bool g_mirrorOpen = false;
export std::uint64_t g_mirrorTaskId = 0;
export std::string g_mirrorAddText;

// ---- 添加下载弹窗 ----

// 添加下载弹窗顶部切换：直链下载 / 种子（两者流程与字段不同，各自独立 pending）。
export enum class AddTab { Direct, Torrent };
export AddTab g_addTab = AddTab::Direct;

// 添加下载弹窗的每任务连接数（默认 = 配置 split 值；空/0 = 配置默认）。
export std::string g_addConnectionsText = std::to_string(cfg::aria2Config().split);
// 添加下载弹窗的每任务高级选项：重命名、目录、本地 .torrent 文件路径（空=无）、
// 镜像多源（勾选时多行 URL 首行为主、其余为同一任务的镜像源）。
export std::string g_addRenameText;
export std::string g_addDirText;
export std::string g_addTorrentPath;
export bool g_addMirror = false;

// “添加下载”弹窗提交：按顶部切换（直链下载 / 种子）分流。
// 直链：URL/磁力 + 连接数/重命名/目录/镜像；种子：本地 .torrent + 目录。
export bool addDownload() {
    dl::StartOptions opts;
    if (g_addTab == AddTab::Torrent) {
        const std::string torrent = trimText(g_addTorrentPath);
        if (torrent.empty()) {
            showStatus("请先选择 .torrent 种子文件");
            return false;
        }
        opts.torrentPath = torrent;
        opts.dirOverride = trimText(g_addDirText);
        const auto r = g_tasks.startFromUrl(torrent, opts);
        showStatus(r.message);
        return r.ok;
    }

    std::string t = g_addConnectionsText;
    if (!t.empty()) {
        try {
            opts.connections = std::clamp(std::stoi(trimText(t)), 0, 64);
        } catch (...) {
            opts.connections = 0;
        }
    }
    opts.outputName = trimText(g_addRenameText);
    opts.dirOverride = trimText(g_addDirText);
    std::string url = g_urlText;
    if (g_addMirror) {
        // 镜像多源：URL 框多行 → 首行为主 URL，其余为同一任务的镜像源（aria2 从
        // 多源并发分段下载同一文件、源挂自动切换）。
        std::vector<std::string> lines;
        std::istringstream ss(url);
        std::string line;
        while (std::getline(ss, line)) {
            const std::string lt = trimText(line);
            if (!lt.empty()) lines.push_back(lt);
        }
        if (lines.size() > 1) {
            url = lines[0];
            opts.mirrors.assign(lines.begin() + 1, lines.end());
        }
    }
    const auto r = g_tasks.startFromUrl(url, opts);
    showStatus(r.message);
    return r.ok;
}

// ---- 删除任务确认弹窗 ----
// 已完成任务删除前弹框选择；未完成任务直接删记录+清缓存。
// g_pendingDelete 非空时，下载页渲染删除确认弹窗。
export std::optional<dl::TaskView> g_pendingDelete;
// 删除弹窗里"同时删除源文件"复选框的状态。默认勾选（源文件移到回收站，可恢复），
// 每次打开弹窗时在 requestDelete 里重置为默认。
export bool g_deleteIncludeFiles = true;

// 删除/取消按钮统一入口：X 与垃圾桶都弹确认框，问是否删除任务 + 勾选是否删源文件。
export void requestDelete(const dl::TaskView& task) {
    g_deleteIncludeFiles = true;  // 每次弹窗恢复默认（删除源文件 → 回收站）
    g_pendingDelete = task;
}
