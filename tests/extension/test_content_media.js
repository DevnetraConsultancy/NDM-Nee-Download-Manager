// Overlay tests for the media / photo / audio picker in content.js (fake DOM, no jsdom).
//   node test_content_media.js <content.js>
'use strict';
const fs = require('fs'), vm = require('vm'), assert = require('assert');
const file = process.argv[2];
let doc;

class El {
  constructor(tag, attrs) {
    this.tagName = tag.toUpperCase(); this.children = []; this.parentNode = null; this.style = {};
    this.attrs = Object.assign({}, attrs || {}); this.listeners = {}; this.rect = null; this._text = '';
    const self = this;
    this.classList = { set: new Set((this.attrs.class || '').split(/\s+/).filter(Boolean)),
      add(c) { this.set.add(c); }, remove(c) { this.set.delete(c); }, contains(c) { return this.set.has(c); } };
    Object.defineProperty(this, 'className', { get() { return [...self.classList.set].join(' '); },
      set(v) { self.classList.set = new Set(String(v).split(/\s+/).filter(Boolean)); } });
  }
  get id() { return this.attrs.id || ''; }
  get src() { return this.attrs.src || ''; }
  get currentSrc() { return this.attrs.currentSrc || this.attrs.src || ''; }
  get naturalWidth() { return +(this.attrs.naturalWidth || 0); }
  get videoHeight() { return +(this.attrs.videoHeight || 0); }
  get parentElement() { return this.parentNode instanceof El && this.parentNode.tagName !== '#SHADOW' ? this.parentNode : null; }
  getRootNode() { let n = this; while (n.parentNode) n = n.parentNode; return n; }
  get isConnected() { let n = this; for (;;) { if (n.parentNode) n = n.parentNode; else if (n.host) n = n.host; else break; } return n === doc.documentElement; }
  get firstChild() { return this.children[0] || null; }
  get offsetWidth() { return 96; }
  setAttribute(k, v) { if (k === 'class') this.className = v; else this.attrs[k] = String(v); }
  getAttribute(k) { return k === 'class' ? (this.className || null) : k in this.attrs ? this.attrs[k] : null; }
  appendChild(c) { if (c.parentNode) c.remove(); c.parentNode = this; this.children.push(c); return c; }
  insertBefore(c, ref) { if (c.parentNode) c.remove(); c.parentNode = this; const i = this.children.indexOf(ref); this.children.splice(i < 0 ? 0 : i, 0, c); return c; }
  remove() { if (this.parentNode) { const a = this.parentNode.children; a.splice(a.indexOf(this), 1); this.parentNode = null; } }
  set textContent(v) { this.children = []; this._text = String(v); }
  get textContent() { return this._text + this.children.map(c => c.textContent).join(''); }
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
  dispatch(t, e) { e = Object.assign({ type: t, stopPropagation() {}, preventDefault() {} }, e || {}); (this.listeners[t] || []).forEach(f => f(e)); }
  getBoundingClientRect() { const r = this.rect || { left: 0, top: 0, width: 0, height: 0 };
    return { left: r.left, top: r.top, width: r.width, height: r.height, right: r.left + r.width, bottom: r.top + r.height }; }
  matches(sel) { return sel.split(/\s*,\s*/).some(s => matchOne(this, s)); }
  closest(sel) { for (let n = this; n instanceof El; n = n.parentNode) if (n.matches(sel)) return n; return null; }
  all() { const out = []; const walk = (n) => n.children.forEach(c => { out.push(c); walk(c); }); walk(this); return out; }
  querySelector(sel) { return this.all().find(n => n.matches(sel)) || null; }
  querySelectorAll(sel) { return this.all().filter(n => n.matches(sel)); }
  attachShadow() { const r = new El('#shadow'); r.host = this; this.shadow = r;
    Object.defineProperty(r, 'innerHTML', { set() { r.children = []; const b = new El('div', { class: 'b' });
      b.appendChild(new El('svg')); b.appendChild(new El('span')); r.appendChild(new El('style')); r.appendChild(b); r.appendChild(new El('div', { class: 'm' })); } });
    return r; }
}
function matchOne(el, s) {
  const m = /^([a-z0-9-]*)((?:#[\w-]+)?)((?:\.[\w-]+)*)((?:\[[^\]]+\])*)$/i.exec(s.trim());
  if (!m) return false;
  if (m[1] && el.tagName !== m[1].toUpperCase()) return false;
  if (m[2] && el.id !== m[2].slice(1)) return false;
  for (const c of (m[3].match(/\.[\w-]+/g) || [])) if (!el.classList.contains(c.slice(1))) return false;
  for (const a of (m[4].match(/\[[^\]]+\]/g) || [])) {
    const am = /^\[([\w-]+)(?:(\*?=)"([^"]*)")?\]$/.exec(a);
    if (!am) return false;
    const v = el.getAttribute(am[1]);
    if (v == null) return false;
    if (am[2] === '*=' && !v.includes(am[3])) return false;
    if (am[2] === '=' && v !== am[3]) return false;
  }
  return true;
}

function setup(href, replies, extra) {
  doc = { listeners: {}, documentElement: new El('html'), fullscreenElement: null, title: 'Page title', referrer: '',
    addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); },
    dispatch(t, e) { (this.listeners[t] || []).forEach(f => f(Object.assign({ type: t, composedPath: () => [] }, e))); },
    createElement: (t) => new El(t), stack: [], elementsFromPoint() { return this.stack; },
    querySelector(sel) { return this.documentElement.querySelector(sel); },
    querySelectorAll(sel) { return this.documentElement.querySelectorAll(sel); } };
  doc.head = doc.documentElement.appendChild(new El('head'));
  doc.body = doc.documentElement.appendChild(new El('body'));
  const sent = [];
  const chrome = { runtime: { lastError: null, sendMessage: (m, cb) => { sent.push(m);
      const r = typeof replies[m.action] === 'function' ? replies[m.action](m) : replies[m.action];
      setTimeout(() => cb(r || null), 0); } },
    storage: { local: { get: (k, cb) => cb({}) }, onChanged: { addListener() {} } } };
  const win = { listeners: {}, addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); } };
  const loc = new URL(href);
  const ctx = Object.assign({ chrome, document: doc, location: { href, hostname: loc.hostname, pathname: loc.pathname },
    innerWidth: 1600, innerHeight: 900, setTimeout, clearTimeout, setInterval: () => 1, clearInterval() {}, URL, console, Date, Promise, Map, Set }, extra || {});
  ctx.window = Object.assign(win, ctx); ctx.window.top = ctx.window;
  vm.createContext(ctx);
  vm.runInContext(fs.readFileSync(file, 'utf8'), ctx);
  return { sent, ctx };
}
const tick = (ms) => new Promise(r => setTimeout(r, ms || 5));
const overlay = () => doc.documentElement.children.find(c => c.tagName === 'NDM-OVERLAY');
async function hover(el, x, y) { doc.stack = Array.isArray(el) ? el : [el]; await tick(130); doc.dispatch('mousemove', { clientX: x || 150, clientY: y || 150 }); }
async function clickBtn() { const b = overlay().shadow.children[1]; b.dispatch('click', { isTrusted: true }); for (let i = 0; i < 8; i++) await tick(); }
const rows = () => overlay().shadow.children[2].children.map(r => r.children.map(c => c.textContent).join(' | '));
const lastPick = (sent) => sent.filter(m => m.action === 'mediaPick').pop();
const big = { left: 100, top: 100, width: 600, height: 400 };
let n = 0;

async function photoCase(href, attrs, want, why, parent) {
  const { sent } = setup(href, { mediaPick: { status: 'accepted' } });
  const img = (parent ? parent(doc.body) : doc.body).appendChild(new El('img', attrs));
  img.rect = big;
  await hover(img, 300, 300);
  const h = overlay();
  assert.ok(h && h.style.display === 'block', why + ': button shown');
  assert.ok(h.shadow.children[1].classList.contains('i'), why + ': icon-only photo button');
  await clickBtn();
  const p = lastPick(sent);
  assert.strictEqual(p && p.item.url, want, why);
  assert.ok(!('filename' in p) && !('filename' in p.item), 'no file name from the content script');
  assert.strictEqual(p.mime, 'image/*');
  n++;
}

(async () => {
  // ── photo picker ──
  await photoCase('https://news.example/article', { src: 'https://img.example/a-400.jpg', naturalWidth: '400',
    srcset: 'https://img.example/a-400.jpg 400w, https://img.example/a-1600.jpg 1600w, https://img.example/a-800.jpg 800w' },
    'https://img.example/a-1600.jpg', 'srcset: largest w candidate');
  await photoCase('https://news.example/article', { src: 'https://img.example/b.jpg', naturalWidth: '500',
    srcset: 'https://img.example/b.jpg 1x, https://img.example/b@2x.jpg 2x' }, 'https://img.example/b@2x.jpg', 'srcset: x descriptors');
  await photoCase('https://news.example/article', { src: 'data:image/gif;base64,R0lGOD', 'data-src': '/uploads/c.jpg' },
    'https://news.example/uploads/c.jpg', 'lazy data-src when src is a placeholder');
  await photoCase('https://blog.example/post', { src: 'https://blog.example/d-300x200.jpg', naturalWidth: '300',
    'data-orig-file': 'https://blog.example/d.jpg', 'data-large-file': 'https://blog.example/d-1024x683.jpg' },
    'https://blog.example/d.jpg', 'WordPress data-orig-file');
  await photoCase('https://shop.example/p', { src: 'https://shop.example/e-small.jpg', naturalWidth: '300' },
    'https://shop.example/e-big.webp', 'picture source srcset largest', (body) => {
      const pic = body.appendChild(new El('picture'));
      pic.appendChild(new El('source', { srcset: 'https://shop.example/e-mid.webp 800w, https://shop.example/e-big.webp 2000w', type: 'image/webp' }));
      return pic; });
  await photoCase('https://x.com/NASA/status/1', { src: 'https://pbs.twimg.com/media/GabcXYZ?format=jpg&name=small', naturalWidth: '680' },
    'https://pbs.twimg.com/media/GabcXYZ?format=jpg&name=orig', 'X: pbs.twimg.com name=orig');
  await photoCase('https://twitter.com/NASA/status/1', { src: 'https://pbs.twimg.com/media/GabcXYZ.png:large', naturalWidth: '680' },
    'https://pbs.twimg.com/media/GabcXYZ?format=png&name=orig', 'X: legacy .png:large form');
  await photoCase('https://www.pinterest.com/pin/1/', { src: 'https://i.pinimg.com/236x/ab/cd/ef/abcdef.jpg', naturalWidth: '236' },
    'https://i.pinimg.com/originals/ab/cd/ef/abcdef.jpg', 'Pinterest originals');
  await photoCase('https://www.reddit.com/r/pics/', { src: 'https://preview.redd.it/sunset-at-the-lake-v0-q1w2e3r4.jpeg?width=640&s=abc', naturalWidth: '640' },
    'https://i.redd.it/q1w2e3r4.jpeg', 'Reddit preview -> i.redd.it original');
  await photoCase('https://www.instagram.com/p/xyz/', { src: 'https://scontent.cdninstagram.com/a_s640.jpg', naturalWidth: '640',
    srcset: 'https://scontent.cdninstagram.com/a_s1080.jpg 1080w,https://scontent.cdninstagram.com/a_s640.jpg 640w, https://scontent.cdninstagram.com/a_s750.jpg 750w' },
    'https://scontent.cdninstagram.com/a_s1080.jpg', 'Instagram largest srcset');
  {
    const { sent } = setup('https://news.example/story', { mediaPick: { status: 'accepted' } });
    doc.head.appendChild(new El('meta', { property: 'og:image', content: 'https://cdn.news.example/hero.jpg' }));
    const img = doc.body.appendChild(new El('img', { src: 'https://cdn.news.example/hero-640x360.jpg', naturalWidth: '640' }));
    img.rect = big; await hover(img); await clickBtn();
    assert.strictEqual(lastPick(sent).item.url, 'https://cdn.news.example/hero.jpg', 'og:image when it is the same main image'); n++;
    const other = doc.body.appendChild(new El('img', { src: 'https://cdn.news.example/other-640x360.jpg', naturalWidth: '640' }));
    other.rect = { left: 100, top: 520, width: 600, height: 300 }; await hover(other, 200, 600); await clickBtn();
    assert.strictEqual(lastPick(sent).item.url, 'https://cdn.news.example/other-640x360.jpg', 'other images keep their own source'); n++;
  }
  {
    const { sent } = setup('https://www.google.com/search?tbm=isch&q=cat', { mediaPick: { status: 'accepted' } });
    const a = doc.body.appendChild(new El('a', { href: '/imgres?imgurl=https%3A%2F%2Fphotos.example%2Fcat.jpg&imgrefurl=x' }));
    const img = a.appendChild(new El('img', { src: 'https://encrypted-tbn0.gstatic.com/images?q=tbn:x', naturalWidth: '300' }));
    img.rect = big; await hover(img); await clickBtn();
    assert.strictEqual(lastPick(sent).item.url, 'https://photos.example/cat.jpg', 'Google Images preview -> original'); n++;
  }
  // skip: small rendered, icon stretched, svg, tracking pixel
  for (const [attrs, rect, why] of [
    [{ src: 'https://a.example/avatar.jpg', naturalWidth: '400' }, { left: 0, top: 0, width: 150, height: 150 }, 'small rendered image'],
    [{ src: 'https://a.example/wide.jpg', naturalWidth: '900' }, { left: 0, top: 0, width: 900, height: 120 }, 'thin banner'],
    [{ src: 'https://a.example/icon.png', naturalWidth: '64' }, big, 'upscaled icon'],
    [{ src: 'https://a.example/logo.svg', naturalWidth: '0' }, big, 'svg'],
    [{ src: 'https://t.example/pixel.gif', naturalWidth: '1' }, big, 'tracking pixel'],
  ]) {
    setup('https://a.example/', {});
    const img = doc.body.appendChild(new El('img', attrs)); img.rect = rect;
    await hover(img, 50, 50);
    assert.ok(!overlay() || overlay().style.display !== 'block', 'no button: ' + why); n++;
  }
  // CSS background photo (no text, big, single url) -> photo; text banner with bg -> none
  {
    const styles = new Map();
    const { sent } = setup('https://gallery.example/', { mediaPick: { status: 'accepted' } },
      { getComputedStyle: (el) => ({ backgroundImage: styles.get(el) || 'none' }) });
    const div = doc.body.appendChild(new El('div')); div.rect = big;
    styles.set(div, 'url("https://gallery.example/photos/f.jpg")');
    await hover(div); await clickBtn();
    assert.strictEqual(lastPick(sent).item.url, 'https://gallery.example/photos/f.jpg', 'background-image photo'); n++;
    const hero = doc.body.appendChild(new El('div')); hero.rect = { left: 0, top: 500, width: 800, height: 300 };
    hero.textContent = 'Welcome to our site. Lorem ipsum dolor sit amet, consectetur adipiscing elit.';
    styles.set(hero, 'url("https://gallery.example/bg.jpg")');
    overlay().style.display = 'none';
    await hover(hero, 100, 600);
    assert.notStrictEqual(overlay().style.display, 'block', 'text banner background is not a photo'); n++;
  }

  // ── X video in the home timeline: the post's status URL is probed ──
  const fmts = { status: 'ok', title: 'NASA', probeUrl: 'https://x.com/NASA/status/1790000000000000001', formats: [
    { label: 'Best (1080p MP4)', kind: 'video', format: 'bv*+ba/b', ext: 'mp4', filesize: 148897792, approx: true },
    { label: '720p MP4', kind: 'video', format: 'bv*[height<=720]+ba/b[height<=720]/b', ext: 'mp4', filesize: 52428800 },
    { label: 'Audio M4A', kind: 'audio', format: 'x:m4a|ba[acodec^=mp4a]/ba/b', ext: 'm4a', filesize: 10271496 },
    { label: 'Audio MP3', kind: 'audio', format: 'x:mp3|ba[acodec^=mp3]/ba/b', ext: 'mp3' }] };
  {
    const { sent } = setup('https://x.com/home', { mediaSniffed: { items: [] }, mediaList: fmts, mediaPick: { status: 'accepted' }, mediaPrefetch: { ok: true } });
    const art = doc.body.appendChild(new El('article'));
    art.appendChild(new El('a', { href: '/NASA' }));
    const t = art.appendChild(new El('a', { href: '/NASA/status/1790000000000000001' })); t.appendChild(new El('time'));
    art.appendChild(new El('a', { href: '/NASA/status/1790000000000000001/analytics' }));
    const v = art.appendChild(new El('div')).appendChild(new El('video', { src: 'blob:https://x.com/1' }));
    v.rect = big;
    await hover(v); await clickBtn();
    const ml = sent.find(m => m.action === 'mediaList');
    assert.strictEqual(ml.pageUrl, 'https://x.com/NASA/status/1790000000000000001', 'X feed: status URL, not /home');
    assert.strictEqual(ml.kind, 'video');
    assert.deepStrictEqual(rows(), ['Best (1080p MP4 · ~142 MB)', '720p MP4 | 50.0 MB', 'Audio M4A | 9.8 MB', 'Audio MP3'], 'Best first, then resolutions, then audio');
    assert.ok(overlay().shadow.children[1].classList.contains('i') && overlay().shadow.children[1].children[1].textContent === '', 'video button: icon only');
    overlay().shadow.children[2].children[0].dispatch('click', { isTrusted: true }); await tick(); await tick();
    assert.strictEqual(lastPick(sent).item.format, 'bv*+ba/b'); n++;
  }
  // ── Reddit: video inside a web component's shadow root, post permalink probed ──
  {
    const { sent } = setup('https://www.reddit.com/', { mediaSniffed: { items: [] }, mediaList: fmts });
    const post = doc.body.appendChild(new El('shreddit-post', { permalink: '/r/videos/comments/abc123/cool_video/' }));
    const player = post.appendChild(new El('shreddit-player'));
    player.rect = big;
    const sr = new El('#shadow'); sr.host = player; player.shadowRoot = sr;
    const v = sr.appendChild(new El('video', { src: 'blob:https://www.reddit.com/2' })); v.rect = big;
    sr.elementsFromPoint = () => [v];
    await hover(player); await clickBtn();
    const ml = sent.find(m => m.action === 'mediaList');
    assert.strictEqual(ml && ml.pageUrl, 'https://www.reddit.com/r/videos/comments/abc123/cool_video/', 'Reddit: shadow-DOM video, permalink'); n++;
  }
  // ── <audio> element: audio button, audio list, exact file first ──
  {
    const { sent } = setup('https://podcast.example/ep/12', {
      mediaSniffed: (m) => ({ items: m.kind === 'audio' && m.src ? [{ label: 'Audio MP3', url: m.src, ext: 'mp3', filesize: 30 * 1048576, kind: 'audio' }] : [] }),
      mediaList: { status: 'ok', title: 'Ep 12', probeUrl: 'https://podcast.example/ep/12', formats: fmts.formats.filter(f => f.kind === 'audio') },
      mediaPick: { status: 'accepted' } });
    const au = doc.body.appendChild(new El('audio', { src: 'https://media.podcast.example/ep12.mp3', controls: '' }));
    au.rect = { left: 100, top: 300, width: 400, height: 40 };
    await hover(au, 200, 320);
    const h = overlay();
    assert.ok(h.style.display === 'block' && h.shadow.children[1].classList.contains('i'), 'audio button: icon only');
    assert.strictEqual(h.style.top, '266px', 'placed above the slim audio bar');
    await clickBtn();
    assert.strictEqual(sent.find(m => m.action === 'mediaList').kind, 'audio');
    assert.deepStrictEqual(rows(), ['Audio MP3 | 30.0 MB', 'Audio M4A | 9.8 MB', 'Audio MP3'], 'exact file first, then yt-dlp audio');
    overlay().shadow.children[2].children[0].dispatch('click', { isTrusted: true }); await tick(); await tick();
    assert.strictEqual(lastPick(sent).item.url, 'https://media.podcast.example/ep12.mp3'); n++;
  }
  // ── custom player: play control on a music site ──
  {
    const { sent } = setup('https://soundcloud.com/artist', { mediaSniffed: { items: [] }, mediaList: { status: 'ok', probeUrl: 'p', formats: fmts.formats.slice(2) } });
    const item = doc.body.appendChild(new El('li', { class: 'soundList__item' }));
    item.appendChild(new El('a', { class: 'soundTitle__title', href: '/artist/track-one?in=x' }));
    const play = item.appendChild(new El('a', { class: 'sc-button-play playButton', title: 'Play', role: 'button' }));
    play.rect = { left: 100, top: 200, width: 40, height: 40 };
    await hover(play, 110, 210);
    assert.strictEqual(overlay().style.display, 'block', 'button next to the play control');
    assert.strictEqual(overlay().style.left, '146px');
    await clickBtn();
    const ml = sent.find(m => m.action === 'mediaList');
    assert.deepStrictEqual([ml.pageUrl, ml.kind], ['https://soundcloud.com/artist/track-one', 'audio'], 'SoundCloud: track link probed for audio'); n++;
  }
  // ── generic site: play control without any audio seen -> no audio button ──
  {
    let seen = false;
    setup('https://shop.example/', { mediaSniffed: () => ({ items: [], audioSeen: seen }) });
    const play = doc.body.appendChild(new El('button', { 'aria-label': 'Play' })); play.rect = { left: 10, top: 10, width: 40, height: 40 };
    await hover(play, 20, 20);
    assert.ok(!overlay() || overlay().style.display !== 'block', 'no audio button without audio'); n++;
  }
  {
    // ...a tab the background saw loading audio: the play control gets one
    setup('https://radio.example/', { mediaSniffed: () => ({ items: [], audioSeen: true }) });
    const play = doc.body.appendChild(new El('button', { 'aria-label': 'Play episode' })); play.rect = { left: 10, top: 10, width: 40, height: 40 };
    await hover(play, 20, 20); await tick(20); await hover(play, 21, 20);
    assert.strictEqual(overlay() && overlay().style.display, 'block', 'audio seen: play control gets the audio button'); n++;
  }
  // ── a video that starts playing (no hover) gets the button ──
  {
    setup('https://x.com/NASA/status/5', {});
    const v = doc.body.appendChild(new El('video', { src: 'blob:x' })); v.rect = big;
    doc.dispatch('play', { target: v });
    assert.strictEqual(overlay().style.display, 'block', 'playing video gets the button'); n++;
  }
  // ── slow quality list: close and reopen while it loads -> ONE list, not two ──
  {
    const slow = { status: 'ok', title: 'T', probeUrl: 'https://x.com/a/status/7', formats: [
      { label: 'Best (1080p MP4)', kind: 'video', format: 'bv*+ba/b', ext: 'mp4' },
      { label: 'Audio M4A', kind: 'audio', format: 'x:m4a|ba/b', ext: 'm4a' }] };
    const { ctx } = setup('https://x.com/a/status/7', { mediaSniffed: { items: [] } });
    const pending = [];
    ctx.chrome.runtime.sendMessage = (m, cb) => {
      if (m.action === 'mediaList') pending.push(() => cb(slow));          // answered later
      else setTimeout(() => cb(m.action === 'mediaSniffed' ? { items: [] } : null), 0);
    };
    const v = doc.body.appendChild(new El('video', { src: 'blob:x' })); v.rect = big;
    await hover(v);
    await clickBtn();                       // open #1 (list pending)
    await clickBtn();                       // close
    await clickBtn();                       // open #2 (list pending)
    pending.forEach(f => f());              // both answers arrive
    for (let i = 0; i < 8; i++) await tick();
    assert.deepStrictEqual(rows(), ['Best (1080p MP4)', 'Audio M4A'], 'reopened menu shows the list once'); n++;
  }
  // ── Bug A: hover prefetch only after the pointer rests on the button (500 ms) ──
  {
    const { sent } = setup('https://x.com/a/status/8', { mediaSniffed: { items: [] }, mediaPrefetch: { ok: true } });
    const v = doc.body.appendChild(new El('video', { src: 'blob:x' })); v.rect = big;
    await hover(v);
    const b = overlay().shadow.children[1];
    const pre = () => sent.filter(m => m.action === 'mediaPrefetch').length;
    b.dispatch('mouseenter'); await tick(200);
    assert.strictEqual(pre(), 0, 'no prefetch on a bare mouseenter'); n++;
    b.dispatch('mouseleave'); await tick(500);
    assert.strictEqual(pre(), 0, 'pointer left before 500 ms: no prefetch'); n++;
    b.dispatch('mouseenter'); await tick(650);
    assert.strictEqual(pre(), 1, 'pointer rested >= 500 ms: one prefetch');
    assert.strictEqual(sent.filter(m => m.action === 'mediaPrefetch')[0].pageUrl, 'https://x.com/a/status/8'); n++;
    b.dispatch('mouseleave'); b.dispatch('mouseenter'); await tick(100); await clickBtn(); await tick(600);
    assert.strictEqual(pre(), 1, 'a click inside the dwell cancels the pending prefetch'); n++;
  }
  // ── Bug A: a timed-out probe says so, not "No downloadable video found" ──
  {
    setup('https://x.com/a/status/9', { mediaSniffed: { items: [] }, mediaList: { status: 'error', error: 'timeout' } });
    const v = doc.body.appendChild(new El('video', { src: 'blob:x' })); v.rect = big;
    await hover(v); await clickBtn();
    assert.deepStrictEqual(rows(), ['Timed out — try again'], 'timeout text'); n++;
  }
  {
    setup('https://x.com/a/status/10', { mediaSniffed: { items: [] }, mediaList: { status: 'error', error: 'no_media' } });
    const v = doc.body.appendChild(new El('video', { src: 'blob:x' })); v.rect = big;
    await hover(v); await clickBtn();
    assert.deepStrictEqual(rows(), ['No downloadable video found'], 'no_media text unchanged'); n++;
  }
  // ── phase 3 ──────────────────────────────────────────────────────────────
  const menuEl = () => overlay().shadow.children[2];
  const btnEl = () => overlay().shadow.children[1];
  // A. icon-only button, tooltip / aria-label, photo glyph
  {
    setup('https://site.example/v', { mediaSniffed: { items: [] } });
    const v = doc.body.appendChild(new El('video', { src: 'blob:x' })); v.rect = big;
    await hover(v);
    const b = btnEl();
    assert.ok(b.classList.contains('i') && !b.classList.contains('ph'), 'video: icon-only download arrow'); n++;
    assert.strictEqual(b.getAttribute('aria-label'), 'Download with NDM', 'aria-label for screen readers'); n++;
    assert.deepStrictEqual([overlay().style.left, overlay().style.top], [(700 - 8 - 96) + 'px', '108px'], 'tucked 8 px into the top-right corner'); n++;
    const img = doc.body.appendChild(new El('img', { src: 'https://img.example/p.jpg', naturalWidth: '900' }));
    img.rect = { left: 800, top: 100, width: 600, height: 400 };
    await hover(img, 900, 200);
    assert.ok(b.classList.contains('ph') && b.classList.contains('i'), 'photo: same button, image glyph'); n++;
  }
  // A. quality menu stays inside the viewport (video whose right edge is near the left edge / near the bottom)
  {
    const { ctx } = setup('https://site.example/v', { mediaSniffed: { items: [] },
      mediaList: { status: 'ok', probeUrl: 'p', formats: fmts.formats } });
    const v = doc.body.appendChild(new El('video', { src: 'blob:x' })); v.rect = { left: -400, top: 100, width: 600, height: 400 };
    await hover(v, 100, 200);
    const hostLeft = parseFloat(overlay().style.left);
    Object.defineProperty(menuEl(), 'offsetWidth', { value: 280, configurable: true });
    Object.defineProperty(menuEl(), 'offsetHeight', { value: 300, configurable: true });
    await clickBtn();
    const ml = parseFloat(menuEl().style.left);
    assert.ok(hostLeft + ml >= 8 && hostLeft + ml + 280 <= ctx.innerWidth - 8,
              'menu clamped inside the viewport on the left (was off-screen): ' + (hostLeft + ml)); n++;
    await clickBtn();                                             // close
    const low = doc.body.appendChild(new El('video', { src: 'blob:y' })); low.rect = { left: 300, top: 700, width: 600, height: 190 };
    await hover(low, 500, 800);
    await clickBtn();
    const hostTop = parseFloat(overlay().style.top), mt = parseFloat(menuEl().style.top);
    assert.ok(hostTop + mt >= 8 && hostTop + mt + 300 <= ctx.innerHeight - 8, 'near the bottom: the menu opens upwards, inside the viewport'); n++;
  }
  // A. <html> with a CSS filter (Dark Reader) re-bases position:fixed: the host is measured and corrected
  {
    const origRect = El.prototype.getBoundingClientRect;
    El.prototype.getBoundingClientRect = function () {
      if (this.tagName !== 'NDM-OVERLAY') return origRect.call(this);
      const l = parseFloat(this.style.left) || 0, t = parseFloat(this.style.top) || 0;
      return { left: l, top: t - 300, width: 30, height: 30, right: l + 30, bottom: t - 270 };   // html scrolled 300 px, filtered
    };
    try {
      setup('https://site.example/v', { mediaSniffed: { items: [] } },
        { getComputedStyle: (el) => ({ filter: el.tagName === 'HTML' ? 'invert(1) hue-rotate(180deg)' : 'none', transform: 'none' }) });
      const v = doc.body.appendChild(new El('video', { src: 'blob:x' })); v.rect = big;
      await hover(v);
      assert.strictEqual(overlay().style.top, '408px', 'filtered <html>: fixed offset measured and corrected (button stays on the video)'); n++;
      setup('https://site.example/v', { mediaSniffed: { items: [] } }, { getComputedStyle: () => ({ filter: 'none', transform: 'none' }) });
      const v2 = doc.body.appendChild(new El('video', { src: 'blob:x' })); v2.rect = big;
      await hover(v2);
      assert.strictEqual(overlay().style.top, '108px', 'no filter / transform: no correction'); n++;
    } finally { El.prototype.getBoundingClientRect = origRect; }
  }
  // B. X: the 2nd media of a post probes /status/ID/video/2; single-video and quoted posts unchanged
  {
    const xpost = (cells, quoted) => {
      const art = doc.body.appendChild(new El('article'));
      const t = art.appendChild(new El('a', { href: '/u/status/1900000000000000001' })); t.appendChild(new El('time'));
      const grid = (quoted ? art.appendChild(new El('div', { role: 'link' })) : art).appendChild(new El('div'));
      let vid = null;
      for (const c of cells) {
        if (c === 'photo') grid.appendChild(new El('div', { 'data-testid': 'tweetPhoto' })).appendChild(new El('img', { src: 'https://pbs.twimg.com/media/A?format=jpg' }));
        else {
          const vp = grid.appendChild(new El('div', { 'data-testid': 'videoPlayer' }));
          const v = vp.appendChild(new El('div', { 'data-testid': 'videoComponent' })).appendChild(new El('video', { src: 'blob:https://x.com/' + cells.indexOf(c) }));
          if (c === 'VIDEO') vid = v;
        }
      }
      vid.rect = big;
      return vid;
    };
    for (const [cells, quoted, want, why] of [
      [['photo', 'VIDEO'], false, '/video/2', 'photo + video: the video is media #2'],
      [['video', 'VIDEO', 'video'], false, '/video/2', 'three videos: the middle one'],
      [['VIDEO', 'video'], false, '/video/1', 'first of two'],
      [['VIDEO'], false, '', 'single video: plain status URL'],
      [['video', 'VIDEO'], true, '', 'video in a quoted post: not this status\' media'],
    ]) {
      const { sent } = setup('https://x.com/home', { mediaSniffed: { items: [] }, mediaList: fmts });
      const v = xpost(cells, quoted);
      await hover(v); await clickBtn();
      const ml = sent.find(m => m.action === 'mediaList');
      assert.strictEqual(ml && ml.pageUrl, 'https://x.com/u/status/1900000000000000001' + want, why); n++;
    }
  }
  // C. the sniffed-files request says which element it is for
  {
    const { sent } = setup('https://x.com/home', { mediaSniffed: { items: [] }, mediaList: fmts });
    const art = doc.body.appendChild(new El('article'));
    art.appendChild(new El('a', { href: '/u/status/1900000000000000001' })).appendChild(new El('time'));
    const vc = art.appendChild(new El('div', { 'data-testid': 'videoComponent' }));
    vc.appendChild(new El('img', { src: 'https://pbs.twimg.com/amplify_video_thumb/2125048626397478912/img/a.jpg' }));
    const v = vc.appendChild(new El('video', { src: 'blob:https://x.com/1' })); v.rect = big;
    doc.body.appendChild(new El('video', { src: 'blob:https://x.com/2' }));           // another post's player
    await hover(v); await clickBtn();
    const s = sent.find(m => m.action === 'mediaSniffed' && m.kind === 'video');
    assert.deepStrictEqual([s.mediaId, s.single], ['2125048626397478912', false], 'media id + "not the only video" sent'); n++;
  }
  // F2. a quick move onto a video inside the 120 ms throttle is still evaluated (trailing call)
  {
    setup('https://site.example/v', { mediaSniffed: { items: [] } });
    const v = doc.body.appendChild(new El('video', { src: 'blob:x' })); v.rect = big;
    doc.stack = []; await tick(130);
    doc.dispatch('mousemove', { clientX: 20, clientY: 20 });          // over nothing
    doc.stack = [v];
    doc.dispatch('mousemove', { clientX: 150, clientY: 150 });        // 0 ms later: throttled
    assert.ok(!overlay() || overlay().style.display !== 'block', 'throttled move not evaluated at once');
    await tick(160);
    assert.ok(overlay() && overlay().style.display === 'block', 'trailing evaluation shows the button where the pointer rests'); n++;
  }
  // H. audio rows: "Best Audio (…)" carries its size inline; a faint line separates video and audio
  {
    const list = { status: 'ok', probeUrl: 'p', formats: [
      { label: 'Best (1080p MP4)', kind: 'video', format: 'bv*+ba/b', ext: 'mp4' },
      { label: 'Best Audio (Opus 160 kbps)', kind: 'audio', format: '251', ext: 'webm', filesize: 3984589 },
      { label: 'Audio M4A 128 kbps', kind: 'audio', format: '140', ext: 'm4a', filesize: 3145728, approx: true },
      { label: 'MP3 160 kbps (converted)', kind: 'audio', format: 'x:mp3:160|251/ba/b', ext: 'mp3' }] };
    const { sent } = setup('https://www.youtube.com/watch?v=abc', { mediaSniffed: { items: [] }, mediaList: list, mediaPick: { status: 'accepted' } });
    const p = doc.body.appendChild(new El('div', { id: 'movie_player', class: 'html5-video-player' })); p.rect = big;
    const v = p.appendChild(new El('video', { src: 'blob:x' })); v.rect = big;
    await hover(v); await clickBtn();
    assert.deepStrictEqual(rows(), ['Best (1080p MP4)', 'Best Audio (Opus 160 kbps · 3.8 MB)', 'Audio M4A 128 kbps | ~3.0 MB', 'MP3 160 kbps (converted)'],
                           'audio rows rendered as NDM labels them'); n++;
    assert.ok(menuEl().children[1].classList.contains('a1') && !menuEl().children[2].classList.contains('a1'), 'one separator before the audio rows'); n++;
    menuEl().children[3].dispatch('click', { isTrusted: true }); await tick(); await tick();
    assert.deepStrictEqual([lastPick(sent).item.format, lastPick(sent).item.ext], ['x:mp3:160|251/ba/b', 'mp3']); n++;
  }
  console.log('all media/photo/audio content tests passed: ' + n + ' cases (' + file + ')');
  process.exit(0);
})().catch(e => { console.error('FAIL', e); process.exit(1); });
