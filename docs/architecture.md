# 模块与状态归属

修改模块边界、共享状态或页面职责前阅读本文。

| 模块 | 文件 | 职责 |
|------|------|------|
| `tinynext.download_engine` | `src/download_engine.cppm` | 引擎接口 `dl::DownloadEngine` |
| `tinynext.aria2_engine` | `src/aria2_engine.cppm/.cpp` | aria2-next 引擎（JSON-RPC + 本地 socket） |
| `tinynext.config` | `src/config.cppm` | 配置 / 主题 / 下载目录 |
| `tinynext.utils` | `src/utils.cppm` | 纯 string/number 帮助函数 + `commandLineArgs()`（无 UI 依赖） |
| `tinynext.store.tasks` | `src/store/tasks.cppm` | 领域 store：`TaskStore` + `g_tasks`（引擎 + 任务命令 + startFromUrl） |
| `tinynext.store.ui` | `src/store/ui.cppm` | 视图 store：状态消息 / 页面 / 筛选·排序·分页 |
| `tinynext.store.dialogs` | `src/store/dialogs.cppm` | 视图 store：弹窗状态机 + addDownload/requestDelete |
| `tinynext.cli` | `src/cli.cppm` | 单实例 + 命令行 URL + TCP socket 转发 + 控制面请求入口 |
| `tinynext.browser_host` | `src/browser_host.cppm` | Native Messaging 宿主与清单注册 |
| `tinynext.cli_control` | `src/cli_control.cppm` | CLI 只读查询与写命令分派（支持 `--json`） |
| `tinynext.ui.*` | `src/ui/*.cppm` | utils（布局常量）/ theme / platform / housekeep / widgets / cards / downloads_page / settings_page / engine_page |
| `src/app.cpp` | 普通 TU | 入口：`app::dslAppConfig()` + `app::compose()` |

页面已按职责拆成独立模块（`pages.cppm` 已删除）：
`downloads_page`（下载页 + 添加下载弹窗）、`settings_page`（设置页，含关于和引擎监控标签）、
`engine_page`（设置页的引擎监控标签内容）。顶级导航已移除：分页器末尾齿轮打开设置、
拼图打开扩展页（浏览器扩展 + MCP），页面右上角关闭按钮返回下载页。

## 状态分层

**共享状态（store 分层）**：状态按领域/视图分层——`tinynext.store.tasks`
（`TaskStore` + `g_tasks`：引擎、任务命令、`startFromUrl` 返回 `StartResult{ok,
message}` 不做 UI 提示）是领域 store，不 import 任何 ui.*/eui；`store.ui` /
`store.dialogs` 是视图 store（状态消息、筛选分页、弹窗草稿）；设置页 pending
草稿是 `settings_page.cppm` 模块私有；主题全局在 `tinynext.ui.theme`。旧
`tinynext.ui.state` 已删除，新状态先想清楚属于哪一层。

## CLI 与扩展入口

`browser_host` 在 Chrome 拉起主程序时运行 stdio 协议循环，并注册
`extension/chrome/` 所需的宿主清单。`cli_control` 的 `status/list/watch`
由 IPC 线程直答；`pause/resume/cancel/retry/remove/clear/quit` marshal 到 UI
线程执行，支持 `--json`。协议与使用方式见 [CLI 文档](cli.md)。
