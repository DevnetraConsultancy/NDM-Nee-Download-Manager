'use strict';

const api = (typeof browser !== 'undefined' && browser.runtime) ? browser : chrome;
const $ = (id) => document.getElementById(id);

const GROUPS = {
  matched:  { label: 'Capture types', test: null },
  archives: { label: 'Archives',  exts: 'zip rar 7z tar gz tgz bz2 xz zst cab iso img' },
  programs: { label: 'Programs',  exts: 'exe msi msix apk xapk dmg pkg deb rpm jar' },
  video:    { label: 'Video',     exts: 'mp4 m4v mkv avi mov wmv flv webm mpg mpeg 3gp ts m2ts' },
  audio:    { label: 'Audio',     exts: 'mp3 m4a aac flac wav ogg opus wma' },
  docs:     { label: 'Documents', exts: 'pdf doc docx xls xlsx ppt pptx odt ods odp rtf epub txt csv' },
  images:   { label: 'Images',    exts: 'jpg jpeg png gif webp bmp svg avif tif tiff' },
  all:      { label: 'All links', test: () => true },
};

let data = null;
let items = [];          // {url, text, kind, name, ext, checked}
let captureExts = new Set();
let group = 'matched';

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
  toastTimer = setTimeout(() => { t.className = 'toast'; }, 3200);
}

function nameOf(u) {
  try { return decodeURIComponent(new URL(u).pathname.split('/').filter(Boolean).pop() || '') || new URL(u).hostname; }
  catch (e) { return u; }
}
function extOf(n) { const m = /\.([a-z0-9]{1,8})$/i.exec(n || ''); return m ? m[1].toLowerCase() : ''; }

function inGroup(it, g) {
  if (g === 'matched') return captureExts.has(it.ext);
  if (GROUPS[g].test) return GROUPS[g].test(it);
  return GROUPS[g].exts.split(' ').includes(it.ext);
}

function visible() {
  const q = $('filter').value.trim().toLowerCase();
  return items.filter(it => inGroup(it, group) &&
    (!q || it.url.toLowerCase().includes(q) || it.text.toLowerCase().includes(q) || ('.' + it.ext) === q));
}

function renderChips() {
  const box = $('chips');
  box.textContent = '';
  for (const [key, g] of Object.entries(GROUPS)) {
    const n = items.filter(it => inGroup(it, key)).length;
    if (!n && key !== 'matched' && key !== 'all') continue;
    const c = document.createElement('span');
    c.className = 'chip' + (key === group ? ' on' : '');
    c.textContent = g.label + ' ' + n;
    c.addEventListener('click', () => { group = key; renderChips(); renderList(); });
    box.appendChild(c);
  }
}

function renderList() {
  const list = $('list');
  list.textContent = '';
  const vis = visible();
  if (!vis.length) {
    const e = document.createElement('div');
    e.className = 'empty';
    e.textContent = items.length ? 'No links match this filter.' : 'No links found.';
    list.appendChild(e);
  }
  for (const it of vis.slice(0, 3000)) {
    const row = document.createElement('div');
    row.className = 'item';
    const lab = document.createElement('label');
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.checked = it.checked;
    cb.addEventListener('change', () => { it.checked = cb.checked; updateCount(); });
    const grow = document.createElement('div');
    grow.className = 'grow';
    const nm = document.createElement('div');
    nm.className = 'name';
    nm.textContent = it.name;
    const ur = document.createElement('div');
    ur.className = 'url';
    ur.textContent = it.text && it.text !== it.name ? it.text + '  —  ' + it.url : it.url;
    ur.title = it.url;
    grow.append(nm, ur);
    const tag = document.createElement('span');
    tag.className = 'tag' + (captureExts.has(it.ext) ? ' gold' : '');
    tag.textContent = it.ext || it.kind;
    lab.append(cb, grow, tag);
    row.appendChild(lab);
    list.appendChild(row);
  }
  updateCount();
}

function updateCount() {
  const n = items.filter(it => it.checked).length;
  $('count').innerHTML = '';
  const b = document.createElement('b');
  b.textContent = String(n);
  $('count').append(b, document.createTextNode(' of ' + items.length + ' links selected'));
  $('send').disabled = n === 0;
}

$('filter').addEventListener('input', renderList);
$('selAll').addEventListener('click', () => { visible().forEach(it => { it.checked = true; }); renderList(); });
$('selNone').addEventListener('click', () => { items.forEach(it => { it.checked = false; }); renderList(); });

$('send').addEventListener('click', async () => {
  const chosen = items.filter(it => it.checked).map(it => ({ url: it.url, filename: it.name }));
  if (!chosen.length) return;
  $('send').disabled = true;
  toast('Sending ' + chosen.length + ' links to NDM...');
  const r = await bg({ action: 'sendLinks', links: chosen, referrer: data.url,
                       incognito: !!data.tabIncognito, cookieStoreId: data.cookieStoreId || '' });
  if (r && r.status === 'accepted') {
    toast('NDM queued ' + r.count + ' download' + (r.count === 1 ? '' : 's') +
          (r.rejected ? ' (' + r.rejected + ' skipped)' : '') + '.', 'ok');
    setTimeout(() => window.close(), 1600);
  } else {
    toast(r && r.error === 'host_missing' ? 'NDM integration is not installed.' : 'NDM is not reachable.', 'err');
    $('send').disabled = false;
  }
});

(async function init() {
  const st = await bg({ action: 'getState' });
  captureExts = new Set(String((st && st.settings && st.settings.extensions) || '').split(/\s+/).filter(Boolean));
  data = await bg({ action: 'getPicker' });
  if (!data || !Array.isArray(data.links)) {
    $('list').innerHTML = '<div class="empty">Nothing to show. Use "Download all links with NDM" on a page.</div>';
    return;
  }
  $('title').textContent = data.title || 'Links on this page';
  $('src').textContent = data.url || '';
  document.title = 'NDM — ' + (data.title || 'links');
  items = data.links.map(l => {
    const name = nameOf(l.url);
    const ext = extOf(name);
    return { url: l.url, text: l.text || '', kind: l.kind || 'link', name, ext, checked: captureExts.has(ext) };
  });
  if (!items.some(it => captureExts.has(it.ext))) group = 'all';
  renderChips();
  renderList();
})();
