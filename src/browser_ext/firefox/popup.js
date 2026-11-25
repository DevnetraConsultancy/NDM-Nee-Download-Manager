'use strict';

const api = (typeof browser !== 'undefined' && browser.runtime) ? browser : chrome;
const $ = (id) => document.getElementById(id);

let settings = null;
let defaults = null;
let siteHost = '';

function bg(msg) {
  return new Promise((resolve) => {
    try {
      const p = api.runtime.sendMessage(msg, (r) => { void api.runtime.lastError; resolve(r); });
      if (p && typeof p.then === 'function') p.then(resolve, () => resolve(null));
    } catch (e) { resolve(null); }
  });
}

let toastTimer = null;
function toast(text, kind) {
  const t = $('toast');
  t.textContent = text;
  t.className = 'toast show ' + (kind || '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.className = 'toast'; }, 2800);
}

function setStatus(kind, text) {
  $('status').className = 'status ' + kind;
  $('statusText').textContent = text;
}

function normHost(h) { return String(h || '').toLowerCase().replace(/^www\./, ''); }

function siteExcluded(host) {
  return (settings.excludedSites || []).some(x => host === x || host.endsWith('.' + x));
}

function render() {
  $('enabled').checked = !!settings.enabled;
  $('videoButton').checked = !!settings.videoButton;
  $('notifications').checked = !!settings.notifications;
  $('exts').value = settings.extensions || '';
  $('minSize').value = settings.minSizeKB || 0;
  if (siteHost) {
    $('site').textContent = siteHost;
    $('siteOn').checked = !siteExcluded(siteHost);
    $('siteOn').disabled = false;
  } else {
    $('site').textContent = 'This page';
    $('siteOn').checked = false;
    $('siteOn').disabled = true;
  }
  const hint = $('hint');
  if (!settings.enabled) { hint.textContent = 'Capture is off. Downloads stay in the browser.'; hint.className = 'show'; }
  else if (siteHost && siteExcluded(siteHost)) { hint.textContent = 'Downloads from ' + siteHost + ' stay in the browser.'; hint.className = 'show'; }
  else hint.className = '';
}

async function save(patch) {
  const r = await bg({ action: 'saveSettings', settings: patch });
  if (r && r.settings) settings = r.settings;
  render();
}

async function refreshStatus() {
  setStatus('wait', 'Checking');
  const s = await bg({ action: 'status' });
  if (!s) setStatus('bad', 'Unavailable');
  else if (s.host && s.running) setStatus('ok', 'NDM running');
  else if (s.host) setStatus('warn', 'NDM idle');
  else if (s.error === 'host_missing') setStatus('bad', 'Not installed');
  else setStatus('bad', 'Unreachable');
  if (s && s.host && !s.running) $('status').title = 'NDM starts automatically when a download is sent.';
  else if (s && !s.host) $('status').title = s.detail || 'The NDM native host could not be started.';
}

async function init() {
  const st = await bg({ action: 'getState' });
  settings = (st && st.settings) || {};
  defaults = (st && st.defaults) || {};
  try {
    const tabs = await api.tabs.query({ active: true, currentWindow: true });
    const u = tabs[0] && tabs[0].url;
    if (u && /^https?:/i.test(u)) siteHost = normHost(new URL(u).hostname);
  } catch (e) { /* no tab access */ }
  render();
  refreshStatus();
}

$('enabled').addEventListener('change', (e) => save({ enabled: e.target.checked }));
$('videoButton').addEventListener('change', (e) => save({ videoButton: e.target.checked }));
$('siteOn').addEventListener('change', (e) => {
  if (!siteHost) return;
  let list = (settings.excludedSites || []).filter(x => x !== siteHost && !siteHost.endsWith('.' + x));
  if (!e.target.checked) list.push(siteHost);
  save({ excludedSites: list });
});

$('quickForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const url = $('quickUrl').value.trim();
  if (!/^https?:\/\//i.test(url)) { toast('Enter an http or https address.', 'err'); return; }
  toast('Sending to NDM...');
  const r = await bg({ action: 'addUrl', url });
  if (r && r.status === 'accepted') { toast('Sent to NDM.', 'ok'); $('quickUrl').value = ''; refreshStatus(); }
  else toast(r && r.error === 'host_missing' ? 'NDM integration is not installed.' : 'NDM is not reachable.', 'err');
});

$('grab').addEventListener('click', async () => {
  const r = await bg({ action: 'grabLinks' });
  if (r && r.ok) window.close();
  else toast(r && r.error === 'empty' ? 'No links found on this page.' : 'This page cannot be read.', 'err');
});

$('openNdm').addEventListener('click', async () => {
  toast('Opening NDM...');
  const r = await bg({ action: 'showNdm' });
  if (r && r.status === 'ok') window.close();
  else toast('NDM is not reachable.', 'err');
});

$('saveAdv').addEventListener('click', async () => {
  await save({
    extensions: $('exts').value,
    minSizeKB: $('minSize').value,
    notifications: $('notifications').checked,
  });
  toast('Settings saved.', 'ok');
});

$('resetExts').addEventListener('click', () => { $('exts').value = defaults.extensions || ''; });

init();
