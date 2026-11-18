// popup：接管开关 + 手动投递链接 + 宿主连接检测。
import { forward, sendToHost } from './messaging.js';
const toggle = document.getElementById('takeover');
const urlInput = document.getElementById('manual-url');
const sendBtn = document.getElementById('send');
const status = document.getElementById('status');
const testBtn = document.getElementById('test');
chrome.storage.sync.get({ takeover: false }, ({ takeover }) => {
    toggle.checked = takeover === true;
});
toggle.addEventListener('change', () => {
    void chrome.storage.sync.set({ takeover: toggle.checked });
});
function setStatus(kind, text) {
    status.className = kind;
    status.textContent = text;
}
async function submitManualUrl() {
    const raw = urlInput.value.trim();
    if (!raw) {
        setStatus('bad', '请输入下载链接（http/https/sftp/magnet/ed2k）');
        urlInput.focus();
        return;
    }
    sendBtn.disabled = true;
    setStatus('', '发送中…');
    try {
        const reply = await forward(raw);
        if (reply.ok) {
            urlInput.value = '';
            setStatus('ok', reply.queued
                ? '已入队并后台拉起 TinyNext ✓'
                : '已转发给 TinyNext ✓');
        }
        else {
            setStatus('bad', `发送失败：${reply.error ?? '未知错误'}`);
        }
    }
    finally {
        sendBtn.disabled = false;
    }
}
sendBtn.addEventListener('click', () => {
    void submitManualUrl();
});
urlInput.addEventListener('keydown', (ev) => {
    if (ev.key === 'Enter') {
        ev.preventDefault();
        void submitManualUrl();
    }
});
testBtn.addEventListener('click', () => {
    testBtn.disabled = true;
    setStatus('', '检测中…');
    void (async () => {
        try {
            const reply = await sendToHost({ type: 'ping' });
            if (reply.ok) {
                setStatus('ok', `已连接 TinyNext ${reply.version ?? ''} ✓`);
            }
            else {
                setStatus('bad', `未连接：${reply.error ?? '未知错误'}`);
            }
        }
        finally {
            testBtn.disabled = false;
        }
    })();
});
