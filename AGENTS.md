# AGENTS.md — 给 AI 助手的项目指南

TinyNext 是一个 **C++23 模块化 GUI 下载器**：EUI-NEO 前端 + **aria2-next** 外部
进程引擎（唯一引擎，TinyHttpsEngine 已移除），单实例，带命令行传参。跨平台
（Windows / Linux / macOS），用 **mcpp** 构建。

## 构建 / 运行

```bash
mcpp build          # 编译（dev）
mcpp build --release
mcpp run            # 启动 GUI（Linux 用 run.sh）
```

- 工具链在 `mcpp.toml` 里固定为 `llvm@22.1.8`，不要改。
- eui-neo 锁在 **0.5.8**（配方加 `-fno-char8_t` 修 C++23 构建 + 补 `-ldwmapi`，见 `docs/roadmap.md`），不要乱升。
- Windows 发行打包：`.\make-dist.ps1`；Linux/macOS：`bash make-dist.sh <os> <arch>`。
- CI：`.github/workflows/release.yml`，push `v*` 标签自动三平台构建 + 发布。

## 用 CLI 添加下载（AI 最常用）

```bash
tinynext https://example.com/file.zip     # 添加下载；应用没开会自动启动
tinynext url1 url2                         # 一次多个
tinynext agent                             # 打印 CLI 使用教学（给 AI 用），退出
```

- **单实例**：重复启动不弹新窗口——第二实例经 TCP loopback socket 把 URL 直发
  主实例（回退写 `<temp>/tinynext.inbox`，Windows 上还会聚焦窗口）后退出。
- 可下载源：`http(s)://` / `ftp(s)://` / `sftp://` 链接、`magnet:` 磁力、`.torrent`
  本地路径；白名单统一在 `isDownloadableSource`（`src/utils.cppm`，`tinynext.utils`）。
  非下载参数忽略。
- `agent` / `--agent` / `help` 参数会打印 CLI 使用教学并退出（不进 GUI）——AI
  不知道用法时先跑 `tinynext agent`。
- 详细：`docs/cli.md`。

## 代码结构（全模块）

| 模块 | 文件 | 职责 |
|------|------|------|
| `tinynext.download_engine` | `src/download_engine.cppm` | 引擎接口 `dl::DownloadEngine` |
| `tinynext.aria2_engine` | `src/aria2_engine.cppm/.cpp` | aria2-next 引擎（JSON-RPC + 本地 socket） |
| `tinynext.config` | `src/config.cppm` | 配置 / 主题 / 下载目录 |
| `tinynext.utils` | `src/utils.cppm` | 纯 string/number 帮助函数（无 UI 依赖） |
| `tinynext.store.tasks` | `src/store/tasks.cppm` | 领域 store：`TaskStore` + `g_tasks`（引擎 + 任务命令 + startFromUrl） |
| `tinynext.store.ui` | `src/store/ui.cppm` | 视图 store：状态消息 / 页面 / 筛选·排序·分页 |
| `tinynext.store.dialogs` | `src/store/dialogs.cppm` | 视图 store：弹窗状态机 + addDownload/requestDelete |
| `tinynext.cli` | `src/cli.cppm` | 单实例 + 命令行 URL + TCP socket 转发 |
| `tinynext.video_resolver` | `src/video_resolver.cppm` | 视频解析（外挂 yt-dlp 进程，`-J` JSON → `VideoInfo`/`VideoFormat`；领域层无 eui） |
| `tinynext.video_merge` | `src/video_merge.cppm` | DASH 音视频合并编排：`MergeTracker` 聚合两个 aria2 子任务成单个合成任务 + ffmpeg 合并 |
| `tinynext.ui.*` | `src/ui/*.cppm` | utils（布局常量）/ theme / platform / housekeep / widgets / cards / downloads_page / video_page / settings_page / about_dialog |
| `src/app.cpp` | 普通 TU | 入口：`app::dslAppConfig()` + `app::compose()` |

页面已按职责拆成独立模块（`pages.cppm` 已删除）：
`downloads_page`（下载页 + 添加下载弹窗）、`video_page`（视频解析页：粘链接 →
yt-dlp 解析 → 选画质 → 下载）、`settings_page`（设置页）、`about_dialog`（关于弹窗）。

### 视频解析（YouTube / bilibili，v0.5）

- **两条下载路线**：
  - **bilibili / 常规站点**：`aria2-next` 做下载引擎——`engines/yt-dlp(.exe)` 只做
    解析（出直链 + 请求头），`engines/ffmpeg(.exe)` 只做 DASH 合并（`-c copy`）。
    两者经 `findEngineBinary` 定位（`<exeDir>/engines/` → cwd/engines/），缺失时仅
    视频功能不可用。
  - **YouTube（googlevideo CDN）**：这类 CDN 拒绝对开放 Range 的首请求（直连 403，
    有代理拦截时更甚，aria2 拿不全流），所以 `VideoFormat::rangeBootstrap` 标记的
    格式走 `MergeTracker::startNativeJob`——yt-dlp **原生下载两个流并自行 ffmpeg 合并**
    （单线程跑 `downloadNativeMerged`），进度按输出文件大小估。这是「使用解析器原生
    能力」的决策，勿改回纯 aria2。
- b 站高画质是音视频分离 DASH：`MergeTracker` 起两个 aria2 子任务（带 yt-dlp 给的
  Referer/UA 头，否则 CDN 403），对外聚合成**单个合成任务**（id ≥ 1,000,000 高段，
  `dl::State::Merging` 表示 ffmpeg 合并中）；housekeep 500ms 轮询触发合并。
- **合并路径坑**：aria2 的 `--auto-file-renaming` 会在重名时把落盘改名为 `xxx (1).m4s`，
  但 MergeTracker 预设路径是原名——ffmpeg 会去开不存在的文件。合并前必须用子任务
  快照的**真实 `destPath`**（`files[0].path`）覆盖预设路径（`pollMerges` 里做）。
- **音频容器**：合并参数按配对音频流决定 `-c:a copy`（aac/m4a 可直装 mp4）还是
  `-c:a aac`（YouTube 的 opus/vorbis 装不进 mp4，需转码）；视频恒 `-c:v copy`。
- Cookie 来源（`cfg::VideoConfig::cookiesBrowser`，JSON `video.cookies_browser`）：
  默认 `"default"`（跟随系统默认浏览器，`video_resolver.cppm` 的
  `detectDefaultBrowserKey()` 探测：Linux 读 `xdg-settings`、Windows 读注册表
  UserChoice、macOS 不探测），yt-dlp 两个 spawn 点（`resolveVideoUrl` /
  `startYtDlpDownload`）统一经 `ytDlpBrowserKey()` 加 `--cookies-from-browser`。
  非 `"off"` 时 SESSDATA 与 cookiesFile 都被忽略；bilibili SESSDATA / cookies_file
  仅 `"off"` 时的手动遗留方案（`resolveVideoUrl` 里按 URL 判断下发 SESSDATA）。
  **Windows 锁库兜底链**：浏览器运行中独占 Cookies 文件（yt-dlp#7271，external-issue，
  yt-dlp 修不了）→ 浏览器模式的所有 yt-dlp 调用都附加 `--cookies <配置目录>/
  browser-cookie-cache.txt`（yt-dlp 对该参数读+写，成功调用退出时自动刷新缓存）；
  `isCookieDbLockedError()` 识别锁库错误（Windows "Could not copy … cookie database"
  与 Linux "Permission denied: …Cookies" 两种形态）后按 ①缓存 ②匿名 顺序重试
  （下载路径在 `runCaptureYtDlp` 内置 finished 前递归，避免轮询抢到中间失败态），
  全失败才报 `vres.cookie_db_locked`。
- 默认画质 / 保留 .m4s 同在设置页「视频」tab（JSON key `"video"`）。
- 已知边界：下载中重启 app，任务表丢失、不再自动合并（bilibili 子任务被会话恢复成
  普通任务 / YouTube 原生任务不恢复），重新下载即可（v1 接受）。

## 关键约定（改代码前必读）

1. **入口**：`main()` 由 eui-neo 的 `app-main` 提供，任何 TU 都不能再定义 `main()`。
2. **禁止在 compose 里挂 `.onFrame`**：eui 会把挂 onFrame 的元素当成「每帧都在动」，
   强制每帧重绘 → 空闲也 90 FPS 满帧（GPU 占用跳跃的根因）。周期/事件工作放后台线程
   （`cli::startCliIpc` / `housekeep::startHousekeeping` / app.cpp 的一次性引擎
   `warmup()` 预热线程），只在真有事时 `core::platform::requestUiUpdate()` 唤醒
   UI 一帧。warmup 与 UI 线程的 start/retry 经引擎 `daemonMutex_` 互斥（锁序
   daemonMutex_ → tasksMutex_）。
3. **`import std;` 后禁止再 `#include` 标准头**（std 模块已声明）。
3. **eui_neo.h 是 header-only 无模块接口**：0.5.6 起 `eui_neo.h` 不再包含
   `eui/detail/dsl_app_impl.h`（`app::update/render` 机制挪进 `app-main` 的
   `glfw_app_main.cpp` 内部编译）。`src/app.cpp` 包含完整 `<eui_neo.h>` 只取声明；
   **UI 模块仍用精简头 `src/ui/eui_ui.h`**（0.5.6 已无 mangled name 冲突，历史原因
   保留）。给 UI 模块加 include 时用 `"eui_ui.h"`。
4. **共享状态（store 分层）**：状态按领域/视图分层——`tinynext.store.tasks`
   （`TaskStore` + `g_tasks`：引擎、任务命令、`startFromUrl` 返回 `StartResult{ok,
   message}` 不做 UI 提示）是领域 store，不 import 任何 ui.*/eui；`store.ui` /
   `store.dialogs` 是视图 store（状态消息、筛选分页、弹窗草稿）；设置页 pending
   草稿是 `settings_page.cppm` 模块私有；主题全局在 `tinynext.ui.theme`。旧
   `tinynext.ui.state` 已删除，新状态先想清楚属于哪一层。
5. **每任务选项**：`dl::StartOptions{connections, outputName, dirOverride, limitBps}`，
   `Aria2Engine` 全部生效（connections/limitBps 需 >0）。注意 aria2-next **没有下载级
   priority 选项**（实测 + `--help=#all` 确认），优先级功能已移除，别再加回去。
6. **磁力**：`g_tasks.startFromUrl` 接受 `magnet:` 前缀；magnet 任务不设 `out`，
   destPath 由 `refreshStates` 从 `files[0].path` 更新为真实路径。
7. **重新下载**：Failed/Cancelled 卡片 ↻ 调 `g_tasks.retry(id)`（委托 `DownloadEngine`
   接口）。aria2 复用原 URL+路径 + `continue=true` 从 `.aria2` 续传。
8. **会话恢复**：aria2 daemon 启动带 `--save-session`/`--input-file`（
   `aria2_engine.cpp::daemonExtraOpts`），`shutdown()` 先 `aria2.saveSession` 再
   forceShutdown；重启后 `recoverSession()` 用 `tellActive/tellWaiting/tellStopped`
   重建任务表。
9. **缩放**：eui-neo 0.5.6 起 `DslAppConfig::uiScale(kUI)` 原生放大（布局+字号）；
   尺寸按设计逻辑像素直接写，不再 `S()` 自乘。`kUI` 仍是唯一缩放旋钮。
10. **aria2 引擎**：进程名 Windows 是 `aria2-next.exe`，unix 是 `aria2-next`；
    字段名用 `connections`（不是 `numConnections`）。
11. **岛屿卡片布局**：内容区/子侧边栏是浮在背景上的圆角"岛"卡（`widgets::drawPanel`，
    底色 `mixColor(background, surface, 0.5)` 中间色调，圆角 `kIslandRadius`）；左侧
    总侧边栏是整高透明列（不铺底色）。布局常量在 `utils.cppm`：`kIslandGap`（岛间距）、
    `kPanelPad`（大卡内边距）、`kRightMargin`（右缘）。
12. **eui 元素 id 全局唯一**：一个 frame 里同名 id 会互相覆盖（如 `components::text`
    标签与 `buildListPicker(id="x")` 内部的 `x.label` 撞名 → 文字不显示）。新增控件
    的 id 要避开已有前缀。
13. **非阻塞打开**：`openFile` / `openContainingFolder` / `openUrl` 在 Windows 走
    `ShellExecuteW`（`platform.cppm::shellExecFn()`），立即返回；**不要用
    `std::system("explorer …")`**——explorer 会让调用方同步等窗口关闭，卡 UI 线程。
14. **下拉点击外部收起**：`buildListPicker` 展开时铺一层全屏透明拦截层（吞掉点击），
    点击弹层外即收起。弹层宽度可用 `popupWidth` 参数（图标字段的弹层要加宽容纳文字）。
15. **提交**：本地 commit 后由用户自行 push（不要代 push）。
