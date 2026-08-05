# mcpp 包方案：`compat:websocket`（包装 IXWebSocket）

> 目标：给 mcpp 生态提供一个 WebSocket 客户端包（`compat:websocket`），
> 顺带支撑 TinyNext 未来用 aria2 的 WebSocket RPC 推送替代轮询。
> **决定：包装现成库，不自己实现 RFC 6455。** 本文件是独立包的外部计划，
> 包仓库与本仓库解耦；落地后按「TinyNext 接入」一节接入，未落地前保持轮询。

## 背景（已核实）

- mcpp 索引**没有 websocket 包**（`mcpp search websocket` / `xlings search websocket` 均空）。
- 相关网络包现状：`mcpplibs:tinyhttps`（HTTP，我们已移除）、`compat:curl`（带 WS，但引入大依赖+TLS）、
  `chriskohlhoff:asio`（standalone asio，模块形态）、`compat:mbedtls`/`compat:openssl`（TLS）。
- mcpp compat 包 = Lua 配方（如 `pkgs/e/compat.eui-neo.lua`）：声明上游 URL + sha256 + 构建配置，
  mcpp 拉源码按平台编译，头文件经 `include_dirs` 暴露。本方案照此格式。

## 选型：IXWebSocket

上游：<https://github.com/machinezone/IXWebSocket>（MIT）

| 理由 |
|------|
| 纯 WebSocket 库，包名诚实 |
| MIT，可自由包装 |
| 跨平台（win/linux/macos），零外部硬依赖（TLS 可编译关掉，local 场景不需要） |
| C++11，源码量级与 eui-neo 相当，mcpp 配方好写 |
| 客户端 + 服务端都有，生态用户可选 |

备选（不推荐，记录原因）：
- `cpp-httplib`（单头，套包最简，但本质是 HTTP 库，包装成 websocket 名不副实）
- `websocketpp`（成熟 header-only，但依赖 asio；mcpp 里的 asio 是模块形态，经典用法对接有坑）
- `libwebsockets`（最稳、Chromium 重度使用，但源码量大、平台宏多、配方最复杂）

## 包结构（仿 `compat.eui-neo`）

```
pkgs/w/compat.websocket.lua
├── package = { namespace="compat", name="websocket", licenses={"MIT"},
│              repo="https://github.com/machinezone/IXWebSocket",
│              xpm = { linux/macosx/windows 各版本的 url(GLOBAL+CN) + sha256 } }
└── mcpp 段
    ├── language = "c++17", import_std = false, header-compat Form B
    ├── 源文件 glob：上游解包后 ixwebsocket/ 下的核心客户端 TU
    │   （WebSocket / WebSocketHandshake / WebSocketFrame / WebSocketMessage /
    │    WebSocketTransport / Socket / Http / Base64 / SHA1 / IXNetSystem /
    │    IXSelectInterrupt* / IXSocketConnect 等，**排除 server、TLS/OpenSSLObject**）
    ├── 不定义 IXWEBSOCKET_USE_TLS → 不拉 OpenSSL
    └── include_dirs 暴露 ixwebsocket/*.h
```

> 精确源文件 glob 以上游实际目录为准，配好本地编译过为准。
> 可选增强：包内附一个 **C++23 薄封装头** `websocket.hpp`（`std::span`/`std::format`/RAII，
> 底层走 IXWebSocket），给用户更现代观感——不强制，先出库再说。

## 本地验证

1. `mcpp` 三平台编译通过（win/linux/macos）。
2. 最小 consumer 程序：
   - 连 `ws://127.0.0.1:<port>/jsonrpc`（aria2 开 `--enable-rpc-websocket`）或一个 WS echo server；
   - 收发 Text / Binary / Ping-Pong 帧；
   - 验证握手 `Sec-WebSocket-Accept`、客户端掩码、分片、关闭握手。
3. 把 consumer 作为包内示例或单独测试仓库。

## 提交贡献

把 `compat.websocket.lua` 配方 PR 到 mcpp 索引仓库（mcpplibs / xim），
写法参考 `pkgs/e/compat.eui-neo.lua`（含 CN mirror 惯例：GLOBAL+gitcode 双 url）。

## TinyNext 接入（已完成，2026-08）

**实际核实修正**：aria2-next **没有 `--enable-rpc-websocket` flag**（HTTP 层检测
`Connection: upgrade` 自动升级），daemon 参数无需改动；且**没有 `onDownloadProgress`
推送事件**，只有 start/pause/stop/complete/error/btComplete 六个（参数只带 gid），
所以进度仍需轮询。

**采用的混合方案**：WS 连接只收推送事件（状态迁移/完成通知即时），请求-响应继续走
现有 HTTP `rpcCall`（已验证、可回退）。

- `aria2_engine.cpp`：新增 `WsNotifier`（包装 `ix::WebSocket`，连
  `ws://127.0.0.1:<port>/jsonrpc`），`ensureDaemon` 就绪后启动；`handleWsEvent` 按
  gid 更新任务状态（complete/error 置 `needsFinalize`，下一轮 poll 补一次 tellStatus
  拿最终字节 / 磁力真实路径 / errorMessage）。
- **并发改造**：推送在 IXWebSocket 后台线程到达 → 给 `tasks_` 加 `tasksMutex_`
  （单一互斥量）；锁纪律见 roadmap。进度轮询从 ~5Hz 降到 1s。
