# TinyNext 下载器

一个用 C++23 编写的 GUI 下载器：**EUI-NEO** 前端 + **tinyhttps** 下载引擎，全部通过 mcpp 包管理。

## 构建与运行

```bash
mcpp build          # 编译
mcpp run            # 启动 GUI 窗口
```

下载的文件保存在运行目录下的 `downloads/` 文件夹。

## 使用

1. 在输入框粘贴 **HTTPS** 下载链接（`http://` 会自动升级为 `https://`，其他协议会被拒绝——tinyhttps 只支持 HTTPS）。
2. 点击「下载」或按回车开始。
3. 每个任务显示文件名、进度条、百分比、实时速度；下载中可「暂停」/「取消」，暂停后可「继续」，完成后可「打开」在资源管理器中定位文件。
4. 同名文件自动加 ` (1)`、` (2)` 后缀，不会互相覆盖。

## 暂停/继续（实现说明）

暂停是**线程内阻塞**实现的，不是断点续传：

- worker 在 tinyhttps 每次读块的边界（`isCancelled` 回调）处按条件变量停车，暂停时**连接保持打开、不读字节**，继续时原地恢复。
- 因此暂停是即时的、恢复是无损的；下载进度不会倒退。
- **局限**：暂停期间连接仍占着；若服务器空闲超时（多数 60~75s）断开了连接，继续后读取会失败、任务进入「失败」。长时间暂停有风险，短暂停完全正常。
- 若关闭程序时仍有暂停任务，会自动取消并立即回收（shutdown 不会挂起）。
- tinyhttps 不支持 HTTP Range/分片，真正跨进程的断点续传需要扩展它（后续可做）。

## 技术栈

| 组件 | 包 | 版本 |
|------|-----|------|
| UI 框架 | `compat:eui-neo` | 0.5.3（feature: `app-main`） |
| 下载引擎 | `mcpplibs:tinyhttps` | 0.2.9 |

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
