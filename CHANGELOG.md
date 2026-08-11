# Changelog

## 0.3.2（2026-08-11）

### 脚本模式 `tinynext --headless <url>`
- **不开窗、按 TinyNext 自身配置下载完退出**：复用 aria2_engine 的 daemon + JSON-RPC，
  不依赖 eui / UI 状态。`exit 0` = 全部成功，`exit 1` = 任一失败或引擎不可用。适合
  脚本 / 定时任务（定位是 TinyNext 用户的脚本工具，roadmap 项落地）。
- 支持多 URL（逐个任务）；接受 http(s)/ftp(s)/sftp / magnet: / 本地 .torrent。
  失败任务保留在会话文件（下次 GUI 启动 aria2 控制文件续传，符合断点续传语义）。
- **daemon 输出重定向**：aria2-next 的进度摘要 / 错误日志不再刷进应用终端，写入
  `configDir/tinynext-aria2.log`（Windows CreateProcess / POSIX posix_spawn 都重定向
  stdout/stderr；GUI 与 headless 共用）。

### 镜像多源补完（0.3.0 实验性的收尾）
- **任务卡显示镜像数**：镜像任务的卡片信息区显示「镜像 ×N」（N = 除主 URL 外的
  镜像源数），一眼看出是否多源。
- **CLI `--mirror` 开关**：`tinynext --mirror url1 url2 ...` 把所有 URL 合并为一个
  多源任务（首 URL 为主、其余为镜像源），与添加弹窗的「多行URL合并为镜像」等价。
  跨进程保留：单实例转发 / inbox 回退用 `mirror:` 前缀单行编码，主实例收到后重建
  多源任务。要求全部为普通 http(s)/ftp(s)/sftp 链接（磁力/种子无多源概念，混入则
  退回逐条任务）。
- **镜像随会话恢复**：会话恢复时从 `files[0].uris` 重建 `opts.mirrors`——重启后
  多源不丢（retry 复用 opts 时带上镜像），任务卡继续显示镜像数。注意 aria2 返回的
  uris 顺序不稳定，恢复后主 URL 可能不是原来的第一个，但多源完整保留。

## 0.3.1（2026-08-11）

### UI
- **关闭标题栏调试统计**：0.3.0 开发期开的 `showDebugStatsInTitle(true)`（窗口标题显示
  FPS / CPU / GPU 等调试数据）本版起关闭，标题保持干净的「TinyNext 下载器」。排障时
  再开。

### 工程
- **版本升 0.3.1**（`mcpp.toml` `[package].version`，`src/versions.generated.h` 重新生成）。
- 修复 `scripts/gen-versions.ps1` 在本机 Windows PowerShell 5.1 下**写入不落盘**的隐性
  bug：脚本含中文注释（UTF-8 无 BOM），PS 5.1 按 ANSI 解码后字节被破坏成引号字符，
  干扰解析导致写入失效（与 `make-dist.ps1` 顶部"保持 ASCII-only"警告同源）。改为纯
  ASCII 注释 + `[IO.File]::WriteAllText`（精确 LF / UTF-8 无 BOM），与
  `gen-versions.sh` 交叉幂等。

## 0.3.0（2026-08-11）

> 跳大版本：本版是一次能力升级（下载类型扩展 + 配置体系 + 设置页改版）。
> 0.2.10 的 eui 升级 / uiScale 缩放 / 性能修复等内容并入本版本（0.2.10 标签虽已
> 推送但因 CI 失败未发布，此处不再单列）。

### 下载类型扩展
- **本地 .torrent 种子文件**：添加弹窗新增「种子文件」行（文件选择器 + 只读路径），
  走 `aria2.addTorrent`（base64，复用 IXWebSocket 的 `macaron::Base64::Encode`）；
  有种子时 URL 框可留空，任务名以种子真实名为准。CLI 也接受 `.torrent` 路径参数。
- **FTP / SFTP / FTPS 链接**：`startDownloadFromUrl` / CLI / 剪贴板预填 白名单统一
  放开 `ftp://`、`sftp://`、`ftps://`（aria2 原生支持，纯过滤扩展）。注意 SFTP 依赖
  `~/.ssh/known_hosts`，首次连未知主机可能失败（可用 `--ssh-host-key-md`，本期未做成
  配置项）。
- **修复 CLI 丢弃 magnet**：`commandLineUrls()` 只保留 http(s)，连 magnet 都被过滤
  （与文档不符）。CLI 现在接受 http(s)/ftp(s)/sftp/magnet/.torrent。URL 前缀白名单
  收敛到 `utils::isDownloadableSource` 一处维护。

### 配置项（设置页新增 13 个 aria2 参数）
- **BitTorrent**：做种时间（`--seed-time`）、做种比率（`--seed-ratio`，空=不限）、
  最大 peers（`--bt-max-peers`）、监听端口（`--listen-port`）、局域网发现
  （`--bt-enable-lpd`）。
- **HTTP**：自定义请求头（`--header`，多行 → 多个 flag）、Cookie 载入/保存
  （`--load-cookies` / `--save-cookies`）。
- **下载行为**：全局限速（`--max-overall-download-limit`，区别于每任务限速）、文件
  分配（`--file-allocation`：默认/none/trunc/falloc）、自动改名与允许覆盖
  （原硬编码在 daemon 命令行的 `--auto-file-renaming` / `--allow-overwrite` 放开为
  配置项，默认值不变）。
- **完整性校验**：`--check-integrity`、`--checksum`。
- 全部为 daemon 级参数（重启生效，与现有行为一致）；设置页 aria2 区按子节归类。

### 设置页改版
- **配置分组子侧边栏**：镜像下载页的任务列表子侧边栏，设置页左侧新增「配置」分组
  （**全局 / 直链下载 / BitTorrent / 下载行为 / 完整性校验**，按下载源模块区分），
  每组配置独立成页，避免全部参数挤在一屏滚动过长。底部「恢复默认/保存/放弃」操作行
  跨组固定可见。分组：全局=主题/关闭行为/下载路径；直链下载=分片/连接/最小分片/
  代理/重试/每任务限速/UA/Referer/请求头/Cookie；BitTorrent=做种/peers/端口/局域网；
  下载行为=并发/全局限速/文件分配/改名/覆盖/完成命令/移除控制文件/磁盘缓存；
  完整性校验=check-integrity/checksum。

### 工程
- **版本信息改为 mcpp.toml 单一来源**：新增 `scripts/gen-versions.ps1/.sh`，从根目录
  `mcpp.toml` 解析应用版本与 eui/websocket/json 依赖版本，生成
  `src/versions.generated.h`；`config.cppm` 的 `kAppVersion`/`kEuiVersion` 改读该头。
  版本只在 `mcpp.toml` 维护，升级后跑一次生成脚本（`make-dist` 打包前自动跑）。
  说明：mcpp 本身无编译期版本注入，生成脚本是"编译时读 mcpp.toml"的最贴近实现。

### CI
- **GitHub Actions 全面升到 node24 运行时**：`softprops/action-gh-release` v2 → **v3**
  （v2 还在 node20，已淘汰）。核对全工作流：`checkout@v7` / `cache@v6` /
  `upload-artifact@v7` / `download-artifact@v8` / `action-gh-release@v3` 全部 node24，
  无残留 node20/18/16 的 action。

### 多 URL 镜像合一（实验性）
- 添加弹窗新增「多行URL合并为镜像」开关：URL 框多行 + 勾选后，首行为主 URL、其余为
  同一任务的镜像源，`aria2.addUri` 一次传入多源数组——aria2 从多源并发分段下载
  同一文件、源挂自动切换，只产出一个文件。重下（retry）复用镜像源。
- **实验性**：任务卡/会话恢复仍以首 URL 为准，镜像不随会话恢复；CLI `--mirror`
  开关、镜像数显示等留到下版完善。

### 修复
- **输入框文字过大 + 字体混用**：eui `components::input` 内部默认
  `metrics_.typography.input = 17`、`fontFamily = "Microsoft YaHei"`。改 uiScale
  原生缩放后 app 自己的字号已回到设计值（标签 11-12），但 input 内部这个 17 仍按
  设计值放大 → 设置页/添加弹窗输入框文字被放得比标签大 ~60%（比「设置」标题还大），
  Windows 上还混用雅黑。修复：主题里把 `metrics.typography.input` 覆写为设计值 13
  （比标签略大、小于标题），所有输入框显式 `fontFamily("")` 改用应用字体
  （Noto Sans SC），与标签一致。

### UI 框架
- **eui-neo 升到 0.5.6**（索引已收录，sha256 `0df8d798…`）：上游新增
  `core/window/window_input_backend.cpp`（输入/IME 事件泵独立成 TU），`-fno-char8_t`
  保留（u8string 问题未修）。0.5.6 起 `eui_neo.h` 不再包含 `dsl_app_impl.h`，入口
  机制挪进 `app-main` 的 `glfw_app_main.cpp` 内部，对本项目无影响。

### 缩放
- **改用 eui 原生 `uiScale`，删除 `utils::S()`**：`DslAppConfig::uiScale(kUI)` 按
  `dpiScale × uiScale` 放大整个逻辑坐标系（布局+字号）。276 处 `S(x)` 调用改回设计
  值；`kUI=1.4` 保留为唯一缩放旋钮。窗口物理尺寸 = 设计尺寸 × `kUI`。渲染输出与
  0.5.5 像素级一致（100% / 150% DPI 均验证）。
- **最大帧率改 `fps(0)` 自动匹配显示器刷新率**（不再写死 90）。

### 性能修复
- **修复挂机时 GPU 占用持续跳动**：根因是根 stack 挂了 `.onFrame`——eui 会把挂
  onFrame 的元素当成「每帧都在动」，无条件每帧重绘，空闲也 90 FPS 满帧。改为去
  onFrame + 后台线程事件驱动，只在真有事时唤醒 UI 一帧：
  - CLI 转发改 **TCP loopback socket**（后台线程阻塞 accept，零轮询；回退 inbox 文件）；
  - 新增 `housekeep` 后台线程：状态消息 4s 过期 / 下载完成失败通知 / 活动任务进度
    刷新，有变化才唤醒；
  - 主题变化（theme_watch）与 WS 状态事件（aria2 完成/失败）直接唤醒。
  - 结果：空闲 `0 FPS / 0% CPU / 0% GPU`。

### 关于
- **关于弹窗版本不再硬编码**：应用版本（`cfg::kAppVersion`）与 eui 版本
  （`cfg::kEuiVersion`）集中到 `config.cppm`，升级只改配置。修复了界面上残留的
  「EUI-NEO 0.5.3」旧版本号。说明：mcpp 无法在编译期注入版本宏，故 eui 版本仍需
  在 `config.cppm` 维护（与 `mcpp.toml` 依赖版本同步）。

### 修复
- **输入框文字过大 + 字体混用**：eui `components::input` 内部默认
  `metrics_.typography.input = 17`、`fontFamily = "Microsoft YaHei"`。改 uiScale
  原生缩放后 app 自己的字号已回到设计值（标签 11-12），但 input 内部这个 17 仍按
  设计值放大 → 设置页/添加弹窗输入框文字被放得比标签大 ~60%（比「设置」标题还大），
  Windows 上还混用雅黑。修复：主题里把 `metrics.typography.input` 覆写为设计值 13
  （比标签略大、小于标题），所有输入框显式 `fontFamily("")` 改用应用字体
  （Noto Sans SC），与标签一致。

### CI / 构建
- `mcpp.lock` 重新解析（eui-neo 0.5.6）。

## 0.2.9（2026-08-10）

### 主题
- **深色模式改为事件驱动**：跟随系统主题不再每 2s 轮询 spawn `gsettings`/`kreadconfig6`
  探测进程，改为后台线程阻塞在 OS 主题变化通知上（Linux inotify / Windows
  WM_SETTINGCHANGE / macOS kqueue），系统切换深浅色即时生效、零轮询。
- 修复：macOS kqueue 用 `NOTE_*`（非 `EV_NOTE_*`）并补 `<cerrno>`；Windows 的
  `setCloexec` 只在 POSIX 编译。

### 设置
- **「关闭时缩到托盘」开关**（配置键 `close_to_tray`）：开启后关闭窗口缩到系统托盘
  （托盘菜单「显示/退出」），重启生效。仅 Windows/macOS 有效；Linux 因 eui-neo
  配方托盘为 stub 无实际效果（X 仍直接退出）。

### CI / 构建
- `mcpp.lock` 重新解析（compat glx/x11 包，私有 glibc 2.39→2.44）。

## 0.2.7（2026-08-08）

### 下载
- **文件名用服务器真实名**：HTTP 下载未显式重命名时不再强制用 URL 末尾段当文件名，
  aria2 从响应头 Content-Disposition 解析真实文件名——CDN/网盘链接末尾是 uuid/随机串
  时，文件不再被命名成 uuid。下载目录已有同名完整文件时，自动改名重新下载
  （`RealName.txt` → `RealName.1.txt`），不再误判为"已下载完"。
- **已完成任务可重新下载**：卡片提供 ↻ 重新下载（与失败/已取消任务统一）；重下走
  auto-file-renaming 改名，避免 `continue` 把已存在完整文件判成"已下载完"直接完成。
- **删除/取消统一为确认弹框**：进行中的 ✕ 与结束后的 🗑 都弹「删除任务」确认，可勾选
  「同时删除源文件」——默认勾选，源文件移到系统回收站（可恢复），或仅删记录。
- **删除真正清理会话记录**：forceRemove + removeDownloadResult + saveSession 重写会话，
  应用重启或再下载时不再复活已删任务；同时清理 `.aria2` 控制文件。

### 修复
- Windows「打开所在文件夹」不再落到桌面：路径归一化成绝对原生路径，文件不存在或本身
  是目录时直接打开所在目录（此前 `explorer /select` 收到无效路径会回落到桌面）。
- 设置页「最小分片」切换 KB/MB 单位按 1024 进制自动换算数值；旧配置里的 G 后缀统一
  按 MB 显示，数值上限按当前单位自适应（避免换单位后超保存校验范围）。
- 任务显示名用真实下载路径 / 种子名：卡片、删除弹窗、完成/失败通知、按名称排序统一
  走 `taskDisplayName`，磁力任务不再显示整条磁力 URI。

### UI
- 卡片按钮重新布局：文件操作（打开文件/复制链接/所在文件夹）成组靠左，删除居中，
  重新下载/开始暂停靠右；开始/暂停按钮去掉主色、与同类一致。
- 「打开所在文件夹」按钮任何状态都显示（未完成时打开下载目录）。
- 移除「暂无任务」空提示文案；「任务列表」侧边栏标题字号加大。

### CI
- Windows 钉住 `mcpp@2026.8.8.1` 并先刷 xlings 索引：`mcpp.toml` 的 `[resources]`
  （版本信息/图标嵌入 PE）需 mcpp ≥ 2026.8.7.1，此前 xlings 索引滞后会装到旧版，
  导致 CI 打出的 exe 缺版本信息（本地构建正常）。

## 0.2.5（2026-08-08）

### 字体与 Linux 安装包
- **正文换用思源黑体（Noto Sans SC，OFL）**：移除 eui 默认示例字体「荆南俊俊体」（卡通
  圆体、Bold 单字重、无明确授权），改用 Noto Sans SC Regular，随包附带 OFL 许可文本；
  UI 文字更规整、可随安装包自由分发。
- **修复 Linux 安装包（deb/rpm）启动空白**：安装版启动脚本先 `cd /opt/tinynext` 再经
  系统 loader 拉起二进制。此前经 `ld.so` 启动时 `/proc/self/exe` 指向 loader 而非本程序，
  eui 的字体/图标资产按 exeDir/CWD 解析全部失败，回退系统字体又在 Fedora 上全 miss，
  导致页面空白无字、图标异常。
- **修复任务栏图标**：桌面项加 `StartupWMClass=TinyNext 下载器`，窗口不再显示系统默认
  图标（此前 WM_CLASS 是中文窗口标题，匹配不上桌面项，出现双图标）。

### 修复
- Windows 跟随系统主题：`osDark()` 从错误的 DLL 查 `SHGetValueW`，导致跟随系统主题时
  永远深色。

## 0.2.4（Unreleased / 开发中）

### CI
- GitHub Actions 升级到 Node 24 大版本（checkout v7 / cache v6 / upload-artifact v7 /
  download-artifact v8），消掉 Node 20 弃用警告。已核对 upload 的
  `name/path/if-no-files-found`、download 的 `path/merge-multiple` 语义不变，
  release artifacts 流程照旧。

## 0.2.3（Unreleased / 开发中）

### 发布与安装包
- **平台安装包**（CI 打包升级）：Windows 出 NSIS `setup.exe`——**Modern UI 2 现代化
  向导**（欢迎/安装目录/进度/完成页 + 品牌图 + 「运行」复选框，中文文案），per-user 装到
  `%LOCALAPPDATA%\Programs\TinyNext`，无管理员、Add/Remove Programs 卸载项；
  Linux 出 `.deb` + `.rpm`（装 `/opt/tinynext` + 系统 loader 启动器 + 桌面项/图标）；
  macOS 暂保持 tar.gz（签名需 Apple 账号，后续做）。免安装 zip / tar.gz 全部保留。
- **配置/会话移到 per-user 目录**：`tinynext.conf` 与 aria2 `tinynext.session` 从 cwd
  改到 `%APPDATA%\TinyNext`（macOS `~/Library/Application Support` / Linux
  `$XDG_CONFIG_HOME`）——安装版经快捷方式启动时 cwd 可能是 System32 等不可写目录，
  写 cwd 会导致设置存不上、任务无法续传。exe 旁已有旧配置继续优先使用（便携版兼容）。
- CI 打包修复：rpm 桌面图标改用预渲染 `assets/icon.png`（icon.ico 多帧导致 convert
  输出 `-0.png` 而非 `icon.png`）、NSIS 中文向导语言文件 `SimpChinese.nlf` 仓库本地化
  （choco 精简版无 Contrib、且 NSIS3 里文件名是 SimpChinese 而非 ChineseSimplified）、
  makensis 标准目录回退搜索。

### UI
- 下载页状态筛选侧边栏标题「下载状态」→「任务列表」。

### 每任务选项
- **移除每任务单独限速**：限速统一走设置页「每任务限速」全局配置（aria2
  `max-download-limit`），添加弹窗不再有「限速KB/s」输入，`StartOptions.limitBps` 删除。

## 0.2.2（2026-08-07）

### 下载引擎：纯 aria2-next
- 移除内置 tinyhttps 引擎（`TinyHttpsEngine` / `tinynext.download_manager` 模块），**aria2-next 成为唯一下载引擎**。
- aria2 本地 JSON-RPC 改用自写的极简跨平台 `LocalSocket`（winsock / POSIX，非阻塞 connect 超时），`mcpp.toml` 去掉 `tinyhttps` 依赖。
- `http://` 不再强制升级为 `https://`（那是 tinyhttps 只支持 HTTPS 的限制；aria2 原生支持 http）。
- 设置页去掉「下载引擎」切换；`config.cppm` 去掉 `EngineChoice` / `engine()`。
- eui-neo 升到 **0.5.5**（compat 配方加包级 `-fno-char8_t` 修 C++23 构建的 u8string 报错，并补 `-ldwmapi`；版本决策见 `docs/roadmap.md`）。
- Windows 链接补 `-lws2_32`（`LocalSocket` 用 winsock）。

### 下载功能
- **磁力 / BT**：接受 `magnet:` 链接（仅 aria2 引擎）；magnet 任务不设 `out`，元数据落地后 `files[0].path` 显示真实路径。
- **断点续传**：失败 / 已取消卡片 ↻「重新下载」→ aria2 从同目录 `.aria2` 控制文件续传（`retry()` 复用原 URL+路径）。
- **重启会话恢复**：daemon 启动带 `--save-session` / `--input-file`，退出前 `aria2.saveSession`；重启后 `recoverSession()` 用 `tellActive/tellWaiting/tellStopped` 重建任务表。
- **下载完成 / 失败系统通知**：任务从进行中迁移到 Done/Failed 时弹系统通知（Windows 原生
  托盘气泡 `Shell_NotifyIconW` / macOS osascript / Linux notify-send→kdialog，best-effort）。
- **WebSocket 事件推送**（新增 `compat:websocket` / IXWebSocket 依赖）：aria2 的
  `onDownloadStart/Complete/Error/Pause/Stop/BtDownloadComplete` 推送**即时**驱动状态
  迁移与完成/失败通知；进度轮询从 ~5Hz 降到 **1s**（对齐 Motrix/AriaNg）。引擎加
  `tasksMutex_` 让 WS 后台线程安全更新任务。核实：aria2-next **无**
  `--enable-rpc-websocket` flag（HTTP 层检测 `Connection: upgrade` 自动升级）、也**无**
  进度推送事件，故保留轮询补进度；WS 掉线自动回退轮询。
- 卡片显示 **ETA 剩余时间**。

### 配置与每任务选项
- aria2 配置扩展：**代理**（HTTP/HTTPS，aria2 不支持 SOCKS5）、**重试次数 / 等待秒**、**最大同时下载数**（队列并发，默认 5）、**完成后移除控制文件**、**完成后命令**、**User-Agent / Referer / 磁盘缓存**。
- 设置页正文改**可滚动**；aria2 参数在 daemon 已启动时提示「重启后生效」。
- 添加弹窗每任务选项：**连接数 / 重命名 / 下载目录**；打开时**默认填配置的连接数与下载目录**。
- **移除优先级功能**：实测 + `aria2-next --help=#all` 确认 aria2-next **没有下载级
  `priority` 选项**（参数被静默忽略，排队恒为添加顺序），「优先级」选择器 / 排序 /
  `priorityValueFromPicker` 一并删除。想先下哪个就暂停其他任务。

### 其他
- 默认窗口分辨率加大（`S(1120)×S(720)`，实际约 1568×1008）。
- GitHub Actions（`release.yml`）：Release 说明同步纯 aria2；加防御性索引刷新步骤；本地验证 `mcpp build --release` 通过。

### UI：岛屿卡片布局
- **岛屿卡片风**：内容区 / 状态筛选侧边栏做成浮在背景上的圆角"岛"卡（`widgets::drawPanel`，
  中间色调 + 细边框 + 柔和投影），顶部贴齐窗口顶、仅右缘留距。
- **总侧边栏透明**：左侧图标栏恢复整高透明列（不铺底色、不套卡片），让主题背景透出。
- **翻页简化**：◀ 页码 ▶ [数字/页]，整组收进一张小卡片；中间只显示当前页码，分页大小
  为无边框的"数字/页"文本（带小箭头）。
- **页面模块拆分**：`pages.cppm` 拆成 `downloads_page` / `settings_page` / `about_dialog`
  三个独立模块，避免单文件管理过多页面。

### UI：添加下载弹窗
- **URL 多行**：输入框改为 multiline，长链接完整可见；打开时自动读剪贴板，若是
  http(s)/magnet 链接则预填。
- 下载页状态筛选侧边栏标题「下载状态」改「任务列表」。
- **分片数分行**：左标签"连接数"改"分片数"（实际是每任务 split）。
- 下拉修复：展开后点击外部不收起（加全屏拦截层）、排序下拉弹层宽度不足文字溢出
  （加 `popupWidth`）。

### 修复
- **Windows 打开文件/文件夹非阻塞**：`openFile` / `openContainingFolder` 从
  `std::system("explorer …")` 改 `ShellExecuteW`（共享 `shellExecFn()`），不再等 Explorer
  窗口关闭卡 UI 线程。
- **通知改原生实现 + 修编码**：Windows 完成/失败通知从 `powershell -WindowStyle Hidden`
  （会被杀软/主防当作静默执行命令行而拦截）改成原生 `Shell_NotifyIconW` 托盘气泡
  （独立线程 + message-only 窗口，不 spawn 命令行）；编码从逐字节扩宽 UTF-8（乱码）改为
  `MultiByteToWideChar(CP_UTF8)` 正确转码。macOS 通知消息改经 argv 传入 osascript。
- 设置页：移除顶部副标题提示；标题下移、表单首行加顶部占位，避免被滚动区上缘裁掉。
- **Linux 目录选择器取消不再二次弹出**：原 `zenity || kdialog` 把「用户取消」（zenity
  退出码非 0）当成「无 zenity」触发 kdialog 回退；改先 `command -v zenity` 探测，只当
  不存在时才回退 kdialog。
- **Linux 跟随系统深色修正**：现代桌面（GNOME 42+ / KDE Plasma 6）不写
  `gtk-application-prefer-dark-theme` 到 settings.ini，旧检测恒返回浅色。新增按序探测：
  `gsettings color-scheme`（prefer-dark/light）→ KDE `kreadconfig6 ColorScheme` → 旧
  settings.ini（并兼容 `=true` 写法与 `gtk-theme-name` 含 dark）。
- **设置页「恢复默认」改为重置全部设置**：原先只重置下载路径（主题/aria2 参数不动），
  现在一并回默认——主题回「跟随系统」并即时预览、下载路径回系统目录、aria2 参数回
  各字段默认值（仍需点「保存」落盘，与放弃/保存语义一致）。
- **配置/会话移到 per-user 目录**：`tinynext.conf` 与 aria2 `tinynext.session` 从
  cwd 改到 `%APPDATA%\TinyNext`（macOS/Linux 对应）——安装版经快捷方式启动时 cwd
  可能是 System32 等不可写目录，写 cwd 会导致设置存不上、任务无法续传。exe 旁已有
  旧配置则继续优先使用（便携版兼容）。

### 底层能力（较早实现）
- **单实例** + **CLI 传参下载**：重复启动把 URL 写入 `<temp>/tinynext.inbox` 由主实例轮询取走；`tinynext agent` 打印 AI 用法指南。
- EUI-NEO 前端全模块化（`tinynext.ui.*`），`kUI=1.4` 统一缩放。
- 深色 / 浅色 / 跟随系统主题，~2s 轮询 OS 主题。

---

规划与决策记录见 `docs/roadmap.md`；AI 助手指南见 `AGENTS.md`。
