# TinyNext 命令行（CLI）

TinyNext 是单实例 GUI 下载器，但也可以从命令行发起下载。CLI 的本质是：**给主
应用传下载链接，由它来下载**，而不是一个独立的命令行下载器。

## 用法

```bash
# 添加一个下载（HTTPS）
tinynext https://example.com/file.zip

# 一次添加多个
tinynext https://a.example.com/1.zip https://b.example.com/2.bin

# 应用没在运行时：自动启动应用并加入下载列表
# 应用已在运行时：把链接转发给已运行实例，它自动添加；Windows 上还会把窗口切到前台
```

非 URL 参数（不以 `http://` / `https://` 开头）会被忽略。

## 单实例规则

- 同一用户同一会话**同时只运行一个 TinyNext**。重复启动不会开第二个窗口。
- 第二实例启动时：
  1. 把命令行里的 URL 写进临时目录的 `tinynext.inbox`（每行一个 URL）；
  2. Windows 上尝试 `SetForegroundWindow` 把已有窗口切到前台；
  3. 立即退出（`exit 0`，不闪窗口）。
- 主实例每 ~0.5s 轮询 inbox，把新 URL 逐个加入下载列表（URL 校验与「添加下载」
  弹窗一致：仅 HTTPS，`http://` 自动升级）。

## 实现位置

- `src/cli.cppm`（模块 `tinynext.cli`）：
  - `commandLineUrls()` — 解析命令行里的 URL 参数（Windows `CommandLineToArgvW`，
    macOS `_NSGetArgc/Argv`，Linux `/proc/self/cmdline`）；
  - `acquireSingleInstance()` — Windows 命名互斥体 / POSIX `flock`；
  - `forwardToRunningInstance()` — 写 inbox + 聚焦窗口；
  - `drainInbox()` — 读并清空 inbox；
  - `CliBoot` — 静态初始化：第二实例转发后 `exit(0)`；
  - `handleCliAndInbox(dt)` — 每帧调用：主实例首帧加自身 CLI URL，之后轮询 inbox。
- inbox 文件：`<temp>/tinynext.inbox`（`std::filesystem::temp_directory_path()`）。

## 排查

- 转发没生效？检查 `%TEMP%\tinynext.inbox`（Windows）是否出现了 URL。
- 加了 URL 但没下载？URL 必须 `https://`；文件名取自 URL 最后一段。
