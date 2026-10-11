// popup：接管开关（chrome.storage.sync，与 background 共享）+ 宿主连接检测。
import { sendToHost } from './messaging.js';

const toggle = document.getElementById('takeover') as HTMLInputElement;
const status = document.getElementById('status') as HTMLElement;
const testBtn = document.getElementById('test') as HTMLButtonElement;

chrome.storage.sync.get({ takeover: false }, ({ takeover }) => {
  toggle.checked = takeover === true;
});

toggle.addEventListener('change', () => {
  void chrome.storage.sync.set({ takeover: toggle.checked });
});

function setStatus(kind: '' | 'ok' | 'bad', text: string): void {
  status.className = kind;
  status.textContent = text;
}

testBtn.addEventListener('click', () => {
  setStatus('', '检测中…');
  void (async () => {
    const reply = await sendToHost({ type: 'ping' });
    if (reply.ok) {
      setStatus('ok', `已连接 TinyNext ${reply.version ?? ''} ✓`);
    } else {
      setStatus('bad', `未连接：${reply.error ?? '未知错误'}`);
    }
  })();
});
