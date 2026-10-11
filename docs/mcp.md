# TinyNext MCP Server（Model Context Protocol）

让 AI 助手（Claude Desktop / Claude Code / ZCode / Cursor 等）直接管理 TinyNext
的下载任务。`tinynext --mcp` 是一个 **stdio MCP server**（伴生进程，与 CLI /
浏览器扩展同一定位：绝不进 GUI，任务语义全部归运行中的 TinyNext 实例）。

## 接入配置

在 MCP 客户端的配置里加：

```json
{
  "mcpServers": {
    "tinynext": {
      "command": "/path/to/tinynext",
      "args": ["--mcp"]
    }
  }
}
```

- Claude Desktop：`Claude → Settings → Developer → Edit Config`（`claude_desktop_config.json`）。
- Claude Code：`claude mcp add tinynext -- /path/to/tinynext --mcp`。
- 其它客户端（ZCode / Cursor / 通用）：同样填 `command` + `args: ["--mcp"]`。

Linux 免安装包用仓库根 `run.sh` 作 command（工具链 glibc 兼容性，见 run.sh 注释）：
`{"command": "/path/to/run.sh", "args": ["--mcp"]}`。Windows/macOS 直接指向主程序。

## 工具（v1，9 个）

| 工具 | 说明 |
|---|---|
| `get_status` | 引擎健康（daemon/RPC/WebSocket）、任务计数、速度、下载目录 |
| `list_downloads(state?, query?)` | 列任务（id/状态/进度/速度/名称/URL/错误）；`state` ∈ all/active/done/failed；`query` 按名称/URL 子串过滤 |
| `add_download(url)` | 提交下载（http/https/ftp/sftp/magnet/ed2k）。应用未运行时**自动启动并进托盘**；每个任务的重命名/目录等高级选项请在界面里改 |
| `pause(ids \| all)` / `resume(ids \| all)` | 暂停/恢复；等待种子文件或媒体轨道选择的任务会被拒绝（去界面选） |
| `cancel(ids)` | 取消进行中/排队任务（保留部分文件，可 retry 续传） |
| `retry(ids)` | 重试失败/已取消任务（复用原 GID 与断点数据） |
| `remove(ids)` | 删除任务记录（不动磁盘文件） |
| `wait_until_idle(timeout_seconds?)` | 阻塞等待无排队/进行中任务（默认 60s 上限 3600），配合 add_download 等"下载完成" |

不提供 `quit`（AI 不应随手关闭应用）。

## 语义与限制

- **应用未运行**：只读工具返回 "TinyNext is not running" 错误；`add_download`
  会落队并自动启动应用（首帧后直接进托盘，见 docs/cli.md）。
- **托盘待机**：窗口隐藏时只读工具正常工作；**变更类工具（pause/resume/…）
  会等待 10s 后返回 "hidden to the tray" 错误**——需要先从托盘菜单「显示」唤回
  窗口（控制面既有限制，agent 帮助有说明）。
- 每个工具调用独立走单实例 TCP 控制面（`TINYNEXT-CLI/2`，见 docs/cli.md），
  与 CLI 完全同一路径；MCP server 进程本身无状态、不持锁、不托管引擎。

## 实现

- `src/mcp_server.cppm`（`tinynext.mcp_server`）——stdio 上的 NDJSON JSON-RPC
  2.0 循环（`initialize`/`tools/list`/`tools/call`/`ping`），工具调用翻译成
  控制面动词；`add_download` 经 URL 转发路径（同 `tinynext <url>`）。
- `src/cli.cppm` — `CliBoot` 的 `--mcp` 分支：注入 ctl 回调（补 `ctl:` 线路
  前缀）与 URL 转发回调。
- 调研参考：[Aria2 Agent](https://github.com/Rehui-2006/aria2-agent)（Python，
  直连 aria2 RPC + 安全规则）、[aria2-mcp-rs](https://github.com/nicholaswilde/aria2-mcp-rs)
  （Rust，通用 aria2 RPC，17 工具 + HTTP/SSE）。两者都绕开应用层，因此需要
  自行补任务语义/沙箱；TinyNext 的 MCP 走 TaskStore/控制面天然继承。HTTP 传输、
  MCP resources、实时通知列 V2（见 roadmap）。
