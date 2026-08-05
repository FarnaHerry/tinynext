# TinyNext 下载器

一个用 C++23 编写的**跨平台** GUI 下载器：**EUI-NEO** 前端 + 可切换下载引擎
（**tinyhttps** 内置 / **aria2-next** 外部进程，支持分片多连接），支持
Windows / Linux / macOS，全部通过 mcpp 包管理。

## 构建与运行

```bash
mcpp build          # 编译
mcpp run            # 启动 GUI 窗口
```

下载的文件默认保存在你的系统下载目录（可在设置页修改保存路径）。

## 界面

- **左侧图标栏**：纯图标导航（下载列表 / 设置），底部按钮切换深浅主题；左上角是应用 logo（项目名缩写）。
- **下载状态子侧边栏**：下载页内容区左侧的 **所有 / 下载中 / 已完成** 筛选。
- **任务卡片**：每个任务一张卡片，纵向排布文件名+状态标签、进度条、信息（百分比/速度/大小）+ 操作图标。
- **添加下载弹窗**：右上角 **➕** 打开，输入链接后「提交」/「取消」。
- **翻页行**：◀ 第 X / Y 页 ▶，右侧选择每页条数（5/10/20/50/100）。

## 使用

1. 点击右上角 **➕** 打开「添加下载」弹窗，粘贴 **HTTPS** 链接（`http://` 会自动升级为 `https://`，其他协议会被拒绝——tinyhttps 只支持 HTTPS），点「提交」或按回车开始。
2. 卡片操作全部用图标，无文字：
   - **复制链接**、**删除**：所有任务都有；
   - 下载中：**暂停** / **取消**；已暂停：**继续** / **取消**；
   - 已完成：**打开** / **打开所在文件夹**。
3. 同名文件自动加 ` (1)`、` (2)` 后缀，不会互相覆盖。

## 暂停/继续（实现说明）

暂停是**线程内阻塞**实现的，不是断点续传：

- worker 在 tinyhttps 每次读块的边界（`isCancelled` 回调）处按条件变量停车，暂停时**连接保持打开、不读字节**，继续时原地恢复。
- 因此暂停是即时的、恢复是无损的；下载进度不会倒退。
- **局限**：暂停期间连接仍占着；若服务器空闲超时（多数 60~75s）断开了连接，继续后读取会失败、任务进入「失败」。长时间暂停有风险，短暂停完全正常。
- 若关闭程序时仍有暂停任务，会自动取消并立即回收（shutdown 不会挂起）。
- tinyhttps 不支持 HTTP Range/分片，真正跨进程的断点续传需要扩展它（后续可做）。

## 设置（⚙ 设置页）

- **主题**：跟随系统 / 深色 / 浅色（跟随系统时 ~2s 轮询 OS 主题，自动切换）。
- **下载引擎**：`tinyhttps`（内置，零依赖）/ `aria2-next`（外部进程，分片多连接）。
  切换后点「保存」立即生效（有进行中任务时需重启）。
- **下载路径**：默认系统下载目录（Windows `%USERPROFILE%\Downloads` / macOS
  `$HOME/Downloads` / Linux `XDG_DOWNLOAD_DIR`），可「浏览」用系统选择器或手输。
- **aria2 参数**（仅 aria2-next）：分片数、每服务器连接（默认 64，上限 64）、
  最小分片（≥1M）、每任务限速（KB/s，0=不限）。新下载立即生效。
- 所有设置点「保存」落盘到 `tinynext.conf`（JSON），「放弃」回滚；左侧栏底部 ⓘ
  打开「关于」弹窗（含项目 GitHub 链接）。

## 下载引擎

UI 只面向抽象 `dl::DownloadEngine` 接口（`src/download_engine.cppm`），两个实现：

| 引擎 | 特点 |
|------|------|
| `TinyHttpsEngine` | 内置 tinyhttps，单连接顺序下载，零外部依赖，暂停是线程内停车 |
| `Aria2Engine` | spawn `engines/aria2-next` 守护进程，JSON-RPC 驱动，`-x 64 -s 64` 分片多连接 + 断点续传（`.aria2` 控制文件） |

## 跨平台与 aria2-next 引擎二进制

三平台都需把对应的 aria2-next 二进制放进 `engines/`（已 gitignore，`checksums.sha256` 保留）：

| 平台 | release 资产（v2.5.5） | 放置为 |
|------|------------------------|--------|
| Windows x64 | `aria2-next-2.5.5-windows-x86_64.exe` | `engines/aria2-next.exe` |
| Linux x64 | `aria2-next-2.5.5-linux-x86_64` | `engines/aria2-next` |
| macOS (Apple Silicon) | `aria2-next-2.5.5-macos-arm64` | `engines/aria2-next` |
| macOS (Intel) | `aria2-next-2.5.5-macos-x86_64` | `engines/aria2-next` |

下载页：https://github.com/AnInsomniacy/aria2-next/releases

平台验证步骤：
1. 各平台 `mcpp build`。Windows 自动加 GUI 子系统标志；Linux 用 `run.sh` 启动
   （规避 mcpp 私有 glibc 与系统 Mesa 的 GLIBC 版本冲突）；macOS 直接 `mcpp run`。
2. 设置页切到 **aria2-next** → 保存 → 添加一个大文件（≥128MB 才能用满 64 连接）。
3. 文件夹选择器依赖：Linux 需 `zenity`（无则回退 `kdialog`，都没有则手输路径）；
   macOS 用 `osascript`；Windows 系统自带。
4. 主题跟随系统：Windows 读注册表、macOS 读 `AppleInterfaceStyle`、Linux 读 gtk
   `settings.ini`（均 best-effort）。

## 技术栈

| 组件 | 包 | 版本 |
|------|-----|------|
| UI 框架 | `compat:eui-neo` | 0.5.3（feature: `app-main`） |
| 下载引擎（内置） | `mcpplibs:tinyhttps` | 0.2.9 |
| 下载引擎（可选） | `aria2-next`（外部进程） | 2.5.5 |
| 配置 JSON | `nlohmann:json` | 3.12.0 |

### 架构

- `src/app.cpp` — EUI 应用入口。启用 `app-main` 特性后，`main()` 由包内的
  GLFW 入口（`core/app/glfw_app_main.cpp`）提供，本项目只定义
  `app::dslAppConfig()` 和 `app::compose()`（**因此任何 TU 都不能再定义 `main()`**）。
- `src/download_manager.cppm` — 模块接口 `export module tinynext.download_manager;`。
  下载管理器线程安全，每个任务独立 `std::thread` + 独立 `HttpClient`（该库非
  线程安全），进度写入互斥保护区，UI 线程通过 `snapshot()` 每帧读取。
- `src/download_manager.cpp` — 模块实现单元（`module tinynext.download_manager;`），
  唯一 import 了 `mcpplibs.tinyhttps` 的 TU。

模块化程度：`import std` + `import mcpplibs.tinyhttps` + `import tinynext.download_manager`
全模块；**唯一的 `#include` 是 `<eui_neo.h>`**（EUI-NEO 是 header-only C++17 库，
上游没有模块接口，`import eui;` 是 compat 计划里的独立工程）。
- `assets/` — EUI 默认中文字体（JingNanJunJunTi）+ 图标字体，运行时按
  `exeDir/assets/` 或 `assets/` 相对路径解析。

## 踩过的坑（重要）

1. **mcpp 不会自动生成 bin 目标**：`mcpp` 只在存在 `src/main.cpp` 时才推断
   可执行目标。启用 `app-main` 后 main 在依赖包里，必须显式声明
   `[targets.tinynext] kind = "bin"`（见 `mcpp.toml`）。
2. **`app-main` 与测试互斥**：该特性会把 `glfw_app_main.o` 急切地链入，
   与任何定义 `main()` 的测试 TU 冲突（`multiple definition of 'main'`）。
   本项目因此删除了 `tests/`。
3. **Winsock 必须手动初始化**：tinyhttps 的 `Socket::platform_init()`（内部调用
   `WSAStartup`）只在它自己的测试里调用，库本身不调用。消费方不初始化的话，
   所有连接都以 "Connection failed" 失败（已验证）。`DownloadManager`
   构造函数/析构函数负责 `platform_init()` / `platform_cleanup()`。
4. **`import std;` 后禁止再 `#include` 标准头**：`download_manager.hpp` 被
   `import std;` 的 TU 包含，因此头文件内不能 `#include <mutex>` 等，否则
   "redefinition of 'defer_lock'" 报错（std 模块已声明这些实体）。
5. **安全提示**：tinyhttps 默认 `verifySsl=true` 但 mbedTLS 用 `VERIFY_OPTIONAL`
   握手，证书校验并非强制。如需严格校验，需自行提供 CA bundle
   （`SSL_CERT_FILE`）或改进库。
6. **双击不弹终端**：Windows 默认把 exe 链接成控制台子系统，双击会附带一个
   黑窗口。已在 `mcpp.toml` 的 `[target.'cfg(windows)'.build]` 里加了
   `-Wl,-subsystem:windows` + `-Wl,-entry:mainCRTStartup`（GUI 子系统的默认
   入口是 WinMainCRTStartup，而入口代码是 `main()`，必须显式指回
   mainCRTStartup），现在直接启动 GUI 窗口、无控制台。
