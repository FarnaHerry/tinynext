# aria2 引擎约束

修改下载行为、daemon、RPC、WebSocket 或引擎并发时阅读本文。
唯一下载引擎为 aria2-next；TinyHttpsEngine 已移除。

## 下载选项、磁力与恢复

**每任务选项**：`dl::StartOptions{connections, outputName, dirOverride, limitBps}`，
`Aria2Engine` 全部生效（connections/limitBps 需 >0）。注意 aria2-next **没有下载级
priority 选项**（实测 + `--help=#all` 确认），优先级功能已移除，别再加回去。

**磁力**：`g_tasks.startFromUrl` 接受 `magnet:` 前缀；magnet 任务不设 `out`，
destPath 由 `refreshStates` 从 `files[0].path` 更新为真实路径。

**重新下载**：Failed/Cancelled 卡片 ↻ 调 `g_tasks.retry(id)`（委托 `DownloadEngine`
接口）。aria2-next 复用原 GID + URL + 输出路径，并从 `aria2-state/stream/state.db`
恢复已提交分片；旧版旁置 `.aria2` 文件不导入。

**会话恢复**：aria2 daemon 启动带 `--save-session`/`--input-file`（
`aria2_engine.cpp::daemonExtraOpts`），`shutdown()` 先 `aria2.saveSession` 再
forceShutdown；重启后 `recoverSession()` 用 `tellActive/tellWaiting/tellStopped`
重建任务表。

## 通信、线程与锁

**aria2 引擎**（`aria2_engine.cpp`）：进程名 Windows 是 `aria2-next.exe`，unix
是 `aria2-next`；字段名用 `connections`（不是 `numConnections`）。RPC 只监听
127.0.0.1 + 随机 `--rpc-secret`。**线程模型**：JSON-RPC 请求走 UI 线程
（`rpcCall`，HTTP over 本地 socket）；WebSocket 只收 aria2 的 6 个推送事件
（onDownloadStart/Pause/Stop/Complete/Error/BtDownloadComplete，无进度事件）
驱动状态迁移，进度靠 ~1s 轮询补；WS 回调跑在 IXWebSocket 内部线程，所以
`tasks_` 一律经 `tasksMutex_` 访问；WS 掉线自动回退纯轮询。**锁纪律**：持锁
方法 snapshot/start/action/recoverSession/handleWsEvent；`ensureDaemon`/
`applyTellStatus` 不持锁；start/retry 先 `ensureDaemon` 再取锁（避免非递归
互斥量死锁）。**Winsock**：Windows 本地 socket 需 `WSAStartup`，由
`LocalSocket::platformInit()`（POSIX no-op）在 `Aria2Engine` 构造/析构里
配对，不要漏。

`app.cpp` 的一次性 `warmup()` 预热线程与 UI 线程的 start/retry
经 `daemonMutex_` 互斥，锁序为 `daemonMutex_` → `tasksMutex_`。
原生资源及 worker 的退出顺序同时遵守
[资源生命周期约束](resource-lifecycle.md)。决策与验证见 [roadmap](roadmap.md)。
