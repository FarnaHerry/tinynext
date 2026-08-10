# Changelog

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
