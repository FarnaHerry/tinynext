# TinyNext 浏览器扩展（Google Chrome）

把网页链接与浏览器下载转发给 TinyNext。链路：

```
Chrome 扩展（MV3） --Native Messaging--> tinynext 宿主模式 --TCP/inbox--> 运行中的 TinyNext
```

## 功能

- 链接右键菜单「用 TinyNext 下载此链接」（http/https/ftp/magnet）。
- 工具栏 popup：
  - 「接管浏览器下载」开关（默认关）：开启后浏览器开始下载文件时自动取消，
    改由 TinyNext 下载（`blob:`/`data:` 等浏览器内部地址除外；已秒完成的下载
    无法取消，保留浏览器下载）。
  - 「检测」按钮：验证 Native Messaging 宿主是否连通（显示 TinyNext 版本）。
- 转发结果用扩展图标角标反馈（绿 ✓ / 红 !）。

## 安装（开发期：加载已解压的扩展）

1. **启动一次 TinyNext**（GUI）。启动时会自动写宿主清单
   （`<configDir>/browser-host/com.tinynext.host.json`）并登记到 Chrome：
   - Windows：`HKCU\Software\Google\Chrome\NativeMessagingHosts\com.tinynext.host`
   - Linux：`~/.config/google-chrome/NativeMessagingHosts/com.tinynext.host.json`
   - macOS：`~/Library/Application Support/Google/Chrome/NativeMessagingHosts/`
2. Chrome 打开 `chrome://extensions/` → 右上角开启「开发者模式」→
   「加载已解压的扩展程序」→ 选择本目录（`extension/chrome/`）。
3. 固定扩展 ID：`ebhblmhndabomflakjjljpipfolmajia`
   （manifest.json 的 `key` 派生，宿主白名单已按它写死——加载后 ID 应与之
   一致；若不一致说明 key 被改动，需同步 `src/browser_host.cppm` 的
   `kExtensionOrigin` 并重新注册）。
4. 扩展 popup 点「检测」，显示 `已连接 TinyNext x.y.z ✓` 即打通。

## 注意

- TinyNext 没在运行时转发：URL 会先落 inbox，宿主顺带拉起 TinyNext（首次
  GUI 窗口出现后自动添加）。
- 扩展 ID 与宿主白名单绑定（`allowed_origins`）。以后发布到 Chrome Web
  Store 会换 ID，届时需在宿主清单中追加 store 的 origin。
- 其它 Chromium 系浏览器（Edge/Brave 等）注册路径不同，暂未支持；Firefox
  使用不同的 Native Messaging 端口协议，暂不支持。

## 源码与构建

源码是 TypeScript（`src/*.ts`），编译产物 `dist/*.js` **随仓库提交**——普通用户
「加载已解压」不需要任何 JS 工具链；只有改扩展源码时才需要构建：

```bash
cd extension/chrome
pnpm install    # typescript + @types/chrome（devDependencies）
pnpm build      # tsc → dist/；pnpm watch 边改边编
```

- 入口：`src/background.ts`（service worker）/ `src/popup.ts`（popup），
  共享逻辑在 `src/protocol.ts`（宿主协议类型）/ `src/messaging.ts`（客户端）。
- `package.json` 带 `"type": "module"`，import 一律写带 `.js` 的相对路径
  （浏览器 ESM 要求显式扩展名，tsc 按 bundler 解析回 .ts）。
- 没有网络时任何全局 `tsc`（≥5.0）也能编：`tsc -p extension/chrome`。

## 实现位置

- `src/browser_host.cppm` — 宿主 stdio 协议循环（Chrome 以
  `chrome-extension://<id>/` 参数拉起主程序时接管）+ 清单生成与注册。
- `src/cli.cppm` — `CliBoot` 的宿主分支 + `forwardUrlsFromBrowser`
  （复用单实例 TCP `TINYNEXT-CLI/2` 握手与 inbox 兜底）。
- `src/app.cpp` — GUI 启动时调用 `registerChromeNativeHost()`。
- 手动调试宿主：`tinynext --native-host`，然后按 Chrome Native Messaging
  协议在 stdin 上写 `u32 长度 + JSON`，例如
  `{"type":"ping"}` / `{"type":"download","url":"https://…"}`。
