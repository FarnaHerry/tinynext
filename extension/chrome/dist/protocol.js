// 宿主协议类型：与 src/browser_host.cppm 的 stdio JSON 契约一一对应，改宿主
// 端字段必须同步这里。
//
//   请求：{"type":"ping"} / {"type":"download","url":"https://…"}
//   回复：{"ok":true,"version":"0.7.12"}（ping）
//        {"ok":true}（已转发给运行中的 TinyNext）
//        {"ok":true,"queued":true}（TinyNext 未运行，已落 inbox 并拉起 GUI）
//        {"ok":false,"error":"…"}
export const HOST_NAME = 'com.tinynext.host';
// 扩展固定 ID（manifest.json 的 key 派生）。宿主清单 allowed_origins 按它写死，
// 换 manifest key 必须同步 src/browser_host.cppm 的 kExtensionOrigin。
export const EXTENSION_ID = 'ebhblmhndabomflakjjljpipfolmajia';
// aria2 能接的源才转发；blob:/data:/about: 等浏览器内部地址跳过。
export function isForwardable(url) {
    return /^(https?|ftp|magnet):/i.test(url ?? '');
}
