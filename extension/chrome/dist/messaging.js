// Native Messaging 客户端：按需 connectNative → postMessage → 收回包 → 断开。
// 浏览器会为每个端口拉起一次宿主进程，断开后进程退出。
import { HOST_NAME, isForwardable, normalizeDownloadUrl, } from './protocol.js';
let badgeTimer;
export function sendToHost(message) {
    return new Promise((resolve) => {
        let port;
        try {
            port = chrome.runtime.connectNative(HOST_NAME);
        }
        catch (e) {
            resolve({ ok: false, error: String(e) });
            return;
        }
        let settled = false;
        const finish = (value) => {
            if (settled)
                return;
            settled = true;
            clearTimeout(timer);
            resolve(value);
            try {
                port.disconnect();
            }
            catch {
                // 已断开
            }
        };
        // 宿主长消息循环里某一步卡死时不无限等，5s 放弃。
        const timer = setTimeout(() => finish({ ok: false, error: '宿主响应超时' }), 5000);
        port.onMessage.addListener((msg) => {
            finish(msg && typeof msg === 'object'
                ? msg
                : { ok: false, error: '无效响应' });
        });
        port.onDisconnect.addListener(() => {
            const err = chrome.runtime.lastError;
            finish({
                ok: false,
                error: err && err.message
                    ? err.message
                    : '宿主已断开（请确认已启动过一次 TinyNext，且扩展 ID 与注册清单一致）',
            });
        });
        port.postMessage(message);
    });
}
// 角标反馈（绿 ✓ / 红 !），2.5s 后自动清除（防抖覆盖前一次计时器）。
export function showBadge(ok, text) {
    if (badgeTimer !== undefined) {
        clearTimeout(badgeTimer);
        badgeTimer = undefined;
    }
    chrome.action
        .setBadgeBackgroundColor({ color: ok ? '#1a7f37' : '#8b1d1d' })
        .then(() => chrome.action.setBadgeText({ text }))
        .then(() => {
        badgeTimer = setTimeout(() => {
            badgeTimer = undefined;
            void chrome.action.setBadgeText({ text: '' }).catch(() => { });
        }, 2500);
    })
        .catch(() => { });
}
// 转发一条 URL 给 TinyNext，按需用角标反馈结果。
export async function forward(url, { silent, mirrors } = {}) {
    const normalized = normalizeDownloadUrl(url);
    if (!isForwardable(normalized)) {
        if (!silent)
            showBadge(false, '!');
        return {
            ok: false,
            error: '不支持的链接（支持 http/https/sftp/magnet/ed2k://|file|）',
        };
    }
    const req = mirrors && mirrors.length > 0
        ? { type: 'download', url: normalized, mirrors }
        : { type: 'download', url: normalized };
    const reply = await sendToHost(req);
    if (!silent)
        showBadge(reply.ok, reply.ok ? '✓' : '!');
    return reply;
}
