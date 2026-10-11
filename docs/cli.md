# TinyNext 命令行（CLI）

TinyNext 是单实例 GUI 下载器，但也可以从命令行发起下载。CLI 的本质是：**给主
应用传下载链接，由它来下载**，而不是一个独立的命令行下载器。

## 用法

```bash
# 添加一个下载（HTTP(S) / SFTP / ED2K / 磁力 / 本地 .torrent）
tinynext https://example.com/file.zip
tinynext 'magnet:?xt=urn:btih:...'
tinynext ./some.torrent

# 一次添加多个
tinynext https://a.example.com/1.zip https://b.example.com/2.bin

# 多 URL 合并为一个镜像任务：首 URL 为主，其余为同一文件的镜像源
# （aria2 多源并发分段下载、源挂自动切换，只产出一个文件）
tinynext --mirror https://fast.example.com/big.iso https://slow.example.org/big.iso

# 脚本模式：不开窗，按 TinyNext 自身配置下载完退出（exit 0 = 成功 / 1 = 失败）
# 适合脚本 / 定时任务；不依赖 GUI，可被 cron 等调用
tinynext --headless https://example.com/file.zip
echo "exit code: $?"   # 0 = 全部成功，1 = 任一失败或引擎不可用

# 应用没在运行时：自动启动应用（首帧后直接进托盘待机，不弹窗；托盘菜单「显示」唤回）
# 并加入下载列表。扩展宿主自拉起同样如此；自定义启动器可显式传 --start-hidden。
# 应用已在运行时：把链接转发给已运行实例，它自动添加；Windows 上还会把窗口切到前台

# 给 AI/脚本看的 CLI 使用教学（打印到 stdout 后退出，不进 GUI）
tinynext agent
```

可下载源前缀：`http://` / `https://` / `sftp://` / `ed2k://|file|` 或
`magnet:`；另接受以 `.torrent` 结尾的本地文件路径。其他参数会被忽略；第一个参数若是
`agent` / `--agent` / `help` / `--help` / `-h`，则打印上面的教学文本并退出。

## 单实例规则

- 同一用户同一会话**同时只运行一个 TinyNext**。重复启动不会开第二个窗口。
- 第二实例启动时：
  1. 优先走 **TCP loopback socket** 直连主实例（端口写在 `<temp>/tinynext.port`），
     把 URL（每行一个）发给主实例的监听线程；
  2. socket 未就绪（主实例还在启动）时回退写 `<temp>/tinynext.inbox`（每行一个 URL），
     主实例下次唤醒会 drain；
  3. Windows 上尝试 `SetForegroundWindow` 把已有窗口切到前台；
  4. 立即退出（`exit 0`，不闪窗口）。
- 主实例的后台监听线程**阻塞在 accept 上**（队列空就挂起，零轮询），收到 URL 后
  唤醒 UI 线程，由 `g_tasks.startFromUrl`（`tinynext.store.tasks`）逐个加入下载列表
  （URL 校验与「添加下载」弹窗一致：http(s)/sftp/ED2K file 链接、magnet: 磁力、本地
  .torrent，http 不做升级）。
- `--mirror` 模式跨进程保留：第二实例转发时把 `--mirror url1 url2 ...` 编码成单行
  `mirror:<主URL> <镜像1> <镜像2> ...`（URL 不含空格，空格分隔安全），socket / inbox
  两路都不被拆开；主实例解码后重建多源任务（`dl::StartOptions::mirrors`）。
- URL 前缀白名单统一在 `isDownloadableSource`（`src/utils.cppm`，`tinynext.utils`）
  一处维护。

## 控制面（status / list / pause / …）

只读命令（`status` / `list` / `watch`）由 CLI 进程直问主实例的 IPC 线程；
写命令（`pause` / `resume` / `cancel` / `retry` / `remove` / `clear` / `quit`）
会 marshal 回主实例的 UI 线程执行。全部支持 `--json`（写命令返回
`{"ok":…,"results":[{"action","ok","message"}…]}`）；文本输出用 `error: ` 开头
标识失败。退出码：0 成功 / 1 应用未运行·不可达·版本过旧 / 2 命令错误（id 不对、
状态不对等）。给 AI 的完整清单可以跑 `tinynext agent`。

**等待用户选择的任务 `resume` 会被拒绝**（exit 2）：磁力/种子任务读到元数据后停在
「等待选择种子文件」、HLS/DASH 任务清单解析后停在「等待选择媒体轨道」时，继续下载
需要先在 GUI 里选（种子文件入口 / 卡片上的「选择媒体轨道」图标）。CLI 会明确告诉
你原因，而不是假报 `ok: resumed`：

```bash
tinynext list                       # 标记 「awaiting selection: #2 (torrent files)」
tinynext status                     # 「waiting : 1 paused task(s) …」
tinynext status --json              # tasks.awaitingSelection
tinynext resume 2                   # error: #2 is waiting for torrent file selection (…)
```

## 浏览器扩展（Chrome）

仓库 `extension/chrome/` 提供 Chrome 扩展（MV3）：链接右键菜单转发 + 可选的
浏览器下载接管。它不直连 TinyNext，而是走 Chrome 官方 Native Messaging：
Chrome 以 `chrome-extension://<id>/` 参数拉起本主程序进入宿主模式
（`src/browser_host.cppm`，stdio 上的长度前缀 JSON 协议），宿主再经本文件描述的
单实例 TCP / inbox 通道转发给运行中的实例；主实例不在时落 inbox 并自动拉起 GUI。

- 宿主清单：GUI 启动时自动写到 `<configDir>/browser-host/com.tinynext.host.json`
  并登记 Chrome（Windows HKCU 注册表；Linux/macOS 用户配置目录），幂等。
- 扩展固定 ID：`chrome-extension://ebhblmhndabomflakjjljpipfolmajia/`
  （manifest `key` 派生；宿主白名单写死此 origin）。
- 调试：`tinynext --native-host` 直接进宿主循环，可手动喂消息。
- 安装步骤与细节见 `extension/chrome/README.md`。

## 实现位置

- `src/cli.cppm`（模块 `tinynext.cli`）：
  - `commandLineUrls()` — 解析命令行里的 URL 参数；命令行本身由
    `tinynext.utils::commandLineArgs()`（`src/utils.cppm`）统一读取（Windows
    `CommandLineToArgvW`，macOS `_NSGetArgc/Argv`，Linux `/proc/self/cmdline`）。
    Linux 下若进程是**经动态加载器直接启动**的（`ld.so --library-path … /path/tinynext
    agent`，见根目录 `run.sh`——工具链 glibc 与系统 glibc 不兼容时只能这么跑），
    cmdline 的 argv[0] 是 ld.so、程序路径是中间一个普通参数，所以解析会先剥掉加载器
    自己的选项再跳过程序路径（否则 `agent` / `status` / `--headless` 这类按首参数
    分派的入口全部失效）；
  - `acquireSingleInstance()` — Windows 命名互斥体 / POSIX `flock`；
  - `forwardToRunningInstance()` — TCP socket 直连（回退写 inbox）+ 聚焦窗口；
  - `drainInbox()` — 读并清空 inbox（socket 未就绪时的兜底）；
  - `startCliIpc()` — 后台线程：bind 127.0.0.1 随机端口 + 写端口文件，阻塞 accept；
  - `processPendingUrls()` — UI 线程每次被唤醒时调用：首帧加自身 CLI URL，随后处理
    socket 队列 + 兜底 inbox；
  - `CliBoot` — 静态初始化：浏览器扩展宿主模式（`chrome-extension://` 参数或
    `--native-host`）最先接管；第二实例转发后 `exit(0)`。
- 端口文件：`<temp>/tinynext.port`（主实例写入，第二实例转发时读取）；兜底 inbox：
  `<temp>/tinynext.inbox`。

## 排查

- 转发没生效？先看主实例是否在跑；再看 `%TEMP%\tinynext.port`（Windows）里有没有端口、
  `%TEMP%\tinynext.inbox` 里有没有回退写入的 URL。
- 加了 URL 但没下载？确认是上述可下载源前缀之一（或 `.torrent` 路径）；文件名取自 URL 最后一段。
