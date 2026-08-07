# TinyNext 下载器

一个用 C++23 编写的**跨平台** GUI 下载器：**EUI-NEO** 前端 + **aria2-next**
外部进程引擎（分片多连接、断点续传、磁力/BT），支持 Windows / Linux / macOS，
全部通过 mcpp 包管理。

## 构建与运行

```bash
mcpp build          # 编译
mcpp run            # 启动 GUI 窗口
```

下载的文件默认保存在你的系统下载目录（可在设置页修改保存路径）。

## 命令行 & 单实例

- **单实例**：同一用户同一会话只运行一个 TinyNext。重复启动不会开第二个窗口，
  而是把命令行参数里的下载链接转发给已运行实例（Windows 上还会把已有窗口
  切到前台），由它自动添加任务。
- **CLI**：`tinynext <https://...>` 启动即添加下载；如果应用未运行，会自动
  打开应用并把链接加入下载列表。可一次传多个 URL。**详细用法见 `docs/cli.md`**。
- 转发走临时目录的 `tinynext.inbox` 文件，主实例每 ~0.5s 轮询取走任务。
- 给 AI 助手的项目指南见 `AGENTS.md`（含构建 / CLI 用法 / 模块约定）。

## 界面

**岛屿卡片风布局**：内容区 / 状态筛选侧边栏是浮在背景上的圆角"岛"卡（`drawPanel` 统一
样式：中间色调 + 细边框 + 柔和投影），顶部贴齐窗口顶（为后续自定义顶部栏预留），左缘是
**透明的总侧边栏**（纯图标、不铺底色），只有卡片右缘留少量间距。

- **左侧图标栏（总侧边栏）**：整高透明列，纯图标导航（下载列表 / 设置），底部按钮切换
  深浅主题；左上角是应用 logo（项目名缩写）。
- **下载状态子侧边栏**：下载页内容区左侧的 **所有 / 下载中 / 已完成** 筛选（独立岛卡）。
- **内容大卡**：下载页的工具栏 + 任务列表 + 翻页收在同一张卡片里。
- **任务卡片**：每个任务一张卡片，纵向排布文件名+状态标签、进度条、信息（百分比/速度/大小）+ 操作图标。
- **添加下载弹窗**：右上角 **➕** 打开；**URL 输入框多行**（长链接完整可见），打开时
  **自动读取剪贴板**（若是 http(s)/magnet 链接则预填）。弹窗里可设置**分片数**（每任务，
  默认填配置值）、**重命名**、**下载目录**。
- **顶部工具栏**（内容大卡右上）：**全部暂停 / 全部继续**、**排序**（最新在前 / 状态优先 /
  文件名 / 大小 / 进度）、**➕ 添加下载**。
- **翻页**：◀ 页码 ▶ [数字/页]，整组收在一张小卡片里；中间只显示当前页码，分页大小是
  无边框的"数字/页"文本（带小箭头，可点开选择 5/10/20/50/100）。

## UI 缩放

EUI-NEO **没有**全局缩放开关（`components::button` 自带的 `.scale()` 只作用于组件
按钮），因此 TinyNext 在 `src/app.cpp` 里用一个统一系数 **`kUI`**（默认 `1.4f`）+
辅助函数 `S(x) = x * kUI`，把所有尺寸 / 字号 / 间距和窗口尺寸整体放大。想整体
改大改小，只调 `kUI` 一个数即可。布局在 EUI 的逻辑像素空间（= 窗口屏幕像素），
所以**窗口与内容必须一起放大**，高 DPI 屏上整体才会真正变大（本机 2560×1600
@150% 下，1.4 倍后窗口约 1568×1008）。

## 使用

1. 点击右上角 **➕** 打开「添加下载」弹窗，粘贴 **HTTP(S)** 链接或 **magnet:** 磁力链接，点「提交」或按回车开始。
2. 弹窗里可为该任务设置：**连接数**（打开时自动填配置的默认值，可改）、**重命名**（留空=URL 文件名）、**下载目录**（打开时自动填配置的默认目录，可改；磁力链接建议确认，因为种子内容名不由 URL 决定）。想先下哪个就暂停其他任务（aria2-next 不支持下载优先级，排队按添加顺序）。
3. 卡片操作全部用图标，无文字：
   - **复制链接**、**删除**：所有任务都有；
   - 下载中：**暂停** / **取消**；已暂停：**继续** / **取消**；
   - 失败 / 已取消：**重新下载**（aria2 从 `.aria2` 控制文件断点续传）；
   - 已完成：**打开** / **打开所在文件夹**。
4. 同名文件自动加 ` (1)`、` (2)` 后缀，不会互相覆盖。

## 暂停/继续与断点续传

暂停 / 继续走 aria2 RPC（`aria2.pause` / `aria2.unpause`）：是**真正的中断**，
不占连接，可随时继续，进度不倒退。

- **重新下载**：失败 / 已取消的卡片 ↻ 按钮用原 URL + 原路径重新入队，aria2 从同
  目录的 `.aria2` 控制文件续传（真正的断点续传）。
- **重启恢复**：aria2 daemon 启动带 `--save-session` / `--input-file`，退出时先
  `aria2.saveSession` 持久化未完成任务；下次启动自动载入并续传，任务列表由
  `tellActive` / `tellWaiting` / `tellStopped` 枚举重建。
- 设置页「完成后移除控制文件」开启后，下载完成即删 `.aria2`；未完成（含取消）则
  保留，供重新下载续传。

## 设置（⚙ 设置页）

- **主题**：跟随系统 / 深色 / 浅色（跟随系统时 ~2s 轮询 OS 主题，自动切换）。
- **下载路径**：默认系统下载目录（Windows `%USERPROFILE%\Downloads` / macOS
  `$HOME/Downloads` / Linux `XDG_DOWNLOAD_DIR`），可「浏览」用系统选择器或手输。
- **aria2 参数**（仅 aria2-next）：分片数、每服务器连接（默认 64，上限 64）、
  最小分片（≥1M）、每任务限速（KB/s，0=不限）、**最大同时下载数**（队列并发上限，
  默认 5，范围 1~64）、**代理地址**（HTTP/HTTPS，aria2 不支持 SOCKS5）、**不使用
  代理列表**、**失败重试次数 / 重试等待秒**、**完成后移除控制文件**、**完成后命令**、
  **User-Agent / Referer / 磁盘缓存**。设置项过多，设置页正文可滚动。
  新下载立即生效；daemon 级参数在 aria2 daemon 已启动时需重启才生效。
- 所有设置点「保存」落盘到 `tinynext.conf`（JSON），「放弃」回滚；左侧栏底部 ⓘ
  打开「关于」弹窗（含项目 GitHub 链接）。
- 配置与 aria2 会话文件放在 **per-user 配置目录**（Windows `%APPDATA%\TinyNext` /
  macOS `~/Library/Application Support/TinyNext` / Linux `$XDG_CONFIG_HOME/tinynext`，
  回退 `~/.config/tinynext`）——安装版经快捷方式启动时 cwd 可能是 System32 等不可写
  目录，不能依赖 cwd。若 exe 旁已存在 `tinynext.conf`（便携版旧配置）则继续用它。

## 下载引擎

UI 只面向抽象 `dl::DownloadEngine` 接口（`src/download_engine.cppm`），唯一实现是
`dl::Aria2Engine`（TinyHttpsEngine 已移除）：

- spawn `engines/aria2-next` 守护进程，JSON-RPC 驱动，`-x 64 -s 64` 分片多连接。
- 断点续传（`.aria2` 控制文件）、磁力/BT、重试、限速、代理等能力来自 aria2 本身。
- 本地 JSON-RPC 用自写的极简跨平台 socket（`aria2_engine.cpp` 里的 `LocalSocket`），
  无外部 HTTP/网络依赖。

## 跨平台与 aria2-next 引擎二进制

三平台都需把对应的 aria2-next 二进制放进 `engines/`（已 gitignore，`checksums.sha256` 保留）：

| 平台 | release 资产（v2.5.5） | 放置为 |
|------|------------------------|--------|
| Windows x64 | `aria2-next-2.5.5-windows-x86_64.exe` | `engines/aria2-next.exe` |
| Linux x64 | `aria2-next-2.5.5-linux-x86_64` | `engines/aria2-next` |
| macOS (Apple Silicon) | `aria2-next-2.5.5-macos-arm64` | `engines/aria2-next` |
| macOS (Intel) | `aria2-next-2.5.5-macos-x86_64` | `engines/aria2-next` |

下载页：https://github.com/AnInsomniacy/aria2-next/releases

Windows 发行打包用 `.\make-dist.ps1`：它自动把 `engines/` 里的 aria2 二进制和
`checksums.sha256` 一起打进 `dist\` 与 `tinynext-v<版本>-win64.zip`（版本号从
`mcpp.toml` 读取）。aria2 是**唯一**下载引擎，`engines/` 缺失时脚本会警告但继续
打包（运行时下载不可用）。

平台验证步骤：
1. 各平台 `mcpp build`。Windows 自动加 GUI 子系统标志；Linux 用 `run.sh` 启动
   （规避 mcpp 私有 glibc 与系统 Mesa 的 GLIBC 版本冲突）；macOS 直接 `mcpp run`。
2. 添加一个大文件（≥128MB 才能用满 64 连接）。
3. 文件夹选择器依赖：Linux 需 `zenity`（无则回退 `kdialog`，都没有则手输路径）；
   macOS 用 `osascript`；Windows 系统自带。
4. 主题跟随系统：Windows 读注册表、macOS 读 `AppleInterfaceStyle`、Linux 读 gtk
   `settings.ini`（均 best-effort）。

## 发布（GitHub Releases）

CI 工作流 `.github/workflows/release.yml` 在推送 `v*` 标签时自动在 Windows /
Linux / macOS 三平台构建并创建 Release（也可 `workflow_dispatch` 手动触发——只
构建并上传 artifacts、不建 Release，便于先修跨平台编译错误）。

流程：

1. 把工作流推到仓库后，先用 `workflow_dispatch` 跑一遍，按失败作业逐一修复
   Linux / macOS 的编译问题（这两个平台是首次在 CI 编译 POSIX 分支）。
2. 三平台都绿后打标签并推送：
   ```bash
   git tag v0.2.3 && git push origin v0.2.3
   ```
3. 到仓库 Releases 页把自动生成的 **draft** release 补充说明后发布。

产物（aria2-next 二进制在 CI 上按 `engines/checksums.sha256` 校验后随包附上）：

**安装包 + 免安装版**：

| 平台 | 安装包 | 免安装版 | 打包脚本 |
|------|--------|----------|----------|
| Windows x64 | `tinynext-v*-win64-setup.exe`（NSIS） | `tinynext-v*-win64.zip` | `make-win-pkg.ps1` |
| Linux x64 | `tinynext-v*-linux-x86_64.deb` / `*.rpm` | `tinynext-v*-linux-x86_64.tar.gz` | `make-linux-pkg.sh` |
| macOS Apple Silicon | —（暂不做 dmg） | `tinynext-v*-macos-arm64.tar.gz` | `make-dist.sh macos arm64` |

- **Windows 安装包**：per-user 装到 `%LOCALAPPDATA%\Programs\TinyNext`（无需管理员），
  开始菜单/桌面快捷方式、Add/Remove Programs 卸载项齐全。未签名，SmartScreen 可能提示。
- **Linux 安装包**：装到 `/opt/tinynext` + `/usr/bin/tinynext` 启动器 + 桌面项；
  启动器走系统 loader（Debian multiarch + Fedora `/usr/lib64` 都覆盖）。`.deb` 声明
  `Depends: libc6 (>= 2.39), libgl1`，目标机器需 glibc ≥ 2.39 且有桌面 Mesa/GLX。
- **Linux 免安装包**内含 `run.sh`（走系统 loader + 系统 Mesa，原理见仓库根 `run.sh`），
  同样需 glibc ≥ 2.39 且有桌面 GLX。

（macOS Intel 暂不参与 CI 构建——官方 mcpp install.sh 只提供 macosx-arm64
二进制；等 Intel 的 mcpp 二进制或 macOS 上 subos 安装验证后再加 `macos-13`
runner。）

## 技术栈

| 组件 | 包 | 版本 |
|------|-----|------|
| 工具链 | LLVM/Clang（`mcpp.toml` 的 `[toolchain]` 固定） | 22.1.8 |
| UI 框架 | `compat:eui-neo` | 0.5.5（feature: `app-main`；配方加 `-fno-char8_t` 修 C++23 构建） |
| 下载引擎 | `aria2-next`（外部进程） | 2.5.5 |
| 配置 JSON | `nlohmann:json` | 3.12.0 |

### 架构

全模块化（`import std` + 各 `tinynext.*` 模块），按职责拆成多个模块：

| 模块 | 文件 | 职责 |
|------|------|------|
| `tinynext.download_engine` | `src/download_engine.cppm` | 引擎抽象接口 `dl::DownloadEngine` / `TaskView` |
| `tinynext.aria2_engine` | `src/aria2_engine.cppm/.cpp` | aria2-next 进程引擎（JSON-RPC + 本地 socket） |
| `tinynext.config` | `src/config.cppm` | JSON 配置 / 主题 / 下载目录 / aria2 参数 |
| `tinynext.cli` | `src/cli.cppm` | 单实例锁 + 命令行 URL + inbox 转发 + CliBoot 引导 |
| `tinynext.ui.utils` | `src/ui/utils.cppm` | `kUI`/`S()` 缩放 + 格式化/解析辅助 |
| `tinynext.ui.theme` | `src/ui/theme.cppm` | `AppTheme` 深浅主题 + `currentTheme()` |
| `tinynext.ui.state` | `src/ui/state.cppm` | 共享可变全局 + 引擎 + 添加下载流程 |
| `tinynext.ui.platform` | `src/ui/platform.cppm` | DPI boot + 文件夹选择 + 打开文件/URL |
| `tinynext.ui.widgets` | `src/ui/widgets.cppm` | 列表选择器 + 侧栏/rail/卡片操作控件 + `drawPanel` 岛卡 |
| `tinynext.ui.cards` | `src/ui/cards.cppm` | 下载任务卡片 |
| `tinynext.ui.downloads_page` | `src/ui/downloads_page.cppm` | 下载页 + 添加下载弹窗 |
| `tinynext.ui.settings_page` | `src/ui/settings_page.cppm` | 设置页 |
| `tinynext.ui.about_dialog` | `src/ui/about_dialog.cppm` | 关于弹窗 |
| `src/app.cpp` | —（普通 TU） | 薄入口：`app::dslAppConfig()` + `app::compose()` 分发 |

页面按职责拆成独立模块（原 `tinynext.ui.pages` / `pages.cppm` 已删除）。

- `src/app.cpp` — EUI 应用入口。启用 `app-main` 特性后，`main()` 由包内的
  GLFW 入口（`core/app/glfw_app_main.cpp`）提供，本项目只定义
  `app::dslAppConfig()` 和 `app::compose()`（**因此任何 TU 都不能再定义 `main()`**）。
- EUI-NEO 是 header-only C++17 库（无模块接口）。`src/app.cpp` 包含完整
  `<eui_neo.h>`（提供 `dsl_app_impl.h` 里的 `app::update/render` 等机制实现）；
  各 UI 模块只包含精简头 `src/ui/eui_ui.h`（去掉 `dsl_app_impl.h`）——该头内联
  lambda 若同时出现在普通 TU 和模块全局片段会 mangled name 冲突。
- `assets/` — EUI 默认中文字体（JingNanJunJunTi）+ 图标字体，运行时按
  `exeDir/assets/` 或 `assets/` 相对路径解析。

## 踩过的坑（重要）

1. **mcpp 不会自动生成 bin 目标**：`mcpp` 只在存在 `src/main.cpp` 时才推断
   可执行目标。启用 `app-main` 后 main 在依赖包里，必须显式声明
   `[targets.tinynext] kind = "bin"`（见 `mcpp.toml`）。
2. **`app-main` 与测试互斥**：该特性会把 `glfw_app_main.o` 急切地链入，
   与任何定义 `main()` 的测试 TU 冲突（`multiple definition of 'main'`）。
   本项目因此删除了 `tests/`。
3. **Winsock 必须手动初始化**：Windows 上本地 JSON-RPC socket 需要 `WSAStartup`。
   `aria2_engine.cpp` 里自写的 `LocalSocket::platformInit()`（POSIX 是 no-op）在
   `Aria2Engine` 构造 / 析构里负责 `WSAStartup` / `WSACleanup`，不要漏。
4. **`import std;` 后禁止再 `#include` 标准头**：被 `import std;` 的 TU 包含的
   头文件内不能 `#include <mutex>` 等标准头，否则 "redefinition of 'defer_lock'"
   报错（std 模块已声明这些实体）。
5. **RPC 仅限本机**：aria2 daemon 用 `--rpc-listen-all=false` 只监听 127.0.0.1，
   `--rpc-secret` 随机生成，本地 RPC 不会被外部访问。
6. **双击不弹终端**：Windows 默认把 exe 链接成控制台子系统，双击会附带一个
   黑窗口。已在 `mcpp.toml` 的 `[target.'cfg(windows)'.build]` 里加了
   `-Wl,-subsystem:windows` + `-Wl,-entry:mainCRTStartup`（GUI 子系统的默认
   入口是 WinMainCRTStartup，而入口代码是 `main()`，必须显式指回
   mainCRTStartup），现在直接启动 GUI 窗口、无控制台。
7. **EUI-NEO 无全局缩放开关**：只有自动 DPI 感知（`highDpi=true`，逻辑坐标 =
   窗口屏幕像素，渲染按 `dpiScale` 换算保证高 DPI 清晰）和组件 button 的
   `.scale()`（只作用于单个按钮）。整体放大 UI 需要自己引入系数（见上文
   「UI 缩放」），并同时放大窗口尺寸，否则高 DPI 屏上控件仍然偏小。
8. **eui 元素 id 必须全局唯一**：同一个 frame 里同名 id 会互相覆盖——例如
   `components::text` 的标签 id 若写成 `tool.sort.label`，会和
   `buildListPicker(id="tool.sort")` 内部的字段标签 id 撞名，导致文字不显示。
   新增控件 id 要避开已有前缀。
9. **Windows 打开文件/文件夹不要用 `std::system("explorer …")`**：`explorer` 从命令行
   启动会让调用进程同步等 Explorer 窗口关闭，UI 线程卡死。统一走
   `ShellExecuteW`（`platform.cppm::shellExecFn()`，立即返回）。
10. **xlings 解压含 symlink 的 tarball 会在 Windows 上中途失败**：下载包里若有符号链接
    （如 IXWebSocket 的 `Dockerfile` → `docker/Dockerfile.alpine`），解压器解到该条目即
    中止，源码目录缺失导致 mcpp 报 `install_packages failed`。下载/校验本身没问题；
    需要手工用系统 tar 完整解压（跳过 symlink）补装 verdir，见仓库根 CLAUDE.md。

## 许可

本项目源码采用 **MIT** 协议（见 `LICENSE`）。

注意：下载引擎 **aria2-next**（`engines/` 下的二进制，GPLv2）是随发行包
**单独分发**的第三方程序，不改变本项目 MIT 许可的状态；其自身仍受 GPLv2 约束。
