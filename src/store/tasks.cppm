// store/tasks.cppm — 领域 store：任务数据与下载命令的唯一入口（JS 语境的
// "store" = 状态 + 只允许通过方法变更状态）。UI 无关：不 import 任何 eui / ui.*
// 模块，GUI / CLI / headless / 未来的其他前端共用。
//
// 纪律：
//   - 引擎对象由 TaskStore 持有，外部不再直接碰 dl::DownloadEngine 指针；
//   - startFromUrl 返回 StartResult{ok, message}，不自己弹状态消息——消息展示
//     是 UI 层的事（store.ui 的 showStatus / headless 的 stdout）；
//   - 线程模型继承引擎层：warmup 可从后台线程调用（引擎内部 daemonMutex_
//     互斥），其余命令在 UI 线程调用；snapshot 内部有锁，任意线程可读。
//
// Boot-order note: g_tasks 的动态初始化（构造引擎对象）先于任何 import 本模块
// 的 TU 的静态初始化执行。主实例没问题；第二实例在 CLI boot 退出前会短暂构造
// 一次引擎对象——与旧 ui/state.cppm 的 g_manager 相同，无害（不 spawn daemon）。
export module tinynext.store.tasks;

import std;
import tinynext.config;
import tinynext.aria2_engine;
import tinynext.download_engine;
import tinynext.i18n;   // tr / trf（结果消息按语言）
import tinynext.utils;

// 任务显示名：BT/磁力优先用种子真实名（displayName，bittorrent.info.name）；
// 否则用真实下载路径的文件名（HTTP 经 Content-Disposition 解析后的最终名，替换
// URL 末尾 uuid 占位）；占位（magnet-N）回退 URL 文件名。
export std::string taskDisplayName(const dl::TaskView& task) {
    if (!task.displayName.empty()) return task.displayName;
    const std::string fp = task.destPath.filename().string();
    if (!fp.empty() && fp.rfind("magnet-", 0) != 0) return fp;
    return fileNameFromUrl(task.url);
}

// startFromUrl 的结果：ok + 给用户看的消息（成功/失败都有）。UI 层决定怎么
// 展示（状态条 / 终端）。
export struct StartResult {
    bool ok = false;
    std::string message;
    std::uint64_t id = 0;   // 成功时的任务 id
};

// 删除任务后清理 aria2 的 .aria2 控制文件（下载缓存）。best-effort，失败静默。
void removeControlFile(const std::filesystem::path& destPath) {
    std::filesystem::path control = destPath;
    control += ".aria2";
    std::error_code ec;
    std::filesystem::remove(control, ec);
}

export class TaskStore {
public:
    TaskStore() : engine_(std::make_unique<dl::Aria2Engine>()) {}

    TaskStore(const TaskStore&) = delete;
    TaskStore& operator=(const TaskStore&) = delete;

    // ---- 查询 ----
    std::vector<dl::TaskView> snapshot() const { return engine_->snapshot(); }
    // 后台线程进度轮询（~1s 节流；UI 线程不要调，见 engine_->pollProgress 注释）。
    void pollProgress() { engine_->pollProgress(); }
    bool busy() const { return engine_->busy(); }
    bool engineActive() const { return engine_->engineActive(); }
    std::string lastError() const { return engine_->lastError(); }

    // ---- 生命周期 ----
    // 启动预热：拉起 daemon + 恢复上次会话历史任务（可后台线程调用，引擎内部
    // 有 daemonMutex_ 与 UI 线程互斥）。见 app.cpp 的预热线程。
    void warmup() { engine_->warmup(); }
    void shutdown() { engine_->shutdown(); }

    // ---- 任务命令（UI 线程）----
    void cancel(std::uint64_t id) { engine_->cancel(id); }
    void pause(std::uint64_t id) { engine_->pause(id); }
    void resume(std::uint64_t id) { engine_->resume(id); }
    void pauseAll() { engine_->pauseAll(); }
    void resumeAll() { engine_->resumeAll(); }
    void retry(std::uint64_t id) { engine_->retry(id); }
    bool addMirror(std::uint64_t id, const std::string& url) {
        return engine_->addMirror(id, url);
    }
    bool removeMirror(std::uint64_t id, const std::string& url) {
        return engine_->removeMirror(id, url);
    }

    // 删除任务记录（daemon 会话 + 本地任务表）并清理下载缓存（.aria2 控制
    // 文件）。源文件是否删除由 UI 层的删除确认弹窗决定，不在本方法职责内。
    void deleteRecord(const dl::TaskView& task) {
        engine_->remove(task.id);
        removeControlFile(task.destPath);
    }

    // ---- 添加下载 ----
    // 校验并启动一个下载。完整的每任务选项在 opts 里（连接数/重命名/目录/镜像）。
    // 对话框 / CLI / 单实例 socket / inbox 共用。不碰 UI 状态，结果经
    // StartResult 返回。
    StartResult startFromUrl(std::string url, const dl::StartOptions& opts) {
        const std::size_t first = url.find_first_not_of(" \t\r\n");
        const std::size_t last = url.find_last_not_of(" \t\r\n");
        url = first == std::string::npos ? "" : url.substr(first, last - first + 1);
        if (url.empty()) {
            return {false, tr("请输入下载地址", "Please enter a download URL")};
        }

        const bool magnet = url.starts_with("magnet:");
        // aria2 原生支持 http/https/ftp/sftp/magnet；http 不再强制升级为 https。
        // 本地 .torrent 文件路径也放行（走 addTorrent）。
        const bool torrentFile = url.ends_with(".torrent") && std::filesystem::exists(url);
        if (!isDownloadableSource(url) && !torrentFile) {
            return {false, tr("仅支持 http(s) / ftp(s) / sftp 链接、magnet: 磁力链接或本地 .torrent 文件",
                              "Only http(s) / ftp(s) / sftp links, magnet: URIs, or local .torrent files are supported")};
        }
        // 下载目录：opts.dirOverride 覆盖（相对路径按配置目录解析）。
        std::filesystem::path dir = opts.dirOverride.empty()
            ? cfg::downloadDir()
            : opts.dirOverride;
        if (dir.is_relative()) dir = cfg::downloadDir() / dir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        // 文件名：优先重命名；磁力 / .torrent 没有可用的 URL 文件名，用占位名
        // （拿到元数据后引擎会从 files[0].path 更新为种子真实名）。
        std::string name = opts.outputName.empty()
            ? (torrentFile ? "torrent" : (magnet ? "magnet" : fileNameFromUrl(url)))
            : opts.outputName;
        if (name.empty()) name = "torrent";

        const std::filesystem::path dest = dir / name;
        const std::uint64_t id = engine_->start(url, dest, opts);
        if (id == 0) {
            // 引擎给不出原因（如没实现 lastError）时回退到笼统提示。
            const std::string err = engine_->lastError();
            return {false, err.empty() ? tr("下载启动失败：引擎不可用",
                                            "Failed to start download: engine unavailable")
                                       : trf("下载启动失败：{}",
                                             "Failed to start download: {}", err)};
        }
        return {true, trf("已开始下载 #{} — {}", "Started download #{} — {}", id, name), id};
    }

    // 兼容重载：仅 URL + 连接数（CLI / inbox 用），其余选项取默认。
    StartResult startFromUrl(std::string url, int connections) {
        dl::StartOptions opts;
        opts.connections = connections;
        return startFromUrl(std::move(url), opts);
    }

private:
    std::unique_ptr<dl::DownloadEngine> engine_;
};

// 全局唯一任务 store（模块级导出变量在 importers 间共享同一实体）。
export TaskStore g_tasks;
