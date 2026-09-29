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
| `tinynext.cli` | `src/cli.cppm` | 单实例 + 命令行 URL + TCP socket 转发 + 控制面请求入口 |
| `tinynext.cli_control` | `src/cli_control.cppm` | CLI 控制面：`status/list/watch`（只读，IPC 线程直答）+ `pause/resume/cancel/retry/remove/clear/quit`（marshal 到 UI 线程执行），支持 `--json` |
| `tinynext.ui.*` | `src/ui/*.cppm` | utils（布局常量）/ theme / platform / housekeep / widgets / cards / downloads_page / settings_page |
| `src/app.cpp` | 普通 TU | 入口：`app::dslAppConfig()` + `app::compose()` |

页面已按职责拆成独立模块（`pages.cppm` 已删除）：
`downloads_page`（下载页 + 添加下载弹窗）、`settings_page`（设置页）、
设置页包含关于信息标签页。

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
11. **黑白极客视觉体系**：界面是纯中性灰 monochrome 风（对标 Codex / Claude Code
    页面）——亮暗双主题严格镜像，**主色 = 反白**（暗主题 primary=白/onPrimary=黑，
    亮主题反转）。层次靠 **1px hairline 描边**（`kHairline` + `theme.outline`）+
    灰阶梯（surfaceContainerLow..Highest）表达，**扁平无投影**——唯一例外是弹窗
    （`shadow(16,4)`：dark 黑 α0.40 / light 黑 α0.12）。色板全部在
    `ui/theme.cppm` 的 `AppTheme`（字段名沿用 M3 角色名，语义已是单色体系）+
    语义容器层 **`panelBg`（岛卡底）/ `cardBg`（内容卡底）**。状态色只保留哑化
    绿/红（done/failed），活动态用单色灰。交互叠加色用 `stateLayer(onColor, α)`
    （hover 0.08 / pressed 0.12）。弹窗统一规范：scrim α0.5 遮罩 +
    surfaceContainerLow + hairline + `kDialogRadius`(8) + 那套弹窗投影；次要按钮用
    `widgets::drawTextButton`。岛卡 = `widgets::drawPanel`（panelBg + hairline +
    `kIslandRadius`=8）；圆角令牌 `kCardRadius`=6 / `kChipRadius`=4 /
    `kButtonRadius`=6（pill 已退役）。**等宽字体点缀**：`kMonoFont`
    （JetBrains Mono，assets/ 内置，OFL）用于数字/速度/页码/版本号等纯拉丁片段，
    CJK 靠 eui 字体栈回退（`fontFamily` 带 `.` 按项目资产路径加载）。
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
16. **原生资源一律用 RAII 管理**（全项目强制）：获取资源后立刻交给 move-only
    所有者，不要把裸句柄留到函数末尾再手工清理，也不要给多个 `return` 分支
    分别配平。通用包装在 `src/native_resource.hpp`，按资源使用正确的释放 API：
    - Windows `HANDLE` → `native::UniqueHandle` / `CloseHandle`；模块 →
      `UniqueModule` / `FreeLibrary`；`LocalAlloc` → `UniqueLocalAlloc` /
      `LocalFree`；`CoTaskMem` → `UniqueCoTaskMem` / 对应的 `CoTaskMemFree`；
      自有 `HICON` → `UniqueIcon` / `DestroyIcon`。共享系统图标（如
      `LoadIconW(nullptr, ...)`）是借用资源，不要交给 `UniqueIcon`。
    - POSIX fd → `UniqueFd` / `close`；`popen` → `UniqueFile` / `pclose`；
      socket 用具备析构关闭的 socket owner（如 `LocalSocket`、`CliSocket`）。
      `WSAStartup`、IXWebSocket 网络初始化等成对 API 也要有 session owner，
      确保成功初始化才执行对应 cleanup。
    - 子进程 owner 除释放 Windows 进程句柄外，还要负责需要退出的子进程
      terminate + wait；POSIX 子进程要 wait/reap，不能只丢弃 PID。未完成启动、
      超时、异常及重复 shutdown 都必须安全收敛。`CreateProcessW` 用显式继承句柄
      列表；POSIX fd 默认设 `FD_CLOEXEC`，只把标准输入/输出等明确需要的 fd
      通过 spawn actions 交给子进程。
    - 已有 owner 可直接复用；新增类型也遵循 delete-copy、支持 move、析构释放。
      `.get()` 只借用，所有权转交必须使用 `.release()` 或 move，并在接收处立刻
      建立新 owner。借用句柄不要重复关闭。释放函数要匹配资源来源，不能用
      `CloseHandle` 释放图标、socket 或 `LocalFree` 内存。
    - 在资源创建点检查：每个 `Create*` / `LoadLibrary` / `socket` / `open` /
      `pipe` / `popen` / `spawn` 是否立即进入 owner；每条提前退出、异常路径和
      子进程继承路径是否仍正确释放。文件动作列表等需要显式 `destroy` 的 C API
      也应使用局部 guard。
17. **线程也是资源**：每个后台线程都要有负责停止和 join 的 owner。优先用
    `std::jthread` + `stop_token`；使用 `std::thread` 时由模块/对象保存并 join，
    阻塞线程必须有可关闭的 socket、stop pipe、条件变量或消息队列来唤醒。退出时
    按依赖顺序先停止并 join 访问某对象的 worker，再销毁该对象或它依赖的 DLL/引擎；
    `atexit` 回调按后注册先执行，注册顺序必须和资源依赖相符。禁止 detach 捕获
    局部对象、模块静态对象或仍可能析构的单例。新代码默认禁止 `detach()`；确有
    进程末尾的一次性工作需要例外时，必须先证明所有捕获对象和被调用模块在 join
    前仍存活，并在代码注释说明依据，不能仅以“全局单例”作为生命周期证明。
