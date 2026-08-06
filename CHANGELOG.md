# Changelog

## 0.1.0（Unreleased / 开发中）

### 下载引擎：纯 aria2-next
- 移除内置 tinyhttps 引擎（`TinyHttpsEngine` / `tinynext.download_manager` 模块），**aria2-next 成为唯一下载引擎**。
- aria2 本地 JSON-RPC 改用自写的极简跨平台 `LocalSocket`（winsock / POSIX，非阻塞 connect 超时），`mcpp.toml` 去掉 `tinyhttps` 依赖。
- `http://` 不再强制升级为 `https://`（那是 tinyhttps 只支持 HTTPS 的限制；aria2 原生支持 http）。
- 设置页去掉「下载引擎」切换；`config.cppm` 去掉 `EngineChoice` / `engine()`。
- eui-neo 固定 **0.5.3**（0.5.5 与 C++23 构建不兼容，见 `docs/roadmap.md`）。
- Windows 链接补 `-lws2_32`（`LocalSocket` 用 winsock）。

### 下载功能
- **磁力 / BT**：接受 `magnet:` 链接（仅 aria2 引擎）；magnet 任务不设 `out`，元数据落地后 `files[0].path` 显示真实路径。
- **断点续传**：失败 / 已取消卡片 ↻「重新下载」→ aria2 从同目录 `.aria2` 控制文件续传（`retry()` 复用原 URL+路径）。
- **重启会话恢复**：daemon 启动带 `--save-session` / `--input-file`，退出前 `aria2.saveSession`；重启后 `recoverSession()` 用 `tellActive/tellWaiting/tellStopped` 重建任务表。
- **下载完成 / 失败系统通知**：任务从进行中迁移到 Done/Failed 时弹系统通知（Windows PowerShell 气泡 / macOS osascript / Linux notify-send→kdialog，best-effort）。
- **WebSocket 事件推送**（新增 `compat:websocket` / IXWebSocket 依赖）：aria2 的
  `onDownloadStart/Complete/Error/Pause/Stop/BtDownloadComplete` 推送**即时**驱动状态
  迁移与完成/失败通知；进度轮询从 ~5Hz 降到 **1s**（对齐 Motrix/AriaNg）。引擎加
  `tasksMutex_` 让 WS 后台线程安全更新任务。核实：aria2-next **无**
  `--enable-rpc-websocket` flag（HTTP 层检测 `Connection: upgrade` 自动升级）、也**无**
  进度推送事件，故保留轮询补进度；WS 掉线自动回退轮询。
- 卡片显示 **ETA 剩余时间**；排序新增「优先级」。

### 配置与每任务选项
- aria2 配置扩展：**代理**（HTTP/HTTPS，aria2 不支持 SOCKS5）、**重试次数 / 等待秒**、**最大同时下载数**（队列并发，默认 5）、**完成后移除控制文件**、**完成后命令**、**User-Agent / Referer / 磁盘缓存**。
- 设置页正文改**可滚动**；aria2 参数在 daemon 已启动时提示「重启后生效」。
- 添加弹窗每任务选项：**连接数 / 优先级 / 重命名 / 限速KB/s / 下载目录**；打开时**默认填配置的连接数与下载目录**。
- 优先级选择器索引 → aria2 `priority` 数值的映射集中在 `state::priorityValueFromPicker`（方向待随包二进制验证，若反只改一处）。

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
- **分片数 / 优先级分行**：左标签"连接数"改"分片数"（实际是每任务 split）；优先级独立
  一行（默认/高/中/低）。
- 优先级下拉修复：标签 id 与 picker 内部撞名导致不显示、展开后点击外部不收起（加全屏
  拦截层）、排序下拉弹层宽度不足文字溢出（加 `popupWidth`）。

### 修复
- **Windows 打开文件/文件夹非阻塞**：`openFile` / `openContainingFolder` 从
  `std::system("explorer …")` 改 `ShellExecuteW`（共享 `shellExecFn()`），不再等 Explorer
  窗口关闭卡 UI 线程。
- 设置页：移除顶部副标题提示；标题下移、表单首行加顶部占位，避免被滚动区上缘裁掉。

### 底层能力（较早实现）
- **单实例** + **CLI 传参下载**：重复启动把 URL 写入 `<temp>/tinynext.inbox` 由主实例轮询取走；`tinynext agent` 打印 AI 用法指南。
- EUI-NEO 前端全模块化（`tinynext.ui.*`），`kUI=1.4` 统一缩放。
- 深色 / 浅色 / 跟随系统主题，~2s 轮询 OS 主题。

---

规划与决策记录见 `docs/roadmap.md`；AI 助手指南见 `AGENTS.md`。
