// Mock-browser tests for src/browser_ext/*/background.js (node test_bg.js <background.js> [--expect-bug])
// Models Chrome's ordering: API calls are queued in call order and applied
// after the current task, exactly like IPC to the browser process.
'use strict';
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

function makeBrowser(opts) {
  const log = { unchecked: [], native: [], dl: [], notifications: [], suggest: 0 };
  const queue = [];
  const flushIpc = () => { while (queue.length) queue.shift()(); };
  const items = new Map();
  const ev = () => { const l = []; return { addListener: f => l.push(f), _l: l, fire: (...a) => l.map(f => f(...a)) }; };
  let lastError = null;
  const ipc = (fn) => new Promise((res, rej) => queue.push(() => { try { res(fn()); } catch (e) { rej(e); } }));

  const downloads = {
    cancel: (id) => ipc(() => { log.dl.push('cancel:' + id); const it = items.get(id); if (it && it.state === 'in_progress') it.state = 'interrupted'; }),
    search: (q) => ipc(() => [...items.values()].filter(i => i.id === q.id).map(i => Object.assign({}, i))),
    erase: (q) => ipc(() => { log.dl.push('erase:' + q.id); items.delete(q.id); return [q.id]; }),
    download: (o) => ipc(() => { log.dl.push('download:' + o.url); return 99; }),
    onCreated: ev(),
  };
  if (!opts.firefox) downloads.onDeterminingFilename = ev();

  const store = {};
  const nativeReply = opts.nativeReply || ((m) => (m.action === 'getMedia'
      ? { status: 'ok', title: 'T', formats: [{ label: '720p MP4', format: 'bv*[height<=720]+ba/b[height<=720]/b', ext: 'mp4', filesize: 1 }] }
      : { status: 'accepted' }));
  const chrome = {
    runtime: {
      id: 'ext-id',
      get lastError() { return lastError; },
      getURL: (p) => 'chrome-extension://ext-id/' + p,
      onMessage: ev(), onInstalled: ev(), onStartup: ev(),
      connectNative: () => {
        const onMsg = ev(), onDisc = ev();
        return {
          onMessage: onMsg, onDisconnect: onDisc, disconnect() {},
          postMessage(m) {
            log.native.push(m);
            // NDM uninstalled: the browser reports the missing host on the port
            if (opts.hostMissing) { setTimeout(() => onDisc.fire({ error: { message: 'Specified native messaging host not found.' } }), 1); return; }
            const r = nativeReply(m); setTimeout(() => onMsg.fire(Object.assign({ id: m.id }, r)), 1);
          },
        };
      },
    },
    downloads,
    storage: {
      local: { get: (k) => Promise.resolve({ [k]: store[k] }), set: (o) => { Object.assign(store, o); return Promise.resolve(); } },
      onChanged: ev(),
    },
    cookies: { getAll: () => Promise.resolve([{ name: 'SID', value: 'x' }]) },
    notifications: { create: (id, o) => { log.notifications.push(o.message); return Promise.resolve(id); } },
    contextMenus: { removeAll: () => Promise.resolve(), create: () => {}, onClicked: ev() },
    tabs: { create: () => Promise.resolve({}), query: () => Promise.resolve([]), onUpdated: ev(), onRemoved: ev() },
    webRequest: { onResponseStarted: ev(), onBeforeRequest: ev() },
    scripting: {},
  };
  // Chrome's suggest(): an internal API call without callback; failure is reported
  // as "Unchecked runtime.lastError".
  function suggestFor(item) {
    return () => {
      log.suggest++;
      queue.push(() => {
        const it = items.get(item.id);
        if (!it || it.state !== 'in_progress') log.unchecked.push('Download must be in progress');
      });
    };
  }
  return { chrome, log, items, flushIpc, suggestFor, store };
}

async function settle(b, ms) {
  for (let i = 0; i < 40; i++) { b.flushIpc(); await new Promise(r => setTimeout(r, (ms || 5))); b.flushIpc(); }
}

function load(file, b, firefox) {
  const ctx = { console, setTimeout, clearTimeout, URL, navigator: { userAgent: 'UA-test' }, Promise, Date, JSON, Map, Set };
  if (firefox) { ctx.browser = b.chrome; b.chrome.runtime.getBrowserInfo = () => Promise.resolve({}); }
  else ctx.chrome = b.chrome;
  vm.createContext(ctx);
  vm.runInContext(fs.readFileSync(file, 'utf8'), ctx, { filename: file });
  return ctx;
}

function newItem(b, id, name) {
  const it = { id, url: 'https://ex.com/' + name, finalUrl: 'https://ex.com/' + name, filename: name,
               state: 'in_progress', mime: '', totalBytes: 0, referrer: 'https://ex.com/' };
  b.items.set(id, it);
  return Object.assign({}, it);
}

function msg(b, m, sender) {
  return new Promise((res) => b.chrome.runtime.onMessage.fire(m, sender || { tab: { id: 7, url: 'https://site.test/watch', title: 'My: Video?' } }, res));
}

(async () => {
  const file = process.argv[2];
  const expectBug = process.argv.includes('--expect-bug');
  let b = makeBrowser({});
  load(file, b);
  await settle(b);

  // 1. captured download (zip): cancel + hand-off, never suggest() afterwards
  const it = newItem(b, 1, 'file.zip');
  const ret = b.chrome.downloads.onDeterminingFilename.fire(it, b.suggestFor(it))[0];
  await settle(b);
  if (expectBug) {
    assert.ok(b.log.unchecked.length > 0, 'old code should reproduce the unchecked lastError');
    console.log('reproduced: Unchecked runtime.lastError: ' + b.log.unchecked[0]);
    process.exit(0);
  }
  assert.strictEqual(ret, true, 'listener returns true');
  assert.deepStrictEqual(b.log.unchecked, [], 'no unchecked lastError');
  assert.strictEqual(b.log.suggest, 0, 'suggest not called for a cancelled item');
  assert.strictEqual(b.log.dl.filter(x => x === 'cancel:1').length, 1, 'cancelled once');
  const add = b.log.native.find(m => m.action === 'addDownload');
  assert.ok(add && add.url === 'https://ex.com/file.zip' && add.source === 'capture' && add.cookies === 'SID=x');

  // 2. same id fired again (duplicate event): no second cancel
  b.chrome.downloads.onDeterminingFilename.fire(it, b.suggestFor(it));
  await settle(b);
  assert.strictEqual(b.log.dl.filter(x => x === 'cancel:1').length, 1, 'not cancelled twice');
  assert.deepStrictEqual(b.log.unchecked, []);

  // 3. not captured (html): suggest exactly once
  const it2 = newItem(b, 2, 'page.html');
  b.chrome.downloads.onDeterminingFilename.fire(it2, b.suggestFor(it2));
  await settle(b);
  assert.strictEqual(b.log.suggest, 1);
  assert.deepStrictEqual(b.log.unchecked, []);

  // 4. .torrent is captured by default
  const it3 = newItem(b, 3, 'linux.torrent');
  b.chrome.downloads.onDeterminingFilename.fire(it3, b.suggestFor(it3));
  await settle(b);
  assert.ok(b.log.native.some(m => m.url === 'https://ex.com/linux.torrent'), 'torrent captured');

  // 5. NDM unreachable -> restarted in the browser, still no lastError
  b = makeBrowser({ nativeReply: () => ({ status: 'error', error: 'ndm_unreachable' }) });
  load(file, b);
  await settle(b);
  const it4 = newItem(b, 4, 'a.exe');
  b.chrome.downloads.onDeterminingFilename.fire(it4, b.suggestFor(it4));
  await settle(b);
  assert.ok(b.log.dl.includes('download:https://ex.com/a.exe'), 'fallback download');
  assert.deepStrictEqual(b.log.unchecked, []);

  // 6. Firefox: onCreated twice for one id -> one cancel
  b = makeBrowser({ firefox: true });
  load(file, b, true);
  await settle(b);
  const f = newItem(b, 5, 'b.zip');
  b.chrome.downloads.onCreated.fire(f);
  b.chrome.downloads.onCreated.fire(f);
  await settle(b);
  assert.strictEqual(b.log.dl.filter(x => x === 'cancel:5').length, 1, 'firefox cancelled once');

  // 7. media (content-script messages are page-controlled input)
  b = makeBrowser({});
  load(file, b);
  await settle(b);
  const yt = { tab: { id: 7, url: 'https://www.youtube.com/watch?v=abc', title: 'Tab Title - YouTube' }, url: 'https://www.youtube.com/watch?v=abc' };
  let r = await msg(b, { action: 'mediaList', pageUrl: 'https://www.youtube.com/watch?v=abc' }, yt);
  assert.strictEqual(r.status, 'ok');
  assert.strictEqual(r.probeUrl, 'https://www.youtube.com/watch?v=abc');
  const gm = b.log.native.find(m => m.action === 'getMedia');
  assert.ok(gm && gm.cookies === 'SID=x' && gm.userAgent === 'UA-test' && gm.cookieStore === 'default');
  assert.ok(Array.isArray(gm.cookieList) && gm.cookieList[0].name === 'SID' && 'hostOnly' in gm.cookieList[0] && 'httpOnly' in gm.cookieList[0],
            'structured cookies sent for yt-dlp');
  // page of another site from this tab -> refused, no probe
  const nProbe = b.log.native.filter(m => m.action === 'getMedia').length;
  r = await msg(b, { action: 'mediaList', pageUrl: 'https://bank.example/account' }, yt);
  assert.strictEqual(r.error, 'bad_url', 'cross-site pageUrl refused');
  await msg(b, { action: 'mediaPrefetch', pageUrl: 'https://bank.example/account' }, yt);
  await settle(b);
  assert.strictEqual(b.log.native.filter(m => m.action === 'getMedia').length, nProbe, 'no probe for cross-site page');
  r = await msg(b, { action: 'mediaList', pageUrl: 'file:///C:/x' }, yt);
  assert.strictEqual(r.probeUrl, 'https://www.youtube.com/watch?v=abc', 'non-http pageUrl -> tab URL');
  assert.ok(!b.log.native.some(m => m.action === 'getMedia' && /^file:/.test(m.pageUrl)));
  // pick: file name derived in the background (probe title), content filename/title ignored
  r = await msg(b, { action: 'mediaPick', item: { label: '720p MP4', format: 'bv*[height<=720]+ba/b[height<=720]/b', ext: 'mp4' },
                     probeUrl: 'https://www.youtube.com/watch?v=abc', title: '..\\..\\evil', filename: 'evil.exe' }, yt);
  let a = b.log.native.filter(m => m.action === 'addDownload').pop();
  assert.strictEqual(r.status, 'accepted');
  assert.deepStrictEqual([a.kind, a.pageUrl, a.url, a.mediaFormat, a.qualityLabel, a.filename],
    ['media', 'https://www.youtube.com/watch?v=abc', 'https://www.youtube.com/watch?v=abc',
     'bv*[height<=720]+ba/b[height<=720]/b', '720p MP4', 'T.mp4']);
  assert.ok(Array.isArray(a.cookieList) && a.cookieList.length === 1, 'media add carries structured cookies');
  r = await msg(b, { action: 'mediaPick', item: { format: 'b' }, probeUrl: 'https://bank.example/x' }, yt);
  assert.strictEqual(r.error, 'bad_url', 'cross-site probeUrl refused');
  // photo from a CDN of another site: plain download, NO cookies, name from URL
  const ig = { tab: { id: 8, url: 'https://www.instagram.com/p/xyz/', title: 'IG' }, url: 'https://www.instagram.com/p/xyz/' };
  await msg(b, { action: 'mediaPick', item: { url: 'https://scontent.example-cdn.net/v/t51/123_n.jpg?stp=x', ext: 'jpg' }, filename: 'x.exe' }, ig);
  a = b.log.native.filter(m => m.action === 'addDownload').pop();
  assert.ok(!a.kind && a.filename === '123_n.jpg' && a.cookies === '', 'cross-site photo: no cookies, derived name');
  // same-site direct file: cookies
  await msg(b, { action: 'mediaPick', item: { url: 'https://cdn.instagram.com/v/a.mp4', ext: 'mp4' } }, ig);
  a = b.log.native.filter(m => m.action === 'addDownload').pop();
  assert.strictEqual(a.cookies, 'SID=x', 'same-site direct file keeps cookies');
  // legacy message types are gone
  for (const act of ['mediaDownload', 'downloadMedia']) {
    r = await msg(b, { action: act, url: 'https://ex.com/a.zip', item: { url: 'https://ex.com/a.zip' } }, yt);
    assert.strictEqual(r.error, 'forbidden', act + ' removed');
  }
  // extension-only actions refused from content scripts
  r = await msg(b, { action: 'addUrl', url: 'https://ex.com/a.zip' }, yt);
  assert.strictEqual(r.error, 'forbidden');
  // magnet + rate limit
  r = await msg(b, { action: 'magnet', url: 'magnet:?xt=urn:btih:abc' }, yt);
  a = b.log.native.filter(m => m.action === 'addDownload').pop();
  assert.strictEqual(a.url, 'magnet:?xt=urn:btih:abc');
  r = await msg(b, { action: 'magnet', url: 'magnet:?xt=urn:btih:def' }, yt);
  assert.strictEqual(r.error, 'rate_limited', 'second magnet within 2 s refused');
  // incognito: separate probe (cache key includes the cookie store)
  const inc = { tab: { id: 9, url: 'https://www.youtube.com/watch?v=abc', title: 'x', incognito: true }, url: 'https://www.youtube.com/watch?v=abc' };
  const before = b.log.native.filter(m => m.action === 'getMedia').length;
  await msg(b, { action: 'mediaList', pageUrl: 'https://www.youtube.com/watch?v=abc' }, yt);
  assert.strictEqual(b.log.native.filter(m => m.action === 'getMedia').length, before, 'normal window: cached');
  await msg(b, { action: 'mediaList', pageUrl: 'https://www.youtube.com/watch?v=abc' }, inc);
  const g2 = b.log.native.filter(m => m.action === 'getMedia');
  assert.strictEqual(g2.length, before + 1, 'incognito: own probe');
  assert.strictEqual(g2.pop().cookieStore, '1');

  // 8. webRequest sniffing
  const wr = b.chrome.webRequest.onResponseStarted;
  const hdr = (t, len) => [{ name: 'Content-Type', value: t }, { name: 'Content-Length', value: String(len) }];
  wr.fire({ tabId: 7, url: 'https://cdn.test/v/master.m3u8', statusCode: 200, responseHeaders: hdr('application/vnd.apple.mpegurl', 500) });
  wr.fire({ tabId: 7, url: 'https://cdn.test/v/720.m3u8', statusCode: 200, responseHeaders: hdr('application/vnd.apple.mpegurl', 500) });
  wr.fire({ tabId: 7, url: 'https://cdn.test/v/seg1.ts', statusCode: 200, responseHeaders: hdr('video/mp2t', 900000) });
  wr.fire({ tabId: 7, url: 'https://cdn.test/movie.mp4', statusCode: 206, responseHeaders: [{ name: 'content-type', value: 'video/mp4' }, { name: 'content-range', value: 'bytes 0-1/52428800' }] });
  wr.fire({ tabId: 7, url: 'https://rr1.googlevideo.com/videoplayback?x', statusCode: 200, responseHeaders: hdr('video/mp4', 9e6) });
  wr.fire({ tabId: 7, url: 'https://cdn.test/click.mp3', statusCode: 200, responseHeaders: hdr('audio/mpeg', 2000) });
  // (single: the frame's only <video> — sniffed files of the tab are then its own)
  r = await msg(b, { action: 'mediaSniffed', src: '', single: true });
  assert.deepStrictEqual(JSON.parse(JSON.stringify(r.items.map(i => [i.label, i.url, i.filesize]))), [['MP4', 'https://cdn.test/movie.mp4', 52428800]]);
  // the exact media the tab loaded (other site) keeps its cookies
  await msg(b, { action: 'mediaPick', item: { url: 'https://cdn.test/movie.mp4', ext: 'mp4' } });
  a = b.log.native.filter(m => m.action === 'addDownload').pop();
  assert.strictEqual(a.cookies, 'SID=x', 'sniffed media URL keeps cookies');
  // page unknown to yt-dlp -> falls back to the sniffed master playlist (variant skipped)
  b = makeBrowser({ nativeReply: (m) => m.action !== 'getMedia' ? { status: 'accepted' }
      : (/m3u8/.test(m.pageUrl) ? { status: 'ok', title: 'hls', formats: [{ label: '1080p MP4', format: 'f', ext: 'mp4' }] }
                                 : { status: 'error', error: 'no_media', detail: 'Unsupported URL' }) });
  load(file, b);
  await settle(b);
  const wr2 = b.chrome.webRequest.onResponseStarted;
  wr2.fire({ tabId: 7, url: 'https://cdn.test/v/master.m3u8', statusCode: 200, responseHeaders: hdr('application/vnd.apple.mpegurl', 500) });
  wr2.fire({ tabId: 7, url: 'https://cdn.test/v/720.m3u8', statusCode: 200, responseHeaders: hdr('application/vnd.apple.mpegurl', 500) });
  r = await msg(b, { action: 'mediaList', pageUrl: 'https://site.test/watch' });
  assert.strictEqual(r.status, 'ok');
  assert.strictEqual(r.probeUrl, 'https://cdn.test/v/master.m3u8');
  assert.deepStrictEqual(b.log.native.filter(m => m.action === 'getMedia').map(m => m.pageUrl).join(' '),
                         'https://site.test/watch https://cdn.test/v/master.m3u8');
  r = await msg(b, { action: 'mediaPick', item: { label: '1080p MP4', format: 'f', ext: 'mp4' }, probeUrl: 'https://cdn.test/v/master.m3u8' });
  assert.strictEqual(r.status, 'accepted', 'sniffed manifest accepted as probeUrl');
  assert.strictEqual(b.log.native.filter(m => m.action === 'addDownload').pop().filename, 'hls.mp4');
  assert.deepStrictEqual(b.log.unchecked, []);

  // 9. Bug B: intercept only when NDM can take the download (cached ping)
  const ndm = (ping, add) => (m) => m.action === 'ping' ? ping : m.action === 'addDownload' ? (add || { status: 'accepted' }) : { status: 'ok' };
  const PING_YES = { status: 'ok', app: 'NDM', version: '1.0.1', running: true, capture: true };
  const chromeDl = async (bb, it) => { bb.chrome.downloads.onDeterminingFilename.fire(it, bb.suggestFor(it)); await settle(bb); };
  const ffDl = async (bb, it) => { bb.chrome.downloads.onCreated.fire(it); await settle(bb); };
  const untouched = (bb, id, why) => {
    assert.ok(!bb.log.dl.some(x => x === 'cancel:' + id || x === 'erase:' + id), why + ': not cancelled');
    assert.ok(!bb.log.dl.some(x => /^download:/.test(x)), why + ': not restarted');
    assert.ok(!bb.log.native.some(m => m.action === 'addDownload'), why + ': not sent to NDM');
  };
  const pings = (bb) => bb.log.native.filter(m => m.action === 'ping').length;
  const ext = { url: 'chrome-extension://ext-id/popup.html' };
  let x;
  // 9a. NDM uninstalled: the browser keeps the download, nothing shown
  b = makeBrowser({ hostMissing: true }); load(file, b); await settle(b);
  x = newItem(b, 20, 'setup.exe'); await chromeDl(b, x);
  untouched(b, 20, 'host missing');
  assert.strictEqual(b.log.suggest, 1, 'host missing: suggest() once');
  assert.deepStrictEqual(b.log.notifications, [], 'host missing: no "Reinstall NDM" notification');
  const p0 = pings(b);
  x = newItem(b, 21, 'b.zip'); await chromeDl(b, x);
  untouched(b, 21, 'host missing (cached)');
  assert.strictEqual(pings(b), p0, 'state cached: no ping per download');
  // explicit sends still report it, once per browser session
  await msg(b, { action: 'addUrl', url: 'https://ex.com/a.zip' }, ext);
  await msg(b, { action: 'addUrl', url: 'https://ex.com/b.zip' }, ext);
  await settle(b);
  assert.strictEqual(b.log.notifications.length, 1, '"not installed" notified once');
  assert.deepStrictEqual(b.log.unchecked, []);
  b = makeBrowser({ hostMissing: true, firefox: true }); load(file, b, true); await settle(b);
  x = newItem(b, 22, 'c.zip'); await ffDl(b, x);
  untouched(b, 22, 'firefox host missing');
  // 9b. capture switched off in NDM (ping capture:false)
  b = makeBrowser({ nativeReply: ndm(Object.assign({}, PING_YES, { capture: false })) }); load(file, b); await settle(b);
  x = newItem(b, 23, 'd.zip'); await chromeDl(b, x);
  untouched(b, 23, 'capture off');
  assert.strictEqual(b.log.suggest, 1);
  // 9c. capture on: normal path unchanged (cancel once, sent once, no restart)
  b = makeBrowser({ nativeReply: ndm(PING_YES) }); load(file, b); await settle(b);
  x = newItem(b, 24, 'e.zip'); await chromeDl(b, x);
  assert.strictEqual(b.log.dl.filter(d => d === 'cancel:24').length, 1, 'capture on: cancelled once');
  assert.strictEqual(b.log.native.filter(m => m.action === 'addDownload' && m.url === 'https://ex.com/e.zip').length, 1, 'sent once');
  assert.ok(!b.log.dl.some(d => /^download:/.test(d)) && b.log.suggest === 0, 'no duplicate, no suggest after cancel');
  // 9d. POST / form results are never intercepted
  const wrb = b.chrome.webRequest.onBeforeRequest;
  wrb.fire({ method: 'POST', url: 'https://ex.com/export.zip#x', type: 'main_frame', tabId: 3 });
  x = newItem(b, 25, 'export.zip'); await chromeDl(b, x);
  assert.ok(!b.log.dl.includes('cancel:25') && !b.log.native.some(m => m.url === 'https://ex.com/export.zip'), 'POST result: browser keeps it');
  wrb.fire({ method: 'POST', url: 'https://ex.com/form.php', type: 'sub_frame', tabId: 3 });
  x = newItem(b, 26, 'report.pdf'); b.items.get(26).finalUrl = x.finalUrl = 'https://files.ex.com/signed/report.pdf?sig=1';
  b.items.get(26).url = x.url = 'https://ex.com/form.php';
  await chromeDl(b, x);
  assert.ok(!b.log.dl.includes('cancel:26'), 'POST redirected to a signed file: browser keeps it');
  x = newItem(b, 27, 'r.zip'); b.items.get(27).referrer = x.referrer = 'https://ex.com/form.php';
  await chromeDl(b, x);
  // A plain link on a page that was loaded by POST is an ordinary GET: NDM takes it.
  assert.ok(b.log.dl.includes('cancel:27'), 'GET link on a POST result page: still captured');
  wrb.fire({ method: 'GET', url: 'https://ex.com/get.zip', type: 'main_frame', tabId: 3 });
  x = newItem(b, 28, 'get.zip'); await chromeDl(b, x);
  assert.ok(b.log.dl.includes('cancel:28'), 'GET navigation still captured');
  assert.deepStrictEqual(b.log.unchecked, []);
  // 9e. Chrome incognito ("spanning"): no definite yes -> untouched; failure after a yes -> no restart
  b = makeBrowser({ nativeReply: ndm({ status: 'error', error: 'timeout' }) }); load(file, b); await settle(b);
  x = newItem(b, 29, 'p.zip'); b.items.get(29).incognito = x.incognito = true;
  await chromeDl(b, x);
  untouched(b, 29, 'incognito, state unknown');
  x = newItem(b, 30, 'q.zip'); await chromeDl(b, x);
  assert.ok(b.log.dl.includes('cancel:30'), 'normal window, state unknown: captured as before');
  b = makeBrowser({ nativeReply: ndm(PING_YES, { status: 'error', error: 'ndm_unreachable' }) }); load(file, b); await settle(b);
  x = newItem(b, 31, 'i.zip'); b.items.get(31).incognito = x.incognito = true;
  await chromeDl(b, x);
  assert.ok(b.log.dl.includes('cancel:31'), 'incognito + yes: captured');
  assert.ok(!b.log.dl.some(d => /^download:/.test(d)), 'incognito failure: never restarted in the normal profile');
  assert.strictEqual(b.log.notifications.length, 1, 'incognito failure: one notification');
  x = newItem(b, 32, 'n.zip'); await chromeDl(b, x);
  assert.ok(b.log.dl.includes('download:https://ex.com/n.zip'), 'normal window failure: restarted in the browser');
  b = makeBrowser({ firefox: true, nativeReply: ndm({ status: 'error', error: 'timeout' }) }); load(file, b, true); await settle(b);
  x = newItem(b, 33, 'f.zip'); b.items.get(33).incognito = x.incognito = true;
  await ffDl(b, x);
  untouched(b, 33, 'firefox private, state unknown');
  assert.deepStrictEqual(b.log.unchecked, []);
  console.log('all background tests passed (' + file + ')');
  process.exit(0);
})().catch(e => { console.error('FAIL', e); process.exit(1); });
