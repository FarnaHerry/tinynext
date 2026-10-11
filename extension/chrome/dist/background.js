// MV3 service worker：链接右键菜单 + 浏览器下载接管（popup 开关，默认关）。
import { forward, showBadge } from './messaging.js';
import { isForwardable } from './protocol.js';
const MENU_ID = 'tinynext-download-link';
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
    void (async () => {
        const { takeover } = await chrome.storage.sync.get({ takeover: false });
        if (!takeover || !item || !isForwardable(item.url))
            return;
        try {
            await chrome.downloads.cancel(item.id);
        }
        catch {
            return; // 取消失败（已完成）：保留浏览器下载，不打扰
        }
        const reply = await forward(item.url, { silent: true });
        showBadge(reply.ok, reply.ok ? '✓' : '!');
    })();
});
