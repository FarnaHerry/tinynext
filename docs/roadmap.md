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

### eui-neo 版本声明与 C++23 构建兼容性（2026-08 发现）

- `mcpp.toml` 曾声明 `eui-neo = 0.5.5`，但 mcpp 包索引一度没有 0.5.5，实际装的是
  **0.5.3**；锁文件却记录 0.5.5，导致 `mcpp build` 拉包时报 `E_NOT_FOUND`。
- `xlings update` 刷新索引后有 0.5.5，但 **0.5.5 在本项目 C++23（llvm 22.1.8）下
  编译不过**：`core/render/image_source.cpp`、`core/platform/platform.cpp` 用
  `path::u8string()`（C++20 起返回 `char8_t`）赋给 `std::string` 报错，另有
  `lowerCopy` 匹配失败。
- 结论：**已把 mcpp.toml 降到 0.5.3**（实际验证可用的版本），README / about
  版本号同步。升 0.5.5 需先给 eui-neo 打补丁（见上条）。

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

### `tinynext --headless <url>` 脚本模式

不开窗、按 TinyNext 自身配置（下载目录 / 连接数 / 引擎）下载完退出，`exit 0/1`。
适合「已用 TinyNext 的人」写脚本 / 定时任务。复用 `aria2_engine` daemon + JSON-RPC，
约 100~150 行，不依赖 eui。与现有 CLI + 单实例机制一致。

- 定位：**TinyNext 用户的脚本工具**，不是给其他包管理器的下载后端。
- 优先级：低。
