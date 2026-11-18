// 宿主协议类型：与 src/browser_host.cppm 的 stdio JSON 契约一一对应，改宿主
// 端字段必须同步这里。
//
//   请求：{"type":"ping"} / {"type":"download","url":"https://…","mirrors":["…"]}
//   回复：{"ok":true,"app":"TinyNext","version":"0.7.16"}（ping）
//        {"ok":true}（已转发给运行中的 TinyNext）
//        {"ok":true,"queued":true}（TinyNext 未运行，已落 inbox 并拉起 GUI）
//        {"ok":false,"error":"…"}
export const HOST_NAME = 'com.tinynext.host';
// 扩展固定 ID（manifest.json 的 key 派生）。宿主清单 allowed_origins 按它写死，
// 换 manifest key 必须同步 src/browser_host.cppm 的 kExtensionOrigin。
export const EXTENSION_ID = 'ebhblmhndabomflakjjljpipfolmajia';
// 仅把协议头（含 ed2k://|file| 前缀）归一为小写，不动后续路径/文件名/哈希的大小写，
// 对齐 C++ 侧 utils::isDownloadableSource 的前缀白名单。
export function normalizeDownloadUrl(raw) {
    const trimmed = (raw ?? '').trim();
    if (!trimmed)
        return '';
    if (/^ed2k:\/\/\|file\|/i.test(trimmed)) {
        return 'ed2k://|file|' + trimmed.slice('ed2k://|file|'.length);
    }
    const m = /^(https?:\/\/|sftp:\/\/|magnet:)/i.exec(trimmed);
    if (m) {
        return m[1].toLowerCase() + trimmed.slice(m[1].length);
    }
    return trimmed;
}
// 与 utils::isDownloadableSource 严格一致：http(s) / sftp / magnet / ed2k://|file|。
// aria2-next 2.8.x 不支持 ftp://，这里同步排除以免误拦截浏览器下载。
export function isForwardable(url) {
    const normalized = normalizeDownloadUrl(url);
    return (normalized.startsWith('http://') ||
        normalized.startsWith('https://') ||
        normalized.startsWith('sftp://') ||
        normalized.startsWith('magnet:') ||
        normalized.startsWith('ed2k://|file|'));
}
