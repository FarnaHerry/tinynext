// MV3 service worker：链接/选中文本右键菜单 + 安全浏览器下载接管（popup 开关，默认关）。
import { forward, showBadge } from './messaging.js';
import { isForwardable } from './protocol.js';
const MENU_LINK_ID = 'tinynext-download-link';
const MENU_SELECTION_ID = 'tinynext-download-selection';
function registerMenus() {
    chrome.contextMenus.removeAll(() => {
        void chrome.runtime.lastError;
        chrome.contextMenus.create({
            id: MENU_LINK_ID,
            title: '用 TinyNext 下载此链接',
            contexts: ['link'],
        }, () => void chrome.runtime.lastError);
        chrome.contextMenus.create({
            id: MENU_SELECTION_ID,
            title: '用 TinyNext 下载选中链接',
            contexts: ['selection'],
        }, () => void chrome.runtime.lastError);
    });
}
chrome.runtime.onInstalled.addListener(registerMenus);
chrome.runtime.onStartup.addListener(registerMenus);
chrome.contextMenus.onClicked.addListener((info) => {
    if (info.menuItemId === MENU_LINK_ID && info.linkUrl) {
        void forward(info.linkUrl);
        return;
    }
    if (info.menuItemId === MENU_SELECTION_ID && info.selectionText) {
        void forward(info.selectionText);
    }
});
// 浏览器下载接管：先暂停浏览器下载 → 转发给宿主确认成功后再取消并擦除浏览器记录；
// 若宿主未注册/不可达则自动恢复浏览器下载，绝不让一次性签名链接或文件两边落空。
chrome.downloads.onCreated.addListener((item) => {
    void (async () => {
        if (!item || item.state !== 'in_progress')
            return;
        const targetUrl = isForwardable(item.finalUrl) ? item.finalUrl : item.url;
        if (!isForwardable(targetUrl))
            return;
        const { takeover } = await chrome.storage.sync.get({ takeover: false });
        if (!takeover)
            return;
        let paused = false;
        try {
            await chrome.downloads.pause(item.id);
            paused = true;
        }
        catch {
            // 不支持暂停或已秒完成：下面用 search 核对状态
        }
        try {
            const [latest] = await chrome.downloads.search({ id: item.id });
            if (!latest || latest.state !== 'in_progress') {
                return; // 已在瞬间完成或已被用户取消，保持原状不重复下载
            }
        }
        catch {
            return;
        }
        const reply = await forward(targetUrl, { silent: true });
        if (reply.ok) {
            try {
                await chrome.downloads.cancel(item.id);
            }
            catch {
                // 已结束则忽略
            }
            try {
                await chrome.downloads.erase({ id: item.id });
            }
            catch {
                // 清理下载栏残留记录失败不影响主任务
            }
            showBadge(true, '✓');
        }
        else {
            if (paused) {
                try {
                    await chrome.downloads.resume(item.id);
                }
                catch {
                    // 尽力恢复浏览器原下载
                }
            }
            showBadge(false, '!');
        }
    })();
});
