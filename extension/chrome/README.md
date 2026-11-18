# TinyNext 浏览器扩展（Chrome / Edge 等 Chromium 系）

把网页链接与浏览器下载转发给 TinyNext。链路：

```
浏览器扩展（MV3） --Native Messaging--> tinynext 宿主模式 --TCP/inbox--> 运行中的 TinyNext
```

## 功能

- 链接右键菜单「用 TinyNext 下载此链接」与选中文本右键菜单「用 TinyNext 下载选中链接」
  （支持 `http://`、`https://`、`sftp://`、`magnet:`、`ed2k://|file|`）。
- 工具栏 popup：
  - 「接管浏览器下载」开关（默认关）：开启后先暂停新建下载并转发给 TinyNext，确认宿主受理后自动取消并清理浏览器下载栏残留记录；若宿主未注册或不可达则自动恢复浏览器原下载，防止一次性签名链接丢失。
  - 手动投递输入框：直接粘贴磁力 / ED2K / HTTP(S) / SFTP 链接发送给 TinyNext。
  - 「检测」按钮：验证 Native Messaging 宿主是否连通（显示 TinyNext 版本）。
- 转发结果用扩展图标角标反馈（绿 ✓ / 红 !）。

## 安装（加载已解压的扩展）

1. **启动一次 TinyNext**（GUI）。启动时会自动写宿主清单
   （`<configDir>/browser-host/com.tinynext.host.json`）并登记到浏览器：
   - Windows：`HKCU\Software\Google\Chrome\...` 与 `HKCU\Software\Microsoft\Edge\...`（若本机装有 Brave / Chromium 也会同步登记）
   - Linux：`~/.config/google-chrome/NativeMessagingHosts/`（及已存在的 `microsoft-edge` / `chromium` / `Brave-Browser`）
   - macOS：`~/Library/Application Support/Google/Chrome/NativeMessagingHosts/`（及已存在的 `Microsoft Edge` / `Chromium` / `Brave-Browser`）
2. 浏览器打开 `chrome://extensions/`（或 `edge://extensions/`）→ 开启「开发者模式」→
   「加载已解压的扩展程序」→ 选择本目录（`extension/chrome/`）。
3. 固定扩展 ID：`ebhblmhndabomflakjjljpipfolmajia`
   （manifest.json 的 `key` 派生，宿主白名单已按它写死——加载后 ID 应与之
   一致；若不一致说明 key 被改动，需同步 `src/browser_host.cppm` 的
   `kExtensionOrigin` 并重新注册）。
4. 扩展 popup 点「检测」，显示 `已连接 TinyNext x.y.z ✓` 即打通。

## 注意

- TinyNext 没在运行时转发：URL 会先落 inbox，宿主顺带以 `--start-hidden` 拉起 TinyNext 进托盘并自动开始下载。
- 扩展 ID 与宿主白名单绑定（`allowed_origins`）。以后发布到应用商店会换 ID，届时需在宿主清单中追加商店的 origin。
- Firefox 使用不同的 Native Messaging 清单格式（`allowed_extensions`），暂未支持。

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
