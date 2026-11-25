'use strict';

const api = (typeof browser !== 'undefined' && browser.runtime) ? browser : chrome;
const $ = (id) => document.getElementById(id);

function bg(msg) {
  return new Promise((resolve) => {
    try {
      const p = api.runtime.sendMessage(msg, (r) => { void api.runtime.lastError; resolve(r); });
      if (p && typeof p.then === 'function') p.then(resolve, () => resolve(null));
    } catch (e) { resolve(null); }
  });
}

const STATUS = {
  sent:    ['ok',  'Sent to NDM'],
  browser: ['',    'Browser'],
  failed:  ['bad', 'Failed'],
};

function when(t) {
  const d = new Date(t);
  const same = new Date().toDateString() === d.toDateString();
  return same ? d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
              : d.toLocaleDateString([], { month: 'short', day: 'numeric' }) + ' ' +
                d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

async function render() {
  const hist = (await bg({ action: 'getHistory' })) || [];
  const list = $('list');
  list.textContent = '';
  if (!hist.length) {
    const e = document.createElement('div');
    e.className = 'empty';
    e.textContent = 'Nothing captured yet. Click a download link and NDM takes it from here.';
    list.appendChild(e);
    return;
  }
  for (const h of hist) {
    const row = document.createElement('div');
    row.className = 'item';
    const grow = document.createElement('div');
    grow.className = 'grow';
    const nm = document.createElement('div');
    nm.className = 'name';
    nm.textContent = h.filename || h.url;
    const ur = document.createElement('div');
    ur.className = 'url';
    ur.textContent = h.url;
    ur.title = h.url;
    grow.append(nm, ur);
    const [cls, label] = STATUS[h.status] || ['', h.status || ''];
    const tag = document.createElement('span');
    tag.className = 'tag ' + cls;
    tag.textContent = label;
    if (h.error) tag.title = h.error;
    const tm = document.createElement('span');
    tm.className = 'time';
    tm.textContent = when(h.t);
    row.append(grow, tag, tm);
    list.appendChild(row);
  }
}

async function status() {
  const s = await bg({ action: 'status' });
  const el = $('status');
  const txt = $('statusText');
  if (s && s.host && s.running) { el.className = 'status ok';   txt.textContent = 'NDM running'; }
  else if (s && s.host)         { el.className = 'status warn'; txt.textContent = 'NDM idle'; }
  else if (s && s.error === 'host_missing') { el.className = 'status bad'; txt.textContent = 'Not installed'; }
  else                          { el.className = 'status bad';  txt.textContent = 'Unreachable'; }
}

$('clear').addEventListener('click', async () => { await bg({ action: 'clearHistory' }); render(); });
$('openNdm').addEventListener('click', async () => { await bg({ action: 'showNdm' }); status(); });

api.storage.onChanged.addListener((ch, area) => { if (area === 'local' && ch.ndmHistory) render(); });

render();
status();
