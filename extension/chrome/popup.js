// popup：接管开关（chrome.storage.sync，与 background 共享）+ 宿主连接检测。
const toggle = document.getElementById('takeover');
const status = document.getElementById('status');
const testBtn = document.getElementById('test');

chrome.storage.sync.get({ takeover: false }, ({ takeover }) => {
  toggle.checked = takeover === true;
});

toggle.addEventListener('change', () => {
  chrome.storage.sync.set({ takeover: toggle.checked === true });
});

testBtn.addEventListener('click', () => {
  status.className = '';
  status.textContent = '检测中…';
  let port;
  try {
    port = chrome.runtime.connectNative('com.tinynext.host');
  } catch (e) {
    status.className = 'bad';
    status.textContent = '无法启动宿主：' + e;
    return;
  }
  const timer = setTimeout(() => {
    status.className = 'bad';
    status.textContent = '宿主响应超时';
    try { port.disconnect(); } catch (e) { /* 已断开 */ }
  }, 5000);
  port.onMessage.addListener((msg) => {
    clearTimeout(timer);
    try { port.disconnect(); } catch (e) { /* 已断开 */ }
    if (msg && msg.ok) {
      status.className = 'ok';
      status.textContent = '已连接 TinyNext ' + (msg.version || '') + ' ✓';
    } else {
      status.className = 'bad';
      status.textContent = '宿主异常：' + (msg && msg.error || '未知错误');
    }
  });
  port.onDisconnect.addListener(() => {
    const err = chrome.runtime.lastError;
    clearTimeout(timer);
    status.className = 'bad';
    status.textContent = err && err.message
      ? '未连接：' + err.message
      : '未连接：宿主已退出（TinyNext 启动过一次才会注册宿主）';
  });
  port.postMessage({ type: 'ping' });
});
