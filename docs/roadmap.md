# TinyNext 规划与决策记录

## 已评估 · 放弃

### 纯 CLI 模式嵌入其他包管理器下载生态（2026-08 评估）

**想法**：做一个 headless 的 `tinynext download <url>`（不开 GUI、只启动内部
aria2），让 mcpp / xlings 等其他包管理器「直接复用 TinyNext 作为下载器」。

**结论：不推荐，放弃。** 理由：

1. **aria2 本身就是 CLI**：`aria2c <url> -x 64 -o …` 是设计给脚本调用、久经
   考验的下载器。包管理器直接 spawn aria2c，比调 `tinynext download` 少一层
   进程边界 + stdout/退出码协议约定。
2. **复用价值很薄**：TinyNext 相对裸 aria2 只多了「装对二进制 + 高层接口」。
   包管理器需要的是自己可控的下载栈（错误处理、断点续传、进度回调、限速），
   它们基本已有 curl/wget/aria2。
3. **重量与许可**：依赖 TinyNext = 拖进整个 GUI 应用 + GPLv2 的 aria2-next
   二进制，还绑死 `engines/` 路径约定。包管理器自己维护 aria2 更干净。
4. **headless 的坑**：aria2 进程生命周期、并发调用、退出码语义、GUI 应用
   静默启动的边界都要额外定义。

**若日后需要**：给想用 aria2 的包管理器指路到
<https://github.com/AnInsomniacy/aria2-next/releases>，而不是复用 TinyNext。

### 自定义标题栏（无边框 + 自绘）（2026-08 评估）

**结论：放弃（本期）。** EUI-NEO（实际安装 0.5.3）不支持无边框/自绘标题栏：

- `DslAppConfig`（`include/eui/dsl_app.h`）和 `core/window/window_types.h` 的
  `WindowCreateRequest` 都没有 `decorated` 字段；`createWindow()`（
  `core/window/window_backend.cpp`）从未调用 `glfwWindowHint(GLFW_DECORATED, GLFW_FALSE)`。
- 没有公开的窗口移动 / 最小化 / 最大化 / 关闭运行时 API（`glfw_app_main.cpp` 里的
  `glfwSetWindowPos`/`glfwHideWindow`/`glfwRestoreWindow` 都是内部局部调用）。
- 鼠标事件（`onDrag`/`onPress`/`onRelease` + `PointerEvent`）框架有，但缺
  "把拖动位移转成窗口位移"的平台 API。
- 要支持需 fork/patch eui-neo 框架：改 `window_types.h`、`window_backend.h/.cpp`、
  `dsl_app.h`、`glfw_app_main.cpp` 约 4-6 个文件，并把补丁维护成本带到每次版本升级。

**若日后要做**：以本地 mcpp 包形式 vendoring 一份打过补丁的 eui-neo（加
`decorated` 配置 + `setWindowPos` / 最小化 / 最大化 API），再自绘标题栏 + 拖动区 +
最小化/最大化/关闭三个按钮。

### eui-neo 版本声明与 C++23 构建兼容性（2026-08 发现，已解决）

- `mcpp.toml` 曾声明 `eui-neo = 0.5.5`，但 mcpp 包索引一度没有 0.5.5，实际装的是
  **0.5.3**；锁文件却记录 0.5.5，导致 `mcpp build` 拉包时报 `E_NOT_FOUND`。
- `xlings update` 刷新索引后有 0.5.5，但 **0.5.5 在本项目 C++23（llvm 22.1.8）下
  编译不过**：`core/render/image_source.cpp`、`core/platform/platform.cpp` 用
  `path::u8string()`（C++20 起返回 `char8_t`）赋给 `std::string` 报错，另有
  `lowerCopy` 匹配失败。
- **解决（2026-08）**：compat.eui-neo 0.5.5 配方加**包级 `-fno-char8_t`**——禁掉
  `char8_t` 后 `u8string()` 在所有平台返回 `std::string`，编译通过；`mcpp.toml` 升回
  **0.5.5**，并补 `-ldwmapi`（0.5.5 的 DWM 标题栏深色用）。0.5.3 的 pin 已解除。

### eui-neo 升 0.5.6（2026-08，已实现）

- 索引侧：`compat.eui-neo` 0.5.6 已由 mcpplibs `Add eui neo 0.5.6 (#203)` 收录（sha256
  `0df8d798…`）。上游 0.5.6（32 commits / 86 files）相对 0.5.5 对配方的影响只有一处：
  **CORE_SOURCES 新增 `core/window/window_input_backend.cpp`**（输入/IME 事件泵从
  `window_backend.cpp` 拆出，两后端均无新依赖），其余 source 列表、`3rd/` vendored
  依赖字节级不变。
- **`-fno-char8_t` 保留**：`path::u8string()` 赋 `std::string` 的问题 0.5.6 上游未修，
  包级 flag 继续生效；`-ldwmapi` 也保留（glfw win32 深色标题栏，vendored glfw 未变）。
- **API 变化（对本项目影响为 0）**：0.5.6 起 `eui_neo.h` 不再包含
  `eui/detail/dsl_app_impl.h`——`app::update/render/initialize` 等入口机制挪进
  `app-main` 特性的 `glfw_app_main.cpp` 内部编译（该 TU 自己 include
  `dsl_app_impl.h`）。`src/app.cpp` 仍只需 `#include <eui_neo.h>` 取
  `DslAppConfig`/`app::compose` 声明即可；UI 模块的精简头 `eui_ui.h` 历史存在的
  mangled name 规避动机随之消失，但保留以减少 include 面。
- `mcpp.toml` 升 `eui-neo = "0.5.6"`（仍带 `app-main` feature，不能去掉——没有它
  就没有 `main()`），`mcpp.lock` 重新解析。Windows 本地构建 + `tinynext agent`
  冒烟通过。

### 根 onFrame 导致空闲 90 FPS 全量重绘（2026-08 发现并修复）

- **症状**：挂机时 GPU 占用持续跳动（标题栏 `90 FPS / GPU 9% / Full 100%`）。
- **根因**：`app.cpp` 根 stack 挂了 `.onFrame`。eui 的 `Runtime::updateFrameCallback`
  （`core/runtime/runtime_update.h`）对任何挂 onFrame 的元素**每帧无条件**
  `composeRequested_ = true; paintRequested_ = true; animating_ = true`——onFrame 是
  eui 给「每帧都在动」的东西用的钩子，被当周期轮询钩子用，eui 就以为 UI 永远在动画：
  每帧重建 DSL 树 + 全量重绘 + `glfwPollEvents` busy-loop，永不睡眠。回调内容（inbox
  轮询 / 通知检测）本身很便宜，**换成空回调也照样满帧**。
- **修复**：去掉根 onFrame，周期/事件工作全部挪到后台线程，只在真有事时
  `core::platform::requestUiUpdate()` 唤醒 UI 一帧（跨线程安全，eui network 线程同款）：
  - `cli::startCliIpc()` — TCP loopback 后台监听线程**阻塞在 accept 上**（队列空就挂起），
    第二实例转发 URL 改为 socket 直连（回退写 inbox 文件），零轮询零延迟；
  - `housekeep::startHousekeeping()` — 每 500ms 查状态消息过期（原子墙钟）/
    下载通知迁移 / 活动任务进度，有变化才唤醒；
  - `theme_watch` — 主题变化时直接唤醒（`markThemeDirty`）；
  - `aria2_engine::handleWsEvent` — 状态迁移后唤醒（完成/失败即时可见）。
- **结果**：空闲 `0 FPS / CPU 0% / GPU 0%`（8s CPU 增量 ~0.05s），有活动下载时才
  以 ~2fps 刷新进度。CLI 转发、完成/失败通知、状态消息 4s 消失均回归通过。
- **UI 缩放改用 eui 原生 `uiScale`（0.5.6 新增）**：`DslAppConfig::uiScale(kUI)`
  按 `dpiScale × uiScale` 放大整个逻辑坐标系（含字号，`toPixels = v*dpiScale`，
  app 把 `effectiveScale` 传入 runtime）。删除了 `utils::S()`（276 处调用改回设计
  值），`kUI` 保留为唯一缩放旋钮。关键点：**窗口物理尺寸 = 设计尺寸 × kUI**
  （`windowSize(1120*kUI, 720*kUI)`）——GLFW 按 DIP 建窗，eui 不会自动乘 uiScale。
  渲染输出与 0.5.5 像素级一致（100% 与 150% DPI 均验证）。

### 下载优先级功能：aria2-next 不支持（2026-08 核实，已移除）

- 添加弹窗曾提供「优先级（默认/高/中/低）」选择器 + 任务排序「优先级」，把
  `priority` 作为 addUri 选项传给 aria2。
- **实测 + `aria2-next --help=#all` 确认：aria2-next 没有下载级 `priority` 选项**
  （只有 `--bt-prioritize-piece`，是 BT 分片选择）。三次排队实测全是 FIFO——`priority`
  参数被静默忽略，所有任务等权。
- 结论：**优先级是伪需求**（想先下就暂停其他任务，app 本就支持），选择器 / 排序 /
  `priorityValueFromPicker` / `StartOptions.priority` 已一并移除。若 aria2-next 日后
  补回该选项，再按「高数值 = 高优先」恢复 UI 即可。

### 代理：aria2 不支持 SOCKS5（2026-08 核实）

- aria2 家族（含 aria2-next）的 `--all-proxy` / `--http-proxy` / `--https-proxy`
  只支持 HTTP 代理协议；`socks5://` 会报 "unrecognized proxy format"。
- 设置页代理字段因此只做 HTTP/HTTPS 代理地址 + 无代理列表，不提供 SOCKS5。

### 移除 tinyhttps 内置引擎，纯 aria2-next（2026-08 决策）

**决定：移除。** 内置 `TinyHttpsEngine`（`tinynext.download_manager` 模块）删除，
aria2-next 是唯一下载引擎。理由：aria2 已覆盖全部需求（分片多连接、断点续传、
磁力/BT、重试、限速、代理），保留内置引擎是重复维护。

连带改动：

- aria2 本地 JSON-RPC 传输改用自写的极简跨平台 `LocalSocket`（`aria2_engine.cpp`，
  winsock / POSIX），`mcpp.toml` 去掉 `tinyhttps` 依赖。
- 设置页去掉「下载引擎」切换；`config.cppm` 去掉 `EngineChoice` / `engine()`。
- `http://` 不再强制升级为 `https://`（那是 tinyhttps 只支持 HTTPS 的限制）。

## 后续方案（未排期）

### aria2 RPC WebSocket 事件推送（已实现，2026-08）

**`compat:websocket` 包已落地**（IXWebSocket 包装，v12.0.1，client-only、零依赖），
TinyNext 已接入，状态迁移 / 完成通知变即时。

**对 aria2-next 源码核实的关键事实**：

- **aria2-next 没有 `--enable-rpc-websocket` flag**（prefs.h / OptionHandlerFactory.cc
  均无）。WebSocket 是 HTTP 层检测 `Connection: upgrade` 后自动升级（HttpServer 的
  `feedUpgradeResponse` 路径），daemon 参数**无需改动**，直接连
  `ws://127.0.0.1:<port>/jsonrpc`（同一 RPC 端口）。
- WS 推送事件只有 6 个：`onDownloadStart / onDownloadPause / onDownloadStop /
  onDownloadComplete / onDownloadError / onBtDownloadComplete`，参数只带 gid，
  **没有 `onDownloadProgress`**（本条目早先的 `onDownloadProgress` 是误解）。
- WS 上 JSON-RPC 走同一套 `processJsonRpcRequest`，rpc-secret 仍作 params[0] 的 token。

**接入方案（用户拍板：混合）**：WS 连接只收 6 个推送事件，请求-响应继续走现有 HTTP
`rpcCall`（已验证、可回退）。状态迁移由事件即时驱动，进度轮询从 200ms 降到 1s
（对齐 Motrix / AriaNg）。引擎加 `tasksMutex_`（单一互斥量，锁纪律：持锁方法
snapshot/start/action/recoverSession/handleWsEvent 等，ensureDaemon/applyTellStatus
不持锁；start/retry 先调 ensureDaemon 再取锁，避免非递归互斥量死锁）。WS 掉线自动
回退轮询（状态迁移延迟 ≤1s）。

**参考实现结论**：MotrixNext（Rust，与 aria2-next 同作者）**纯 HTTP 无 WS**；
Motrix 经典版 `aria2` npm 包「WS 打开走 WS、否则 HTTP」。故混合 + HTTP 回退是稳妥路径。

若日后要升级「全量 WS」（Motrix 经典模式）：WsNotifier 已具备连接层，把 `rpcCall`
换成 WS 发送 + id 匹配 + 条件变量即可，锁纪律同步收紧（持锁期间不能等响应）。

### `tinynext --headless <url>` 脚本模式（已实现，2026-08）

不开窗、按 TinyNext 自身配置（下载目录 / 连接数 / 引擎）下载完退出，`exit 0/1`。
适合「已用 TinyNext 的人」写脚本 / 定时任务。复用 `aria2_engine` daemon + JSON-RPC，
不依赖 eui。实现为独立模块 `tinynext.headless`（`src/headless.cppm`），由
`cli::CliBoot` 在 main 之前接管（不抢单实例锁、不进 GUI、不转发 URL）。

- 定位：**TinyNext 用户的脚本工具**，不是给其他包管理器的下载后端。
- 支持多 URL 逐个任务；接受 http(s)/ftp(s)/sftp / magnet: / 本地 .torrent。
- 失败任务保留在会话文件（下次 GUI 启动续传）。daemon 输出重定向到
  `configDir/tinynext-aria2.log`（终端保持干净）。
