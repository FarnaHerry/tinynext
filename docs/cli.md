# TinyNext 命令行（CLI）

TinyNext 是单实例 GUI 下载器，但也可以从命令行发起下载。CLI 的本质是：**给主
应用传下载链接，由它来下载**，而不是一个独立的命令行下载器。

## 用法

```bash
# 添加一个下载（HTTP(S)）
tinynext https://example.com/file.zip

# 一次添加多个
tinynext https://a.example.com/1.zip https://b.example.com/2.bin

# 应用没在运行时：自动启动应用并加入下载列表
# 应用已在运行时：把链接转发给已运行实例，它自动添加；Windows 上还会把窗口切到前台

# 给 AI/脚本看的 CLI 使用教学（打印到 stdout 后退出，不进 GUI）
tinynext agent
```

非 URL 参数（不以 `http://` / `https://` 开头）会被忽略；第一个参数若是
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
  唤醒 UI 线程，由 `startDownloadFromUrl` 逐个加入下载列表（URL 校验与「添加下载」
  弹窗一致：http(s) 链接或 magnet: 磁力链接，http 不做升级）。

## 实现位置

- `src/cli.cppm`（模块 `tinynext.cli`）：
  - `commandLineUrls()` — 解析命令行里的 URL 参数（Windows `CommandLineToArgvW`，
    macOS `_NSGetArgc/Argv`，Linux `/proc/self/cmdline`）；
  - `acquireSingleInstance()` — Windows 命名互斥体 / POSIX `flock`；
  - `forwardToRunningInstance()` — TCP socket 直连（回退写 inbox）+ 聚焦窗口；
  - `drainInbox()` — 读并清空 inbox（socket 未就绪时的兜底）；
  - `startCliIpc()` — 后台线程：bind 127.0.0.1 随机端口 + 写端口文件，阻塞 accept；
  - `processPendingUrls()` — UI 线程每次被唤醒时调用：首帧加自身 CLI URL，随后处理
    socket 队列 + 兜底 inbox；
  - `CliBoot` — 静态初始化：第二实例转发后 `exit(0)`。
- 端口文件：`<temp>/tinynext.port`（主实例写入，第二实例转发时读取）；兜底 inbox：
  `<temp>/tinynext.inbox`。

## 排查

- 转发没生效？先看主实例是否在跑；再看 `%TEMP%\tinynext.port`（Windows）里有没有端口、
  `%TEMP%\tinynext.inbox` 里有没有回退写入的 URL。
- 加了 URL 但没下载？URL 必须以 `http://` 或 `https://` 开头；文件名取自 URL 最后一段。
