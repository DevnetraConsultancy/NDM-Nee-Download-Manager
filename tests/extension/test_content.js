// Minimal fake-DOM test for src/browser_ext/*/content.js (no jsdom).
//   node test_content.js <content.js>
'use strict';
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

class El {
  constructor(tag, attrs) {
    this.tagName = tag.toUpperCase(); this.children = []; this.parentNode = null; this.style = {};
    this.attrs = Object.assign({}, attrs || {}); this.listeners = {}; this.rect = null; this._text = '';
    const self = this;
    this.classList = {
      set: new Set((this.attrs.class || '').split(/\s+/).filter(Boolean)),
      add(c) { this.set.add(c); }, remove(c) { this.set.delete(c); }, contains(c) { return this.set.has(c); },
    };
    Object.defineProperty(this, 'className', { get() { return [...self.classList.set].join(' '); },
                                               set(v) { self.classList.set = new Set(String(v).split(/\s+/).filter(Boolean)); } });
  }
  get id() { return this.attrs.id || ''; }
  get src() { return this.attrs.src || ''; }
  get currentSrc() { return this.attrs.currentSrc || this.attrs.src || ''; }
  get naturalWidth() { return +(this.attrs.naturalWidth || 0); }
  get parentElement() { return this.parentNode instanceof El ? this.parentNode : null; }
  get isConnected() { let n = this; while (n.parentNode) n = n.parentNode; return n === doc.documentElement; }
  get firstChild() { return this.children[0] || null; }
  get offsetWidth() { return 96; }
  getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
  appendChild(c) { if (c.parentNode) c.remove(); c.parentNode = this; this.children.push(c); return c; }
  insertBefore(c, ref) { if (c.parentNode) c.remove(); c.parentNode = this; const i = this.children.indexOf(ref); this.children.splice(i < 0 ? 0 : i, 0, c); return c; }
  remove() { if (this.parentNode) { const a = this.parentNode.children; a.splice(a.indexOf(this), 1); this.parentNode = null; } }
  set textContent(v) { this.children = []; this._text = String(v); }
  get textContent() { return this._text + this.children.map(c => c.textContent).join(''); }
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
  dispatch(t, e) { e = Object.assign({ type: t, stopPropagation() {}, preventDefault() {} }, e || {}); (this.listeners[t] || []).forEach(f => f(e)); }
  getBoundingClientRect() {
    const r = this.rect || { left: 0, top: 0, width: 0, height: 0 };
    return { left: r.left, top: r.top, width: r.width, height: r.height, right: r.left + r.width, bottom: r.top + r.height };
  }
  matches(sel) { return sel.split(/\s*,\s*/).some(s => matchOne(this, s)); }
  closest(sel) { for (let n = this; n instanceof El; n = n.parentNode) if (n.matches(sel)) return n; return null; }
  all() { const out = []; const walk = (n) => n.children.forEach(c => { out.push(c); walk(c); }); walk(this); return out; }
  querySelector(sel) { return this.all().find(n => n.matches(sel)) || null; }
  attachShadow() { const r = new El('#shadow'); r.host = this; this.shadow = r;
    Object.defineProperty(r, 'innerHTML', { set(html) { // tiny parser for the overlay template
      r.children = []; const b = new El('div', { class: 'b' }); b.appendChild(new El('svg')); b.appendChild(new El('span'));
      r.appendChild(new El('style')); r.appendChild(b); r.appendChild(new El('div', { class: 'm' })); } });
    return r; }
}
// selector: tag, #id, .class, [attr], [attr*="x"], combos like a#thumbnail[href*="/watch"]
function matchOne(el, s) {
  const m = /^([a-z0-9-]*)((?:#[\w-]+)?)((?:\.[\w-]+)*)((?:\[[^\]]+\])*)$/i.exec(s.trim());
  if (!m) return false;
  if (m[1] && el.tagName !== m[1].toUpperCase()) return false;
  if (m[2] && el.id !== m[2].slice(1)) return false;
  for (const c of (m[3].match(/\.[\w-]+/g) || [])) if (!el.classList.contains(c.slice(1))) return false;
  for (const a of (m[4].match(/\[[^\]]+\]/g) || [])) {
    const am = /^\[([\w-]+)(?:(\*?=)"([^"]*)")?\]$/.exec(a);
    const v = el.getAttribute(am[1]);
    if (v == null) return false;
    if (am[2] === '*=' && !v.includes(am[3])) return false;
    if (am[2] === '=' && v !== am[3]) return false;
  }
  return true;
}

const doc = { listeners: {}, documentElement: new El('html'), fullscreenElement: null,
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); },
  dispatch(t, e) { (this.listeners[t] || []).forEach(f => f(Object.assign({ type: t, composedPath: () => [] }, e))); },
  createElement: (t) => new El(t), getElementsByTagName: () => [], title: 'Big: Buck Bunny - YouTube', referrer: '',
  stack: [], elementsFromPoint() { return this.stack; } };
doc.body = doc.documentElement.appendChild(new El('body'));

function run(file, href) {
  const sent = [];
  const replies = {
    mediaSniffed: { items: [{ label: 'MP4', url: 'https://cdn.test/direct.mp4', ext: 'mp4', filesize: 5 * 1048576 }] },
    mediaList: { status: 'ok', title: 'Big Buck Bunny', probeUrl: 'PROBE', formats: [
      { label: '1080p MP4', format: 'bv*[height<=1080]+ba/b[height<=1080]/b', ext: 'mp4', filesize: 148897792, approx: true },
      { label: 'Audio M4A', format: 'ba/b', ext: 'm4a', filesize: 10271496 }] },
    mediaPick: { status: 'accepted' }, mediaPrefetch: { ok: true }, magnet: { status: 'accepted' },
  };
  const chrome = { runtime: { lastError: null, sendMessage: (m, cb) => { sent.push(m); setTimeout(() => cb(m.action === 'mediaSniffed' && !m.src ? { items: [] } : (replies[m.action] || null)), 0); } },
                   storage: { local: { get: (k, cb) => cb({}) }, onChanged: { addListener() {} } } };
  const win = { listeners: {}, addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); } };
  const loc = new URL(href);
  const ctx = { chrome, document: doc, location: { href, hostname: loc.hostname, pathname: loc.pathname }, innerWidth: 1600, innerHeight: 900,
                setTimeout, clearTimeout, setInterval: () => 1, clearInterval() {}, URL, console, Date, Promise, Map, Set };
  ctx.window = Object.assign(win, ctx); ctx.window.top = ctx.window;
  vm.createContext(ctx);
  vm.runInContext(fs.readFileSync(file, 'utf8'), ctx);
  return { sent, ctx };
}
const tick = () => new Promise(r => setTimeout(r, 5));
const overlay = () => doc.documentElement.children.find(c => c.tagName === 'NDM-OVERLAY');
const move = (x, y) => { doc.dispatch('mousemove', { clientX: x, clientY: y }); };

(async () => {
  const file = process.argv[2];
  // YouTube watch page: player 1280x720
  const player = doc.body.appendChild(new El('div', { id: 'movie_player', class: 'html5-video-player' }));
  player.rect = { left: 100, top: 80, width: 1280, height: 720 };
  const video = player.appendChild(new El('video', { src: 'blob:https://www.youtube.com/x' }));
  video.rect = player.rect;
  const { sent, ctx } = run(file, 'https://www.youtube.com/watch?v=aqz-KE-bpKQ&t=10s&list=PL1');
  doc.stack = [new El('div', { class: 'ytp-chrome' }), video];
  move(500, 300);
  const host = overlay();
  assert.ok(host && host.style.display === 'block', 'button shown over the player');
  assert.strictEqual(host.style.left, (1380 - 8 - 96) + 'px');
  assert.strictEqual(host.style.top, '88px');
  const root = host.shadow;
  const btn = root.children[1], menu = root.children[2];
  // icon-only at rest (a ~30 px square tucked 8 px into the corner)
  assert.strictEqual(btn.children[1].textContent, '');
  assert.ok(btn.classList.contains('i'), 'icon-only button');

  btn.dispatch('click', { isTrusted: false });
  for (let i = 0; i < 5; i++) await tick();
  assert.ok(!sent.some(m => m.action === 'mediaList'), 'synthetic click on the button ignored');
  btn.dispatch('click', { isTrusted: true });
  for (let i = 0; i < 5; i++) await tick();
  const list = sent.find(m => m.action === 'mediaList');
  assert.strictEqual(list.pageUrl, 'https://www.youtube.com/watch?v=aqz-KE-bpKQ', 'clean watch URL probed');
  const rows = menu.children.map(r => r.children.map(c => c.textContent).join(' | '));
  assert.deepStrictEqual(rows, ['1080p MP4 | ~142 MB', 'Audio M4A | 9.8 MB'], 'blob video: no direct row, yt-dlp list');
  menu.children[0].dispatch('click', { isTrusted: true });
  await tick(); await tick();
  assert.ok(!sent.some(m => m.action === 'mediaDownload' || m.action === 'downloadMedia'), 'legacy types not used');
  const dl = sent.find(m => m.action === 'mediaPick');
  assert.ok(!('filename' in dl) && !('title' in dl), 'content script sends no file name / title');
  assert.strictEqual(dl.item.format, 'bv*[height<=1080]+ba/b[height<=1080]/b');
  assert.strictEqual(dl.probeUrl, 'PROBE');
  assert.strictEqual(btn.children[1].textContent, 'Sent to NDM');

  // ad playing -> hidden; tiny video -> no button
  player.classList.add('ad-showing');
  move(600, 300); await new Promise(r => setTimeout(r, 130)); move(601, 300);
  // (usable() rejects it; the follow timer would hide it — call place via scroll)
  ctx_scroll();
  assert.strictEqual(host.style.display, 'none', 'hidden during ads');
  player.classList.remove('ad-showing');
  const tiny = doc.body.appendChild(new El('video', { src: 'https://x/y.mp4' }));
  tiny.rect = { left: 10, top: 10, width: 150, height: 90 };
  doc.stack = [tiny];
  await new Promise(r => setTimeout(r, 130)); move(20, 20);
  assert.strictEqual(host.style.display, 'none', 'no button on tiny video');

  // direct-file video shows the instant row first
  const big = doc.body.appendChild(new El('video', { src: 'https://cdn.test/direct.mp4' }));
  big.rect = { left: 0, top: 0, width: 640, height: 360 };
  doc.stack = [big];
  await new Promise(r => setTimeout(r, 130)); move(50, 50);
  assert.strictEqual(host.style.display, 'block');
  btn.dispatch('click', { isTrusted: true });
  for (let i = 0; i < 5; i++) await tick();
  // yt-dlp video entries first, then the page's own file, then audio
  assert.deepStrictEqual(menu.children.map(r => r.children.map(c => c.textContent).join(' | ')),
                         ['1080p MP4 | ~142 MB', 'MP4 | 5.0 MB', 'Audio M4A | 9.8 MB']);

  // magnet link click -> sent to NDM, default prevented
  const a = doc.body.appendChild(new El('a', { href: 'magnet:?xt=urn:btih:abc' }));
  a.href = 'magnet:?xt=urn:btih:abc';
  let prevented = false;
  doc.dispatch('click', { isTrusted: false, button: 0, target: a, altKey: false, preventDefault() { prevented = true; }, stopPropagation() {} });
  await tick();
  assert.ok(!prevented && !sent.some(m => m.action === 'magnet'), 'synthetic (script) magnet click ignored');
  doc.dispatch('click', { isTrusted: true, button: 0, target: a, altKey: false, preventDefault() { prevented = true; }, stopPropagation() {} });
  await tick();
  assert.ok(prevented && sent.some(m => m.action === 'magnet' && m.url === a.href), 'magnet captured');
  prevented = false;
  doc.dispatch('click', { isTrusted: true, button: 0, target: a, altKey: false, preventDefault() { prevented = true; }, stopPropagation() {} });
  await tick();
  assert.strictEqual(sent.filter(m => m.action === 'magnet').length, 1, 'second magnet within 1.5 s not sent');

  // NDM unreachable: the link is re-clicked for the OS handler — and the NEXT
  // real click on it goes to NDM again (the bypass flag never sticks).
  {
    const b2 = doc.body.appendChild(new El('a', { href: 'magnet:?xt=urn:btih:def' }));
    b2.href = 'magnet:?xt=urn:btih:def';
    let osClicks = 0;
    b2.click = () => { osClicks++; doc.dispatch('click', { isTrusted: false, button: 0, target: b2, altKey: false, preventDefault() {}, stopPropagation() {} }); };
    const realSend = ctx.chrome.runtime.sendMessage;
    ctx.chrome.runtime.sendMessage = (m, cb) => { if (m.action !== 'magnet') return realSend(m, cb); sent.push(m); setTimeout(() => cb({ status: 'error', error: 'ndm_unreachable' }), 0); };
    await new Promise(r => setTimeout(r, 1600));
    let prev = false;
    doc.dispatch('click', { isTrusted: true, button: 0, target: b2, altKey: false, preventDefault() { prev = true; }, stopPropagation() {} });
    for (let i = 0; i < 4; i++) await tick();
    assert.ok(prev && osClicks === 1, 'unreachable: handed back to the OS once');
    await new Promise(r => setTimeout(r, 1600));
    prev = false;
    const before = sent.filter(m => m.action === 'magnet').length;
    doc.dispatch('click', { isTrusted: true, button: 0, target: b2, altKey: false, preventDefault() { prev = true; }, stopPropagation() {} });
    await tick();
    assert.ok(prev && sent.filter(m => m.action === 'magnet').length === before + 1, 'next real click goes to NDM again (flag cleared)');
  }
  console.log('all content tests passed (' + file + ')');
  process.exit(0);
  function ctx_scroll() { (ctx.window.listeners.scroll || []).forEach(f => f({})); }
})().catch(e => { console.error('FAIL', e); process.exit(1); });
