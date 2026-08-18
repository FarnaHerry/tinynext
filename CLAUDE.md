# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

TinyNext 是一个 **C++23 模块化 GUI 下载器**：EUI-NEO 前端 + **aria2-next** 外部进程
引擎（唯一引擎，TinyHttpsEngine 已移除），单实例 + 命令行传参。跨平台（Windows /
Linux / macOS），用 **mcpp** 构建。仓库还维护了一份更全的 `AGENTS.md`（同样面向 AI，
含全部模块约定与踩坑）；本文档是 Claude Code 的操作要点，细节以 `AGENTS.md` 与
`docs/roadmap.md` 为准。

## 构建 / 运行 / 打包

```bash
mcpp build          # 编译（dev）
mcpp build --release
mcpp run            # 启动 GUI（Linux 用 ./run.sh，见下）
.\make-dist.ps1     # Windows 发行打包 → dist\ + tinynext-v<版本>-win64.zip
bash make-dist.sh <os> <arch>   # Linux / macOS 打包 → tar.gz
```

- 工具链固定 `llvm@22.1.8`、eui-neo 锁 **0.5.6**、websocket（IXWebSocket 包装）
  **12.0.1**、nlohmann::json **3.12.0**（都在 `mcpp.toml` / `mcpp.lock`，不要乱升）。
- **版本只在 mcpp.toml 维护**：应用版本 `[package].version` 与 eui/websocket/json 依赖
  版本由 `scripts/gen-versions.ps1`（或 `.sh`）生成 `src/versions.generated.h`，
  `config.cppm` 的 `kAppVersion`/`kEuiVersion` 读它。升版本后跑一次生成脚本
  （`make-dist` 打包前会自动跑），不要手改生成头。
- **Linux 用 `./run.sh` 启动**而非 `mcpp run`：mcpp 私有 glibc 与系统 Mesa 的
  GLIBC 版本冲突，run.sh 走系统 ld.so + 系统 Mesa。
- **没有测试**：eui-neo 的 `app-main` 特性会把 `glfw_app_main.o` 急切链入，与任何
  定义 `main()` 的测试 TU 冲突（`multiple definition of 'main'`），tests/ 已删除。
- CI：`.github/workflows/release.yml`。push `v*` 标签自动三平台构建 + 发布 draft
  Release；`workflow_dispatch` 只构建上传 artifacts（用于先修跨平台编译错误）。
- 提交：本地 commit 后由用户自行 push，不要代 push。

## CLI 添加下载（AI 最常用）

```bash
tinynext https://example.com/file.zip   # 添加下载；应用没开会自动启动
tinynext url1 url2                       # 一次多个
tinynext agent                           # 打印 CLI 使用教学（给 AI 用）后退出
```

- **单实例**：第二实例经 TCP loopback socket 把 URL 直发主实例（回退写
  `<temp>/tinynext.inbox`；主实例后台线程阻塞 accept，收到即唤醒 UI），Windows 上
  还聚焦窗口）后退出，不弹新窗口。
- 接受 `http://` / `https://` / `ftp://` / `ftps://` / `sftp://` 前缀、`magnet:` 磁力、
  或以 `.torrent` 结尾的本地文件路径；其他参数忽略。http 不强制升级 https。白名单
  统一在 `isDownloadableSource`（`src/utils.cppm`，`tinynext.utils`）。
- 不记得用法时先跑 `tinynext agent`。详细见 `docs/cli.md`。

## 架构

全模块化（`import std` + 各 `tinynext.*` 模块），UI 只面向引擎抽象：

| 模块 | 文件 | 职责 |
|------|------|------|
| `tinynext.download_engine` | `src/download_engine.cppm` | 抽象接口 `dl::DownloadEngine` / `TaskView` / `State` / `StartOptions` |
| `tinynext.aria2_engine` | `src/aria2_engine.cppm/.cpp`（~1.1k 行） | aria2-next 进程引擎：JSON-RPC + 本地 socket + WebSocket 推送 |
| `tinynext.config` | `src/config.cppm` | 配置 / 主题 / 下载目录 / aria2 参数，落盘 `tinynext.conf` |
| `tinynext.utils` | `src/utils.cppm` | 纯 string/number 帮助函数（无 UI 依赖，CLI/headless/store 共用） |
| `tinynext.store.tasks` | `src/store/tasks.cppm` | **领域 store**：`TaskStore` 持有引擎 + 任务命令 + `startFromUrl` + 删除记录；`g_tasks` 单例 |
| `tinynext.store.ui` | `src/store/ui.cppm` | 视图 store：状态消息 / 页面 / 筛选·排序·分页（无 eui） |
| `tinynext.store.dialogs` | `src/store/dialogs.cppm` | 视图 store：添加/镜像/删除/关于弹窗状态机 + `addDownload`/`requestDelete`（无 eui） |
| `tinynext.cli` | `src/cli.cppm` | 单实例锁 + 命令行 URL + TCP socket 转发 + CliBoot 引导 |
| `tinynext.video_resolver` | `src/video_resolver.cppm` | 视频解析：外挂 yt-dlp 进程出直链（`VideoInfo`/`VideoFormat`），领域层无 eui |
| `tinynext.video_merge` | `src/video_merge.cppm` | DASH 编排：`MergeTracker` 聚合音/视频子任务成单个合成任务 + ffmpeg 合并 |
| `tinynext.ui.*` | `src/ui/*.cppm` | utils（布局常量）/ theme / platform / housekeep / widgets / cards / downloads_page / video_page / settings_page / about_dialog |
| `src/app.cpp` | 普通 TU | 薄入口：`app::dslAppConfig()` + `app::compose()` |

**入口**：`main()` 由 eui-neo 的 `app-main` 提供（GLFW 入口），任何 TU 都不能再定义
`main()`。0.5.6 起 `eui_neo.h` 不再包含 `eui/detail/dsl_app_impl.h`（`app::update/render`
机制已挪进 app-main 的 `glfw_app_main.cpp` 内部编译）；`src/app.cpp` 包含完整
`<eui_neo.h>` 只为取 `DslAppConfig` / `app::compose` 声明。各 UI 模块仍用精简头
`src/ui/eui_ui.h`（0.5.6 已无 mangled name 冲突，纯为保持最小 include 面，历史原因保留）。

**共享状态（store 分层）**：可变状态按「领域 / 视图」分层，模块级导出变量在
importers 间共享同一实体，直接读写：
- `tinynext.store.tasks` — **领域 store**（不 import 任何 ui.*/eui）：`g_tasks`
  （`TaskStore`）持有引擎对象，任务命令（pause/resume/retry/mirror/删除记录）、
  启动预热 `warmup()`、下载校验/启动流程 `startFromUrl`（返回 `StartResult{ok,
  message}`，自己不做 UI 提示；弹窗 / CLI / socket/inbox 共用）都在这里；
- `tinynext.store.ui` / `tinynext.store.dialogs` — **视图 store**（仍无 eui 依赖，
  但属「这个 UI 的实现细节」）：状态消息 / 筛选·排序·分页 / 各弹窗草稿与开关；
- 设置页 pending 草稿是 `settings_page.cppm` 的模块私有状态（无人跨模块用）；
  主题全局（g_dark/g_themeMode/...）归位 `tinynext.ui.theme`。
旧 `tinynext.ui.state`（上帝 store）已删除——新状态先想清楚属于哪一层再放。

**渲染循环 / 事件驱动（重要）**：`app::compose` **不能挂 `.onFrame`** —— eui 会把挂
onFrame 的元素当成「每帧都在动」，无条件每帧 composeRequested/paintRequested/animating，
导致空闲也 90 FPS 全量重绘（GPU 占用跳跃）。周期/事件工作都在后台线程、只在真有事时
`core::platform::requestUiUpdate()` 唤醒 UI 一帧：
- `cli::startCliIpc()` — 后台线程阻塞 accept（TCP loopback），收到转发 URL 才唤醒；
- `app.cpp` 启动预热线程 — 一次性：后台 `engine->warmup()` 拉起 aria2 daemon 并
  恢复上次会话历史任务（不再是首次下载才懒惰 spawn，历史记录启动即显示），完成
  后唤醒 UI；与 UI 线程的 start/retry 经引擎 `daemonMutex_` 互斥；
- `housekeep::startHousekeeping()` — 后台线程每 500ms 查状态消息过期 / 下载通知迁移 /
  活动任务进度，有变化才唤醒；
- `theme_watch` — 主题变化时直接唤醒；
- `aria2_engine::handleWsEvent` — 状态迁移（完成/失败）后唤醒。
空闲时 UI 走 `glfwWaitEvents` 睡眠，零渲染零 CPU。

**引擎线程模型**（`aria2_engine.cpp`）：JSON-RPC 请求走 UI 线程（`rpcCall`，HTTP），
WebSocket 只收 aria2 的 6 个推送事件（onDownloadStart/Pause/Stop/Complete/Error/
BtDownloadComplete，无进度事件）驱动状态迁移；进度靠 ~1s 轮询补。WS 回调跑在
IXWebSocket 内部线程，所以 `tasks_` 一律经 `tasksMutex_` 访问。**锁纪律**：持锁方法
snapshot/start/action/recoverSession/handleWsEvent；`ensureDaemon`/`applyTellStatus`
不持锁；start/retry 先 `ensureDaemon` 再取锁（避免非递归互斥量死锁）。WS 掉线自动
回退纯轮询。动机与验证见 `docs/roadmap.md`。

**会话恢复**：daemon 启动带 `--save-session`/`--input-file`，`shutdown()` 先
`aria2.saveSession` 再 forceShutdown；重启后 `recoverSession()` 用
tellActive/tellWaiting/tellStopped 重建任务表。

## 关键约定（改代码前必读）

1. **`import std;` 后禁止再 `#include` 标准头**（std 模块已声明，否则 redefinition 报错）。
2. **UI 模块加 include 用 `"eui_ui.h"`**，不要用完整 `<eui_neo.h>`。
3. **缩放**：eui-neo 0.5.6 起用 `DslAppConfig::uiScale(kUI)`（`kUI=1.4`）原生放大
   整个逻辑坐标系（布局+字号）。所有尺寸按**设计逻辑像素**直接书写，**不再**用
   `utils::S()` 自乘；窗口物理尺寸 = 设计尺寸 × `kUI`（`app.cpp` 里
   `windowSize(1120*kUI, 720*kUI)`）。
4. **每任务选项**：`dl::StartOptions{connections, outputName, dirOverride, limitBps}`
   全部生效。注意 aria2-next **没有下载级 `priority` 选项**（实测 + `--help=#all`
   确认），优先级功能已移除，别再加回去。
5. **磁力**：magnet 任务不设 `out`，真实路径由 `refreshStates` 从 `files[0].path` 更新；
   重命名 / 下载目录解析逻辑都在 `TaskStore::startFromUrl`。
6. **重新下载**：Failed/Cancelled 卡片 ↻ 调 `g_tasks.retry(id)`，aria2 复用原 URL+路径
   + `continue=true` 从 `.aria2` 续传。
7. **aria2 字段名**：用 `connections`（不是 `numConnections`）；进程名 Windows 是
   `aria2-next.exe`、unix 是 `aria2-next`。RPC 只监听 127.0.0.1 + 随机 `--rpc-secret`。
8. **Winsock**：Windows 本地 socket 需 `WSAStartup`，由 `LocalSocket::platformInit()`
   （POSIX no-op）在 `Aria2Engine` 构造/析构里配对，不要漏。
9. 引擎二进制在 `engines/`（已 gitignore，`checksums.sha256` 保留）；缺失时下载不可用，
   打包脚本会警告但继续。
10. **eui 元素 id 全局唯一**：同 frame 同名 id 会互相覆盖（如标签 id 与
    `buildListPicker` 内部的 `x.label` 撞名 → 文字不显示）。
11. **Windows 打开文件/文件夹走 `ShellExecuteW`**（`platform.cppm::shellExecFn()`），
    不要用 `std::system("explorer …")`（会同步等 Explorer 关闭、卡 UI 线程）。
12. **xlings 解压含 symlink 的 tarball 在 Windows 会中途失败**：若 `mcpp build` 报
    `install_packages failed` 而下载/校验正常，多半是包解压不完整（如 IXWebSocket 的
    `Dockerfile` symlink）。手工补装：`curl -L` 拉校验过的 tarball → 用系统 tar 解压
    （跳过 symlink）到 `~/.mcpp/registry/data/xpkgs/<ns>-x-<name>/<ver>/` + `mcpp_generated/`，
    再 `mcpp build`。

## 常用文档

- `README.md` — 功能 / 设置项 / 发布流程的完整说明。
- `AGENTS.md` — AI 助手指南（更全的模块约定与踩坑，与本文档互相引用）。
- `docs/roadmap.md` — 决策记录（纯 aria2、WebSocket 方案、eui-neo 版本等）。
- `docs/cli.md` — CLI 单实例转发细节与排查。
