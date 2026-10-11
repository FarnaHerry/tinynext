// TinyNext 下载器浏览器扩展 — MV3 service worker。
//
// 链接右键菜单 + 浏览器下载接管（popup 开关，默认关）→ 经 Chrome Native
// Messaging 发给 TinyNext 宿主（主程序 tinynext 以 chrome-extension:// 参数被
// Chrome 拉起时进入宿主模式，见 src/browser_host.cppm），宿主再经单实例
// TCP/inbox 通道转发给运行中的 TinyNext。
//
// 固定扩展 ID（manifest key 派生）：
// chrome-extension://ebhblmhndabomflakjjljpipfolmajia/
// 换 manifest 的 key 必须同步改宿主端 kExtensionOrigin 与已注册的宿主清单。

const HOST_NAME = 'com.tinynext.host';
const MENU_ID = 'tinynext-download-link';
const HOST_TIMEOUT_MS = 5000;
const BADGE_RESET_MS = 2500;

// aria2 能接的源才转发；blob:/data:/about: 等浏览器内部地址跳过。
function isForwardable(url) {
  return /^(https?|ftp|magnet):/i.test(url || '');
}

function sendToHost(message) {
  return new Promise((resolve) => {
    let port;
    try {
      port = chrome.runtime.connectNative(HOST_NAME);
    } catch (e) {
      resolve({ ok: false, error: String(e) });
      return;
    }
    let settled = false;
    const finish = (value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve(value);
      try { port.disconnect(); } catch (e) { /* 已断开 */ }
    };
    const timer = setTimeout(
      () => finish({ ok: false, error: '宿主响应超时' }), HOST_TIMEOUT_MS);
    port.onMessage.addListener((msg) => {
      finish(msg && typeof msg === 'object'
        ? msg : { ok: false, error: '无效响应' });
    });
    port.onDisconnect.addListener(() => {
      const err = chrome.runtime.lastError;
      finish({
        ok: false,
        error: err && err.message
          ? err.message
          : '宿主已断开（请确认 TinyNext 启动过一次、扩展 ID 与注册清单一致）',
      });
    });
    port.postMessage(message);
  });
}

function showBadge(ok, text) {
  chrome.action.setBadgeBackgroundColor({ color: ok ? '#1a7f37' : '#8b1d1d' })
    .then(() => chrome.action.setBadgeText({ text }))
    .then(() => setTimeout(() => chrome.action.setBadgeText({ text: '' }),
                           BADGE_RESET_MS))
    .catch(() => {});
}

async function forward(url, { silent } = {}) {
  if (!isForwardable(url)) {
    if (!silent) showBadge(false, '!');
    return { ok: false, error: '不支持的链接' };
  }
  const reply = await sendToHost({ type: 'download', url });
  if (!silent) showBadge(reply.ok === true, reply.ok === true ? '✓' : '!');
  return reply;
}

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({
    id: MENU_ID,
    title: '用 TinyNext 下载此链接',
    contexts: ['link'],
  }, () => void chrome.runtime.lastError);
});

chrome.contextMenus.onClicked.addListener((info) => {
  if (info.menuItemId === MENU_ID && info.linkUrl) {
    void forward(info.linkUrl);
  }
});

// 浏览器下载接管：chrome.storage.sync 的 takeover 开关（popup 控制，默认关）。
// blob:/data: 无法交给 aria2；已完成/秒完成的下载 cancel 会失败，静默放行。
chrome.downloads.onCreated.addListener((item) => {
  (async () => {
    const { takeover } = await chrome.storage.sync.get({ takeover: false });
    if (!takeover || !item || !isForwardable(item.url)) return;
    try {
      await chrome.downloads.cancel(item.id);
    } catch (e) {
      return;  // 取消失败（已完成）：保留浏览器下载，不打扰
    }
    const reply = await forward(item.url, { silent: true });
    showBadge(reply.ok === true, reply.ok === true ? '✓' : '!');
  })();
});
