# TinyNext 规划与决策记录

## 界面布局偏好（2026-10-11）

**用户明确偏好：任务卡片保留竖向进度条。** 下载、暂停、排队、完成、失败、取消及
媒体任务使用同一套卡片布局：进度条和刻度独占左列，自下向上填充并贯穿卡片内容高度；
右列沿用第一版的两行文件名、来源、大号百分比、状态与传输信息。完成填满；
未知总量保持空轨道并显示 `--`，直播显示 `LIVE`。
进度条宽度、高度、文字间距统一由 `ui/utils.cppm` 的 `kCardProgress*` 令牌管理。
页面继续采用统一顶部标签、底部工具栏和黑白扁平视觉，后续精简不改变竖向进度风格。

**操作按钮左对齐，独占右列最后一行。** 不与名称或传输信息并排，不侵入进度列，
不添加操作区分隔线、不向上换行；窄卡片按操作数量收紧间距，保留按钮大小。
沿用第一版的两列内容层次并收紧尺寸：高度 200、卡宽 244–260、内边距 12、
卡片间距 8，确保右列容纳全部操作，按钮顺序与此前一致。

**输入框偏好：MD3 Outlined（描边式）。** 通过 `widgets::outlinedInput` 组合原生
EUI input 的编辑能力与独立外壳：浮动标签 / 顶部描边留口 / 聚焦 2px 描边 / 4px 圆角 /
无投影，沿用黑白主题。常规字段采用 40 逻辑像素的紧凑高度，多行字段加高；标签留在
编辑器 clip 外，留口底色匹配页面或弹窗。设置页、添加下载和镜像地址统一使用；数字
步进框共用外壳。该样式由应用封装，EUI-NEO 保持锁定 0.6.2。

**浮动标签滑动动画（2026-10-15）。** 聚焦/输入时标签从框内滑到顶部留口、失焦且
清空后滑回，0.16 秒 OutCubic（`kOutlinedAnimSeconds`），聚焦变色（主色）与描边
1px↔2px 过渡同节奏。实现走 eui Frame 插值（`.animate(Frame|TextColor)`）：布局
帧静止时与目标恒等，不产生常驻动画状态；字号按姿态原生光栅（静置 11px / 浮动
10px）保证两态都锐利，切换瞬间的字号变化由滑动运动掩护。刻意不用 transform.scale
模拟字号连续缩放（text 挂 transform 走变换绘制路径），留口与占位提示用随姿态
组装/消失的硬切——eui 只缓存「无活跃动画」的子树（`subtreeAnimating` 门控），
任何常驻插值状态都会让所在弹窗放弃缓存逐帧重绘。

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

### 纯无状态 CLI（不依赖 GUI 独立管理下载的完整命令行）（2026-10-11 决策）

**想法**：把 `tinynext` 做成不依赖 GUI 的独立命令行（无头常驻服务 / 全量任务
管理 / 自带配置），GUI 只是可选前端。

**结论：不做，CLI 定位为 GUI 应用的伴生入口。** 理由：

1. **架构上下载状态归 GUI 进程所有**：单实例锁、任务表、引擎 daemon 生命周期、
   会话文件都由主实例持有。CLI 的添加（`tinynext <url>` 应用没开就自动启动
   GUI）与控制面（status/pause/… 只与运行中的实例通信）都围绕这个设计；浏览器
   扩展宿主（2026-10-11 落地）同样把 URL 交给运行中的实例、不在则落 inbox 并
   拉起 GUI。
2. **无状态 CLI = 第二套任务管理语义**：任务表/排序/退出码/会话恢复要在无 GUI
   进程里再实现一遍，与 GUI 长期漂移。`--headless` 已覆盖「脚本跑完一个下载」，
   控制面覆盖「查询/操作运行中实例」，缝隙不大。
3. **既有评估仍然成立**：真要程序化下载，脚本/包管理器直接用 aria2c 更干净
   （见上一条「纯 CLI 模式嵌入其他包管理器下载生态」）。

**边界**：已发布的 `--headless`（自带 daemon、下载完退出）保留不再向常驻无头
服务方向扩展；`agent` 帮助打印保持无状态；其余 CLI 动词维持「依赖运行中的
实例」语义，不做裁剪。

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

### 标题栏关闭事件（close 钩子）不可自定义（2026-08 评估，保持现状）

**结论：放弃，保持 `close_to_tray` 现状**（X = 缩托盘 / 退出二选一）。用户曾想
自定义点 X 后的行为（退出 / 缩托盘 / 最小化 / 先弹确认框），但 eui-neo 0.5.6 没有
可自定义的关闭钩子：

- **限制不在标题栏**：应用用 GLFW 原生窗口 + 系统原生标题栏（非自绘，DWM 深色），
  X 是系统原生按钮（WM_CLOSE → GLFW close 事件）；缺的是**事件处理**扩展点。
- **eui 硬编码关闭逻辑**：`glfw_app_main.cpp:452` 的 `glfwSetWindowCloseCallback`
  写死「有弹窗 → 聚焦弹窗并取消关闭；tray 可用 → 缩托盘；否则退出」。公开 API
  （`app.h` / `dsl_app.h`）无任何 close/exit 钩子；应用侧唯一开关是 `.tray(bool)`
  —— 即 `close_to_tray`，只给「缩托盘 or 退出」两种。
- **要自定义只能非常规手段**：① 首帧 compose 用 `glfwSetWindowCloseCallback`
  （GLFW 返回上一个回调）包一层 eui 原回调、存下并委派回去，可加「最小化」
  （`glfwIconifyWindow`）或「确认框」（关闭回调里不能同步弹 eui 弹窗，需挂起 +
  下次 compose 弹 + 状态机）；② fork 改 `glfw_app_main.cpp`（最重）。
- **何时再考虑**：真需要「X = 最小化」或「X 前确认」时用「包一层回调」（eui-neo 锁
  0.5.6，封装点稳定）；若 eui 日后加关闭钩子/配置项，直接迁移过去。

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

### eui-neo 升 0.5.8（2026-09，已实现；0.5.7 同期带过）

- 上游 0.5.8 新增：**`core/render/image_stream.cpp`**（`eui::ImageStream` 动态纹理——
  CPU 侧像素上传 RGBA8/BGRA8/NV12/I420/P010，GPU YUV→RGB，官方示例是 FFmpeg 视频
  播放器；配方只需把这一个 TU 加进 CORE_SOURCES，无新依赖）；`main()` 拆出
  `eui_app_run()` 库模式入口（`EUI_APP_RUNNER_LIBRARY`，默认 off，对本项目无影响）；
  retained-layer/dirty-rect 渲染性能优化。托盘桥（SNI/gio）、窗口/输入后端与 0.5.7
  **逐字节一致**。
- 对本项目影响为 0：无 API 变动，`mcpp.toml` 升版本 + `gen-versions.sh` 重生成即可，
  全量构建通过，GUI/主题/下载冒烟正常。
- **排查记录（虚惊一场）**：升级后 `busctl --user list | grep -i notifieritem` 看不到
  TinyNext，一度怀疑 xmake-repo 配方「Linux 托盘默认关」同步到了 compat 配方。实际
  SNI 后端就用**唯一连接名**（`:1.653/org/freedesktop/StatusNotifierItem`）向
  watcher 注册，从不占有 `org.kde.StatusNotifierItem-<pid>-N` 这种 well-known 名，
  grep 自然不中。验证托盘要看 watcher 的 `RegisteredStatusNotifierItems` 属性：
  `busctl --user get-property org.kde.StatusNotifierWatcher /StatusNotifierWatcher
  org.kde.StatusNotifierWatcher RegisteredStatusNotifierItems`。
- 仍无应用侧关闭钩子（`WindowState` 私有、close 回调固定在 app-main 内部），
  关窗缩托盘开关的「重启生效」弹窗方案（v0.5.17）保持不变。

### eui-neo 升 0.5.9（2026-09，已实现）

- 上游 0.5.9 是**渲染缓存加固 + 构建系统收敛**：脏矩形合并估算只统计真正省略的
  render pass（避免零散小区域被错误合并成大面积重绘）；OpenGL/Vulkan 清屏时作废
  backdrop 捕获（修背景模糊复用过期数据）；标题栏统计新增文本批 flush/顶点数与
  backdrop 捕获/复用计数；xmake 集成与 `eui_neo.h` 里的 `EUI_APP_RUNNER` 残留
  全部移除，CMake 成为唯一官方构建；外部 CMake 工程用 `eui_neo_configure_app()`
  一行接入。
- 对本项目影响为 0：CORE_SOURCES 与 0.5.8 **逐字节一致**（compat 配方直接编源码，
  xmake 移除无感）；`glfw_app_main.cpp`/`tray_bridge.c`/`platform.cpp` 不变，
  `app_runner.h` 只加了统计字段。全量构建 + GUI/daemon/托盘冒烟通过。
-  backdrop 捕获作废修复对我们相关：设置页/弹窗用 `glassFill` 背景模糊，0.5.8
  及之前在清屏帧可能复用过期模糊底。

### eui-neo 升 0.6.2（2026-10，已实现）

- 上游 0.6.2 聚焦**触摸输入、长内容组件与文本渲染性能**：SDL2 后端单指触摸转
  pointer 事件（`PointerEvent::touch` 区分来源）与像素级滚动数据（`ScrollEvent` 新增
  `pixelX/pixelY`，`x/y` 调用不受影响）；Markdown 仅构建视口附近内容；固定字号字体面
  缓存 glyph advance；字形图集按需分配、仅上传已使用区域（首渲私有内存约 -8 MB）。
  GLFW 后端仍走 OS 触摸→鼠标映射，无原始触摸标记——本项目用 GLFW，触摸相关无行为变化。
- lib 源清单与 0.6.1 一致（文本/markdown 改动均在既有 TU 内），`glfw_app_main.cpp`
  不变；升级 = `mcpp.toml` 改版本 + 全量构建通过。compat 配方包级 `-fno-char8_t`
  原样保留（上游未修）。

### eui-neo 升 0.6.1（2026-10，已实现）

- 上游 0.6.1 是**窗口生命周期 + 组件修复**：DslAppConfig 新增 transparent 透明帧
  缓冲与 onStart()/appId()；beginWindowMove/beginWindowResize 程序化移动/缩放
  （Linux 走 EWMH `_NET_WM_MOVERESIZE` 委托，新头 `x11_moveresize.h` 在运行时
  dlopen libX11，无链接期 X11 依赖）；下拉/滑块 z-order、OpenGL pixel-unpack 修复。
  **CORE_SOURCES 与 0.6.0 逐字节一致**；头文件变化全是增量（dsl_app.h 新增字段/回调），
  `components/progress.h` 与 0.6.0 diff 一致，竖向进度卡方案的前提（progress 只有
  横向填充）仍成立。
- 对本项目影响为 0：无已用 API 变动，`mcpp.toml` 升版本 + `gen-versions.sh` 重生成
  + 全量构建通过。compat 配方的包级 `-fno-char8_t` 原样保留（上游未修）。
- 索引侧（mcpplibs PR #531）带一个包私有 shim：上游 SDL2 窗口分支的
  `sendX11MoveResizeForWindow` 声明在三重宏守卫内而调用点无守卫，macOS/Windows 的
  SDL2 构建必炸；索引在 `mcpp_eui_backends.h` 里按平台补宏/桩。本项目用默认
  GLFW 后端，不经过该分支。

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
- 支持多 URL 逐个任务；接受 http(s)/sftp/ED2K file / magnet: / 本地 .torrent。
- 失败任务保留在会话文件（下次 GUI 启动续传）。daemon 输出重定向到
  `configDir/tinynext-aria2.log`（终端保持干净）。

### MCP server：AI 助手接入（已实现 2026-10-11）

`tinynext --mcp` 是一个 stdio MCP（Model Context Protocol）server，AI 客户端
（Claude Desktop / Claude Code / ZCode…）按 mcpServers 配置拉起它即可管理下载
（9 个工具：get_status / list_downloads / add_download / pause / resume /
cancel / retry / remove / wait_until_idle）。实现为独立模块
`tinynext.mcp_server`（`src/mcp_server.cppm`），`CliBoot` 接管（不进 GUI、
不抢单实例锁），工具调用翻译成控制面动词经 TCP 交运行中的实例；应用未运行时
add 走转发路径（落 inbox + 拉起 GUI 进托盘，同 `tinynext <url>`）。详见
`docs/mcp.md`。

**架构决策：aria2-next 不打包成独立服务，MCP 走伴生进程。**

- 「CLI = GUI 伴生入口」决策（2026-10-11）的自然延伸：AI 助手是又一个客户端，
  下载状态（任务表/seq/失败闸门/会话恢复）继续归 GUI 进程所有。
- 独立服务的代价：daemon 生命周期脱离应用（自启动/系统服务、固定端口+secret
  落盘与随机安全模型冲突）、组件更新「停 daemon 才能换二进制」要重做、
  stop-with-process 孤儿兜底要为服务模式反转。
- 参考实现调研：[Aria2 Agent](https://github.com/Rehui-2006/aria2-agent)（Python，
  8 工具，直连 aria2 RPC，需自补防幻影文件等安全规则）与
  [aria2-mcp-rs](https://github.com/nicholaswilde/aria2-mcp-rs)（Rust，17 工具 +
  HTTP/SSE + RSS/带宽计划，自述 not production-ready）——都绕开应用层，因此要
  重新发明任务语义/沙箱；TinyNext 的 MCP 面向 TaskStore/控制面，语义天然继承。
  吸收了两点：`list_downloads` 的 query 过滤（学 search_downloads）、
  V2 预留 HTTP 传输 + Bearer token（学 aria2-mcp-rs）。
- **再评估触发条件**：出现「无 GUI 常驻下载」的真实需求（NAS/服务器场景）时，
  再评估独立 daemon 模式；届时 MCP 传输可平移。
- V1.5 候选：带参数的 `add` 控制动词（name/dir/connections；ctl token 无引号
  转义需先定编码，dir 必须限下载目录内防路径穿越）、`tinynext mcp register`
  写客户端配置、MCP resources。

## CI 打包：Windows runner 固定 windows-2022（2026-10-01）

**问题**：发 v0.7.12 时 `build-win64` 卡在 «Install NSIS» 一步——curl 从
SourceForge 下 `nsis-3.10.zip` 拿到 **HTTP 522**（整站故障，本地/各镜像端点同样
522），而 `curl --retry` 默认**不重试 522**，一次失败就 throw，Release 直接不发。
`windows-latest` 现在是 `windows-2025-vs2026`，该镜像**不再预装 NSIS**
（2022 镜像预装 3.10），所以只能现下。

**决定**：矩阵里 Windows 固定 **`windows-2022`**（不再用 `windows-latest`）：
NSIS 直接用镜像自带的 3.10（`C:\Program Files (x86)\NSIS\makensis.exe`，与本地
验证版本一致），CI 日志确认下载步骤被跳过；下载路径保留为兜底，并加上
`--retry-all-errors`（这次要的就是它）+ 三个 SourceForge 端点轮询 + 体积校验。

**代价 / 后续**：windows-2022 是 VS2022 镜像（比 `windows-latest` 的 VS2026 旧，
对 mcpp 自带工具链无影响，实测三平台打包全绿）。等 GitHub 宣布 2022 退役、
或 2025 镜像重新预装 NSIS 时，改回 `windows-latest` 即可，兜底下载路径已经能扛住
SourceForge 的间歇故障。

## 待办（未完成，下次处理）

2026-10-01 复查 v0.7.11「稳定元素 id + 媒体轨道选择」那次提交后清理出的小尾巴，
代码修复已随 **0.7.12** 发布。**以下三条已于 2026-10-11 全部处理完毕**（拍板记录
见各条目内）：1）`.agents/` 已入库；2）两个选择弹窗的快照失效检查已实现；3）
`quit` 托盘待机时 marshal 超时后自动硬退出（见上「已拍板」条目）。

### 1. 项目 UI skill 纳入版本管理（已完成）

2026-10-26 指南瘦身后，当前入口为
[project-skills/tinynext-eui-ui/SKILL.md](../project-skills/tinynext-eui-ui/SKILL.md)，
长细节放在该技能的 `references/`。以下保留当时 `.agents/` 入库问题的背景。

仓库里 `AGENTS.md` / `CLAUDE.md` 都是跟踪的，但给 AI 用的 UI 技能目录
`.agents/` 仍是 `git status` 里的唯一 `??`。内容是项目自己的 EUI 纪律
（元素 id 唯一性 / 稳定 key / 测量回调用传入的 `Ui&` 等），不提交等于只在本机生效。

**要拍板**：入库（推荐，和 AGENTS.md 一起走 code review）还是保持本地私有
（那就在 `.gitignore` 里显式忽略，免得每次 `git status` 都挂着）。

### 2. 媒体轨道选择弹窗的快照不会随任务状态失效

`g_pendingMediaTrackSelection`（`src/store/dialogs.cppm`）只在「取消 / 提交 / 点遮罩」
时清空。若弹窗开着时任务自己离开「等待选择」状态（例如引擎重连后重新探测、
或用户在 CLI 把任务移除了），快照仍是旧的：提交时引擎会拒绝并回显错误，
不会写坏状态，但提示不够准确。

**要拍板**：接受现状（当前判断：可接受，弹窗本就是短交互），还是在状态刷新时
顺手失效（比如 `requestMediaTrackSelection` 之外，给 refresh 回调加一句
「任务已不在等待选择 → reset」）。

### 3. `close_to_tray = true` 时 `quit` 的行为语义

`tinynext quit` 走的是「关窗」这条路径，配置里开着 `close_to_tray` 时它只是把窗口
缩进托盘：进程继续存活，而此后 marshal 到 UI 线程的任务操作
（`pause` / `resume` / `cancel` / `retry` / `remove` / `clear` / `quit`）会 10s 超时，
只读的 `status` / `list` / `watch` 仍能在 IPC 线程作答。`tinynext agent` 帮助里已经
写明「任务操作只在 UI 循环活着时执行」，所以不是 bug，但对脚本来说很容易踩。

**要拍板**：`quit` 是否应该强制退出（绕过托盘）？备选：① 保持现状，只在文档里
强调；② `quit` 加 `--force`（或 `quit --force`）走真正的退出路径；③ `quit` 直接
强制退出，想缩托盘的用户点窗口的 X —— ③ 改动最小但要改 agent 帮助文案。

**已拍板（2026-10-11，方案：marshal 超时后自动硬退出）**：`quit` 在 UI 线程
10s 未应答（托盘待机态）时不再报错，改为在监听线程先回
`ok: TinyNext is force quitting…`（写进内核缓冲，客户端必达），再执行
`g_tasks.shutdown()`（saveSession + 杀 daemon，线程安全、幂等）并
`_exit(0)`/`ExitProcess(0)` 立即结束进程——刻意跳过 atexit/静态析构，避免
「主循环还活着却开始拆全局」的竞态；X11/Wayland/GLFW 状态由服务端与合成器
回收，Windows 托盘图标残留到鼠标悬停（外观代价）。窗口可见时仍走 UI 线程的
优雅路径（托盘图标正常清理）。「add → watch --until-idle → quit」脚本流程在
托盘模式下闭环。实现：`cli.cppm` 的 `runControl` 超时分支 +
`g_forceQuitAfterResponse` 标志 + `forceQuitNow()`（响应由监听循环发送后再
触发硬退出，保证先应答后退出）。

另外顺带记录：排查过程中发现「用 `pkill -x tinynext` 杀不掉 `run.sh` 启动的进程」
——进程名是 `ld-linux-x86-64.so.2`（加载器），要按 PID 或
`pkill -f 'ld-linux.*tinynext'` 杀。
