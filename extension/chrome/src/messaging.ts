// Native Messaging 客户端：按需 connectNative → postMessage → 收回包 → 断开。
// Chrome 会为每个端口拉起一次宿主进程，断开后进程退出。
import {
  HOST_NAME,
  isForwardable,
  type HostReply,
  type HostRequest,
} from './protocol.js';

export function sendToHost(message: HostRequest): Promise<HostReply> {
  return new Promise((resolve) => {
    let port: chrome.runtime.Port;
    try {
      port = chrome.runtime.connectNative(HOST_NAME);
    } catch (e) {
      resolve({ ok: false, error: String(e) });
      return;
    }
    let settled = false;
    const finish = (value: HostReply): void => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve(value);
      try {
        port.disconnect();
      } catch {
        // 已断开
      }
    };
    // 宿主长消息循环里某一步卡死时不无限等，5s 放弃。
    const timer = setTimeout(
      () => finish({ ok: false, error: '宿主响应超时' }),
      5000,
    );
    port.onMessage.addListener((msg: unknown) => {
      finish(
        msg && typeof msg === 'object'
          ? (msg as HostReply)
          : { ok: false, error: '无效响应' },
      );
    });
    port.onDisconnect.addListener(() => {
      const err = chrome.runtime.lastError;
      finish({
        ok: false,
        error:
          err && err.message
            ? err.message
            : '宿主已断开（请确认 TinyNext 启动过一次、扩展 ID 与注册清单一致）',
      });
    });
    port.postMessage(message);
  });
}

// 角标反馈（绿 ✓ / 红 !），2.5s 后自动清除。
export function showBadge(ok: boolean, text: string): void {
  chrome.action
    .setBadgeBackgroundColor({ color: ok ? '#1a7f37' : '#8b1d1d' })
    .then(() => chrome.action.setBadgeText({ text }))
    .then(() => setTimeout(() => chrome.action.setBadgeText({ text: '' }), 2500))
    .catch(() => {});
}

// 转发一条 URL 给 TinyNext，按需用角标反馈结果。
export async function forward(
  url: string,
  { silent }: { silent?: boolean } = {},
): Promise<HostReply> {
  if (!isForwardable(url)) {
    if (!silent) showBadge(false, '!');
    return { ok: false, error: '不支持的链接' };
  }
  const reply = await sendToHost({ type: 'download', url });
  if (!silent) showBadge(reply.ok, reply.ok ? '✓' : '!');
  return reply;
}
