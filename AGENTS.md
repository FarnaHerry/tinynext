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
- eui-neo 锁在 **0.6.0**（配方加 `-fno-char8_t` 修 C++23 构建 + 补 `-ldwmapi`，见 `docs/roadmap.md`），不要乱升。
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
| `tinynext.ui.*` | `src/ui/*.cppm` | utils（布局常量）/ theme / platform / housekeep / widgets / cards / downloads_page / settings_page / about_dialog |
| `src/app.cpp` | 普通 TU | 入口：`app::dslAppConfig()` + `app::compose()` |

页面已按职责拆成独立模块（`pages.cppm` 已删除）：
`downloads_page`（下载页 + 添加下载弹窗）、`settings_page`（设置页）、
`about_dialog`（关于弹窗）。

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
11. **Material 3 视觉体系**：界面是 MD3 风格——tonal 表面分层（无描边无玻璃拟态，
    `glassFill` 已退役）、pill/大圆角、state layer 交互反馈。色板全部在
    `ui/theme.cppm` 的 `AppTheme`：MD3 角色（primary/onPrimary/primaryContainer/
    surfaceContainer{Low..Highest}/outline/outlineVariant/inverseSurface/scrim…）+
    语义容器层 **`panelBg`（岛卡底）/ `cardBg`（内容卡底）**——岛卡必须比窗口背景
    「抬」一层（暗色=surfaceContainerLow，亮色=surfaceContainerLowest≈白，亮色
    直接用 surfaceContainerLow 反而比背景暗）。交互叠加色用 `stateLayer(onColor,
    α)`（hover 0.08 / pressed 0.12）。弹窗统一 M3 Basic Dialog：scrim 遮罩 +
    surfaceContainerHigh + `kDialogRadius`(20) + 投影，**不加 blur**；次要按钮用
    `widgets::drawTextButton`（透明底 + state layer）。岛卡 =
    `widgets::drawPanel`（panelBg + `kIslandRadius`）；左侧是 MD3 Navigation Rail
    （图标+标签 pill 指示，`kRailWidth`=64）。布局常量在 `utils.cppm`：
    `kIslandGap`、`kPanelPad`、`kRightMargin`、`kCardRadius`、`kChipRadius` 等。
12. **eui 元素 id 全局唯一**：一个 frame 里同名 id 会互相覆盖（如 `components::text`
    标签与 `buildListPicker(id="x")` 内部的 `x.label` 撞名 → 文字不显示）。新增控件
    的 id 要避开已有前缀。
13. **非阻塞打开**：`openFile` / `openContainingFolder` / `openUrl` 在 Windows 走
    `ShellExecuteW`（`platform.cppm::shellExecFn()`），立即返回；**不要用
    `std::system("explorer …")`**——explorer 会让调用方同步等窗口关闭，卡 UI 线程。
14. **下拉点击外部收起**：`buildListPicker` 展开时铺一层全屏透明拦截层（吞掉点击），
    点击弹层外即收起。弹层宽度可用 `popupWidth` 参数（图标字段的弹层要加宽容纳文字）。
15. **提交**：feature 分支本地 commit，release build 全绿后由助手直接
    merge 到 main 并 push（含发布 `v*` tag）。「用户自行 push」的旧惯例已于
    2026-09 作废。
16. **资源一律 RAII 包裹**（全项目强制）：fd / socket / Windows HANDLE / 管道 /
    进程句柄 / CoTaskMem / LocalFree 等原生资源，**获取点即交给所有者**——
    析构即释放的守卫（`aria2_engine.cpp` 的 `LocalSocket`、`cli.cppm` 的
    `WsSession`、`std::unique_ptr<T, Deleter>`）或封装好的辅助（`runCapture`、
    `spawnDaemon` 内建配平），不要手写「多条 return 路径各自 close」的配平——
    少一条路径就是泄漏（审计已修过 theme_watch 的 `return` 跳清理、cli 的
    WSAStartup 无 Cleanup 这类实例）。新增代码检查点：每个 `Create*/socket/
    open/spawn` 是否有对应的析构释放；提前退出（break/return/异常）是否仍释放。
17. **线程也是资源**：常驻后台线程必须有所有者负责「唤醒退出 + join」——
    `cmdThread_`（shutdown 置标志 + join）、`g_listenerThread`（atexit shutdown
    socket 唤醒 + join）、`housekeep::g_thread`、`theme_watch` 的
    `jthread + stop_callback`。`detach()` 只允许用于**一次性、引用生命周期到
    进程末尾的对象**（全局单例 / atomic / 已加锁的 store），并在注释写明依据；
    禁止 detach 线程捕获可能先亡的局部对象。
