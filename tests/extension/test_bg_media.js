// Background tests for media sniffing / quality lists / photo names.
//   node test_bg_media.js <background.js>
'use strict';
const J = (x) => JSON.parse(JSON.stringify(x));
const fs = require('fs'), vm = require('vm'), assert = require('assert');
const file = process.argv[2];

function load(nativeReply, extra) {
  const native = [];
  const ev = () => { const l = []; return { addListener: f => l.push(f), fire: (...a) => l.map(f => f(...a)) }; };
  const chrome = {
    runtime: { id: 'x', lastError: null, getURL: p => 'chrome-extension://x/' + p, onMessage: ev(), onInstalled: ev(), onStartup: ev(),
      connectNative: () => { const onMsg = ev(); return { onMessage: onMsg, onDisconnect: ev(), disconnect() {},
        postMessage(m) { native.push(m); const r = nativeReply(m); setTimeout(() => onMsg.fire(Object.assign({ id: m.id }, r)), 1); } }; } },
    downloads: { onCreated: ev(), onDeterminingFilename: ev() },
    storage: { local: { get: () => Promise.resolve({}), set: () => Promise.resolve() }, onChanged: ev() },
    cookies: { getAll: () => Promise.resolve(J((extra && extra.cookieJarStub) || [])) },
    notifications: { create: () => Promise.resolve('') },
    contextMenus: { removeAll: () => Promise.resolve(), create: () => {}, onClicked: ev() },
    tabs: { onUpdated: ev(), onRemoved: ev() },
    webRequest: { onResponseStarted: ev() },
  };
  const ctx = Object.assign({ console, setTimeout, clearTimeout, URL, navigator: { userAgent: 'UA' }, Promise, Date, JSON, Map, Set, chrome }, extra || {});
  vm.createContext(ctx);
  vm.runInContext(fs.readFileSync(file, 'utf8'), ctx);
  const msg = (m, sender) => new Promise(res => chrome.runtime.onMessage.fire(m, sender, res));
  const wr = (d) => chrome.webRequest.onResponseStarted.fire(Object.assign({ tabId: 7, statusCode: 200, frameId: 0 }, d));
  return { native, msg, wr, ctx };
}
const hdr = (t, len, extra) => [{ name: 'content-type', value: t }].concat(len != null ? [{ name: 'content-length', value: String(len) }] : []).concat(extra || []);
let n = 0;

(async () => {
  // ── X timeline: yt-dlp cannot read /home; the sniffed MASTER is probed, never the audio rendition ──
  {
    const B = 'https://video.twimg.com/amplify_video/1961497281439453184';
    const { native, msg, wr } = load((m) => m.action !== 'getMedia' ? { status: 'accepted' }
      : /\/pl\/[^/]+\.m3u8/.test(m.pageUrl) && !/mp4a|avc1/.test(m.pageUrl)
        ? { status: 'ok', title: 'x', formats: [{ label: 'Best (1024p MP4)', kind: 'video', format: 'bv*+ba/b', ext: 'mp4' },
                                               { label: 'Audio M4A', kind: 'audio', format: 'x:m4a|ba/b', ext: 'm4a' }] }
        : /\.m3u8/.test(m.pageUrl) ? { status: 'ok', title: 'x', formats: [{ label: 'Best quality', kind: 'video', format: 'bv*+ba/b', ext: 'mp4' }] }
        : { status: 'error', error: 'no_media', detail: 'Unsupported URL' });
    const tab = { tab: { id: 7, url: 'https://x.com/home', title: 'Home / X' }, url: 'https://x.com/home' };
    const T = 'application/x-mpegURL';
    wr({ url: B + '/pl/p7k23gjvCYKdTNrE.m3u8?tag=21', type: 'xmlhttprequest', responseHeaders: hdr(T, 900) });
    wr({ url: B + '/pl/avc1/576x1024/_FuylHvKLHhzOtaf.m3u8', type: 'xmlhttprequest', responseHeaders: hdr(T, 9000) });
    wr({ url: B + '/pl/mp4a/128000/pKQmN-8CL7sXYOE6.m3u8', type: 'xmlhttprequest', responseHeaders: hdr(T, 9000) });
    wr({ url: B + '/aud/mp4a/0/0/128000/SyclKnjzq4KcHMa8.mp4', type: 'xmlhttprequest', responseHeaders: hdr('video/mp4', 800) });
    wr({ url: B + '/vid/avc1/0/0/576x1024/To6TwiX-99LfaU1A.mp4', type: 'xmlhttprequest', responseHeaders: hdr('video/mp4', null) });
    wr({ url: B + '/aud/mp4a/0/3000/128000/oYBiEb50DqWir-Mi.m4s', type: 'xmlhttprequest', responseHeaders: hdr('video/mp4', 50000) });
    wr({ url: B + '/vid/avc1/0/3000/576x1024/FXJeoSa5ekFHmGVm.m4s', type: 'xmlhttprequest', responseHeaders: hdr('video/mp4', 900000) });
    let r = await msg({ action: 'mediaSniffed', src: '', kind: 'video' }, tab);
    assert.deepStrictEqual(r.items.length, 0, 'X: no init segment / .m4s / audio rendition offered as a file'); n++;
    assert.strictEqual(r.audioSeen, false, 'a video page with an audio rendition is not an "audio page"'); n++;
    r = await msg({ action: 'mediaList', pageUrl: 'https://x.com/home', kind: 'video' }, tab);
    assert.strictEqual(r.status, 'ok');
    assert.strictEqual(r.probeUrl, B + '/pl/p7k23gjvCYKdTNrE.m3u8?tag=21', 'master playlist probed');
    assert.ok(!native.some(m => m.action === 'getMedia' && /mp4a/.test(m.pageUrl)), 'audio rendition never probed for video');
    assert.deepStrictEqual(J(r.formats.map(f => f.label)), ['Best (1024p MP4)', 'Audio M4A']); n++;
    // audio list on the same tab: only audio entries
    r = await msg({ action: 'mediaList', pageUrl: 'https://x.com/home', kind: 'audio' }, tab);
    assert.ok(r.formats.length && r.formats.every(f => f.kind === 'audio'), 'audio menu: audio only'); n++;
  }
  // ── only an audio rendition / a video variant was seen: honest labels ──
  {
    const { msg, wr } = load((m) => m.action !== 'getMedia' ? { status: 'accepted' }
      : /\.m3u8/.test(m.pageUrl) ? { status: 'ok', title: 'x', formats: [{ label: 'Best quality', kind: 'video', format: 'bv*+ba/b', ext: 'mp4' }] }
      : { status: 'error', error: 'no_media' });
    const tab = { tab: { id: 7, url: 'https://news.example/live', title: 'Live' }, url: 'https://news.example/live' };
    wr({ url: 'https://cdn.example/s/audio_eng_128k/index.m3u8', type: 'xmlhttprequest', responseHeaders: hdr('application/vnd.apple.mpegurl', 500) });
    let r = await msg({ action: 'mediaList', pageUrl: tab.tab.url, kind: 'video' }, tab);
    assert.notStrictEqual(r.status, 'ok', 'an audio rendition is not offered as video'); n++;
    r = await msg({ action: 'mediaList', pageUrl: tab.tab.url, kind: 'audio' }, tab);
    assert.deepStrictEqual(J(r.formats.map(f => f.label)), ['Audio M4A', 'MP3 (converted)'], 'audio rendition -> audio entries, MP3 said to be converted'); n++;
    wr({ url: 'https://cdn.example/s/video/1280x720/index.m3u8', type: 'xmlhttprequest', responseHeaders: hdr('application/vnd.apple.mpegurl', 500) });
    r = await msg({ action: 'mediaList', pageUrl: tab.tab.url, kind: 'video' }, tab);
    assert.deepStrictEqual(J(r.formats.map(f => f.label)), ['Video stream'], 'a lone variant playlist is not called "Best"'); n++;
  }
  // ── sniffed whole files: kinds and labels ──
  {
    const { msg, wr } = load(() => ({ status: 'accepted' }));
    const tab = { tab: { id: 7, url: 'https://site.example/p', title: 'P' }, url: 'https://site.example/p' };
    wr({ url: 'https://cdn.example/movie.mp4', type: 'media', statusCode: 206, responseHeaders: [{ name: 'content-type', value: 'video/mp4' }, { name: 'content-range', value: 'bytes 0-1/52428800' }] });
    wr({ url: 'https://cdn.example/song.mp3', type: 'media', responseHeaders: hdr('audio/mpeg', 6e6) });
    wr({ url: 'https://cdn.example/DASH_720.mp4', type: 'xmlhttprequest', statusCode: 206, responseHeaders: [{ name: 'content-type', value: 'video/mp4' }, { name: 'content-range', value: 'bytes 0-99/9000000' }] });
    wr({ url: 'https://cdn.example/DASH_AUDIO_128.mp4', type: 'media', responseHeaders: hdr('video/mp4', 2e6) });
    wr({ url: 'https://cdn.example/v/seg-12.mp4', type: 'media', responseHeaders: hdr('video/mp4', 2e6) });
    wr({ url: 'https://cdn.example/v/full.mp4?range=0-50000', type: 'media', responseHeaders: hdr('video/mp4', 2e6) });
    wr({ url: 'https://cdn.example/v/init.mp4', type: 'media', responseHeaders: hdr('video/mp4', null) });
    let r = await msg({ action: 'mediaSniffed', src: '', kind: 'video', single: true }, tab);
    assert.deepStrictEqual(J(r.items.map(i => [i.label, i.url, i.kind])), [['MP4', 'https://cdn.example/movie.mp4', 'video']],
                           'video list: whole video file only (no XHR fragment, DASH audio, segment, range URL, init)'); n++;
    assert.strictEqual(r.audioSeen, true);
    r = await msg({ action: 'mediaSniffed', src: '', kind: 'audio' }, tab);
    assert.deepStrictEqual(J(r.items.map(i => i.label)), ['Audio MP4', 'Audio MP3'], 'audio list labelled "Audio"'); n++;
  }
  // ── photo names get a real image extension ──
  {
    const heads = [];
    const fetch = async (u, o) => { heads.push([u, o.method, o.credentials]); return { headers: { get: () => 'image/webp; q=1' } }; };
    const { native, msg } = load(() => ({ status: 'accepted' }), { fetch, AbortController });
    const tab = { tab: { id: 7, url: 'https://x.com/NASA/status/1', title: 'NASA on X' }, url: 'https://x.com/NASA/status/1' };
    await msg({ action: 'mediaPick', item: { url: 'https://pbs.twimg.com/media/GabcXYZ?format=jpg&name=orig', ext: 'jpg' }, mime: 'image/*' }, tab);
    assert.strictEqual(native.pop().filename, 'GabcXYZ.jpg', 'twimg: ?format= extension'); n++;
    await msg({ action: 'mediaPick', item: { url: 'https://images.example/p/12345', ext: 'jpg' }, mime: 'image/*' }, tab);
    const a = native.pop();
    assert.strictEqual(a.filename, '12345.webp', 'no extension: Content-Type of a HEAD request');
    assert.deepStrictEqual(heads.pop(), ['https://images.example/p/12345', 'HEAD', 'omit'], 'HEAD without cookies');
    assert.strictEqual(a.cookies, '', 'cross-site photo: no cookies'); n++;
    await msg({ action: 'mediaPick', item: { url: 'https://i.redd.it/q1w2e3r4.jpeg', ext: 'jpg' }, mime: 'image/*' }, tab);
    assert.strictEqual(native.pop().filename, 'q1w2e3r4.jpg'); n++;
    // media pick of an extraction entry keeps its format and the extension
    await msg({ action: 'mediaList', pageUrl: 'https://x.com/NASA/status/1' }, tab);
    await msg({ action: 'mediaPick', item: { label: 'Audio MP3', format: 'x:mp3|ba[acodec^=mp3]/ba/b', ext: 'mp3' }, probeUrl: 'https://x.com/NASA/status/1' }, tab);
    const m = native.filter(x => x.action === 'addDownload').pop();
    assert.deepStrictEqual([m.kind, m.mediaFormat, /\.mp3$/.test(m.filename)], ['media', 'x:mp3|ba[acodec^=mp3]/ba/b', true]); n++;
  }
  // ── X profile timeline: master not seen, other videos around -> the clicked
  //    video's variants joined with ITS best audio; every resolution, audio last ──
  {
    const B = 'https://video.twimg.com/amplify_video/2125048626397478912';
    const O = 'https://video.twimg.com/amplify_video/2125054593751187456';
    const { native, msg, wr } = load((m) => m.action !== 'getMedia' ? { status: 'accepted' }
      : /\.m3u8/.test(m.pageUrl) ? { status: 'ok', title: 'other', formats: [{ label: 'Best quality', kind: 'video', format: 'bv*+ba/b', ext: 'mp4' }] }
      : { status: 'error', error: 'no_media', detail: 'No video could be found in this tweet' });
    const tab = { tab: { id: 7, url: 'https://x.com/narendramodi', title: 'Profile / X' }, url: 'https://x.com/narendramodi' };
    const T = 'application/x-mpegURL';
    wr({ url: O + '/pl/Mstr0therVid01.m3u8?tag=14', type: 'xmlhttprequest', responseHeaders: hdr(T, 900) });
    wr({ url: O + '/pl/mp4a/64000/sJfW8Ts0AoKA4hZF.m3u8', type: 'xmlhttprequest', responseHeaders: hdr(T, 900) });
    wr({ url: B + '/pl/avc1/640x360/aaaaaaaaaaaaaaaa.m3u8', type: 'xmlhttprequest', responseHeaders: hdr(T, 900) });
    wr({ url: B + '/pl/avc1/1280x720/lijw00gmVSIjAC3x.m3u8', type: 'xmlhttprequest', responseHeaders: hdr(T, 900) });
    wr({ url: B + '/pl/mp4a/64000/bbbbbbbbbbbbbbbb.m3u8', type: 'xmlhttprequest', responseHeaders: hdr(T, 900) });
    wr({ url: B + '/pl/mp4a/128000/cccccccccccccccc.m3u8', type: 'xmlhttprequest', responseHeaders: hdr(T, 900) });
    let r = await msg({ action: 'mediaList', pageUrl: 'https://x.com/narendramodi/status/2125048700000000000', kind: 'video',
                        mediaId: '2125048626397478912' }, tab);
    assert.strictEqual(r.status, 'ok');
    assert.ok(!native.some(m => m.action === 'getMedia' && m.pageUrl.indexOf(O) === 0), 'another video is never probed'); n++;
    assert.deepStrictEqual(J(r.formats.map(f => f.label)), ['Best (720p MP4)', '360p MP4', 'Best Audio (M4A 128 kbps)',
                           'Audio M4A 64 kbps', 'MP3 128 kbps (converted)'],
                           'every resolution, best first; then the real renditions and an honest MP3'); n++;
    assert.ok(r.formats.filter(f => f.label !== 'Audio M4A 64 kbps').every(f => f.format.indexOf('pair|' + B + '/pl/mp4a/128000/') === 0),
              'joined with the best audio of this video'); n++;
    assert.ok(r.formats[3].format.indexOf('pair|' + B + '/pl/mp4a/64000/') === 0 && /\|x:m4a\|ba\/b$/.test(r.formats[3].format),
              'the 64 kbps row downloads THAT rendition, copied (no conversion)'); n++;
    assert.ok(/\|x:mp3:128\|ba\/b$/.test(r.formats[4].format), 'MP3 encoded at the stated 128 kbps'); n++;
    const low = await msg({ action: 'mediaPick', item: r.formats[3], probeUrl: r.probeUrl }, tab);
    assert.strictEqual(low.status, 'accepted', 'a sniffed lower rendition is accepted for download'); n++;
    await msg({ action: 'mediaPick', item: r.formats[1], probeUrl: r.probeUrl }, tab);
    const m = native.filter(x => x.action === 'addDownload').pop();
    assert.deepStrictEqual([m.kind, m.mediaFormat], ['media', r.formats[1].format], 'joined pick sent as is'); n++;
    // forged: audio of a playlist the tab never loaded
    const forged = 'pair|https://evil.example/a.m3u8|v=' + B + '/pl/avc1/1280x720/lijw00gmVSIjAC3x.m3u8|bv*+ba/b';
    r = await msg({ action: 'mediaPick', item: { label: 'x', format: forged, ext: 'mp4' }, probeUrl: B + '/pl/avc1/1280x720/lijw00gmVSIjAC3x.m3u8' }, tab);
    assert.strictEqual(r.error, 'bad_url', 'forged joined playlist refused'); n++;
  }
  // ── X signed-in probe fails, public one works -> download without cookies ──
  {
    const cookieJarStub = [{ name: 'auth_token', value: 'a', domain: '.x.com', path: '/', secure: true, httpOnly: true,
                             sameSite: 'no_restriction', hostOnly: false, session: true, storeId: '0' }];
    const { native, msg } = load((m) => m.action !== 'getMedia' ? { status: 'accepted' }
      : m.cookies ? { status: 'error', error: 'no_media', detail: 'Dependency: Unspecified' }
      : { status: 'ok', title: 'Clip', formats: [{ label: 'Best (1080p MP4)', kind: 'video', format: 'bv*+ba/b', ext: 'mp4' },
                                                  { label: 'Audio M4A', kind: 'audio', format: 'x:m4a|ba/b', ext: 'm4a' }] },
      { cookieJarStub });
    const tab = { tab: { id: 7, url: 'https://x.com/home', title: 'Home / X' }, url: 'https://x.com/home' };
    const r = await msg({ action: 'mediaList', pageUrl: 'https://x.com/a/status/9', kind: 'video' }, tab);
    const probes = native.filter(x => x.action === 'getMedia');
    assert.deepStrictEqual([probes.length, !!probes[0].cookies, probes[1].cookies], [2, true, ''], 'signed-in probe, then public'); n++;
    assert.deepStrictEqual(J(r.formats.map(f => f.label)), ['Best (1080p MP4)', 'Audio M4A']); n++;
    await msg({ action: 'mediaPick', item: r.formats[0], probeUrl: r.probeUrl }, tab);
    const m = native.filter(x => x.action === 'addDownload').pop();
    assert.deepStrictEqual([m.mediaFormat, m.cookies, J(m.cookieList)], ['bv*+ba/b', '', []], 'public post downloads without cookies'); n++;
  }
  // ── Bug A: clicks never wait behind hover prefetches (port pool + priority) ──
  {
    // Native replies are held until the test answers them; each port is one relay.
    const tick = (ms) => new Promise(res => setTimeout(res, ms || 5));
    const ports = [], posted = [];
    const ev = () => { const l = []; return { addListener: f => l.push(f), fire: (...a) => l.map(f => f(...a)) }; };
    const chrome = {
      runtime: { id: 'x', lastError: null, getURL: p => 'chrome-extension://x/' + p, onMessage: ev(), onInstalled: ev(), onStartup: ev(),
        connectNative: () => { const onMsg = ev(); const port = { onMessage: onMsg, onDisconnect: ev(), disconnect() { port.closed = true; },
          postMessage(m) { posted.push({ port, m }); } }; ports.push(port); return port; } },
      downloads: { onCreated: ev(), onDeterminingFilename: ev() },
      storage: { local: { get: () => Promise.resolve({}), set: () => Promise.resolve() }, onChanged: ev() },
      cookies: { getAll: () => Promise.resolve([]) },
      notifications: { create: () => Promise.resolve('') },
      contextMenus: { removeAll: () => Promise.resolve(), create: () => {}, onClicked: ev() },
      tabs: { onUpdated: ev(), onRemoved: ev() },
      webRequest: { onResponseStarted: ev() },
    };
    const ctx = { console, setTimeout, clearTimeout, URL, navigator: { userAgent: 'UA' }, Promise, Date, JSON, Map, Set, chrome };
    vm.createContext(ctx);
    vm.runInContext(fs.readFileSync(file, 'utf8'), ctx);
    const msg = (m) => new Promise(res => chrome.runtime.onMessage.fire(m, { tab: { id: 7, url: 'https://x.com/home', title: 'X' }, url: 'https://x.com/home' }, res));
    const ok = (title) => ({ status: 'ok', title, formats: [{ label: 'Best (720p MP4)', kind: 'video', format: 'bv*+ba/b', ext: 'mp4' }] });
    const gm = () => posted.filter(p => p.m.action === 'getMedia');
    const answer = (url, r) => { const p = gm().find(x => x.m.pageUrl === url && !x.done); p.done = true; p.port.onMessage.fire(Object.assign({ id: p.m.id }, r)); };
    const page = (i) => 'https://x.com/u/status/' + (100 + i);
    const until = async (f) => { for (let i = 0; i < 100 && !f(); i++) await tick(2); };

    // hover 5 videos (prefetch), then click a 6th
    for (let i = 1; i <= 5; i++) { await msg({ action: 'mediaPrefetch', pageUrl: page(i) }); await tick(2); }
    await until(() => gm().length >= 1);
    let click = msg({ action: 'mediaList', pageUrl: page(6), kind: 'video' });
    await until(() => gm().some(p => p.m.pageUrl === page(6)));
    assert.deepStrictEqual(gm().map(p => p.m.pageUrl), [page(1), page(6)],
                           'one prefetch running, the click sent at once; queued prefetches not sent'); n++;
    const p1 = gm()[0].port, p6 = gm()[1].port;
    assert.notStrictEqual(p1, p6, 'the click uses another port (relay) than the running prefetch'); n++;
    answer(page(6), ok('six'));
    let r = await click;
    assert.deepStrictEqual([r.status, r.title], ['ok', 'six'], 'click answered while the prefetch still runs'); n++;
    // prefetch 1 finishes -> only the LATEST hovered target is prefetched next (2-4 dropped)
    answer(page(1), ok('one'));
    await until(() => gm().length >= 3);
    assert.deepStrictEqual(gm().map(p => p.m.pageUrl), [page(1), page(6), page(5)], 'only the latest prefetch runs next'); n++;
    // a dropped prefetch is not cached: clicking it probes now (no wait behind prefetch 5)
    click = msg({ action: 'mediaList', pageUrl: page(3), kind: 'video' });
    await until(() => gm().some(p => p.m.pageUrl === page(3)));
    assert.ok(gm().some(p => p.m.pageUrl === page(3)), 'dropped prefetch re-probed on click'); n++;
    answer(page(3), ok('three'));
    assert.strictEqual((await click).title, 'three');
    // a click on a still-queued prefetch upgrades it (jumps ahead of the running prefetch)
    await msg({ action: 'mediaPrefetch', pageUrl: page(7) }); await tick(5);     // queued behind 5
    assert.ok(!gm().some(p => p.m.pageUrl === page(7)), 'second prefetch waits (cap 1)');
    click = msg({ action: 'mediaList', pageUrl: page(7), kind: 'video' });
    await until(() => gm().some(p => p.m.pageUrl === page(7)));
    assert.ok(gm().some(p => p.m.pageUrl === page(7)), 'queued prefetch upgraded to a click and sent'); n++;
    answer(page(7), ok('seven'));
    assert.strictEqual((await click).title, 'seven');
    // NDM's yt-dlp timed out -> distinct "timeout" error, and asking again probes again
    click = msg({ action: 'mediaList', pageUrl: page(8), kind: 'video' });
    await until(() => gm().some(p => p.m.pageUrl === page(8)));
    answer(page(8), { status: 'error', error: 'no_media', detail: 'The site took too long to answer.', timedOut: true });
    r = await click;
    assert.deepStrictEqual([r.status, r.error], ['error', 'timeout'], 'timeout reported as timeout'); n++;
    click = msg({ action: 'mediaList', pageUrl: page(8), kind: 'video' });
    await until(() => gm().filter(p => p.m.pageUrl === page(8)).length === 2);
    assert.strictEqual(gm().filter(p => p.m.pageUrl === page(8)).length, 2, 'timed-out probe not cached'); n++;
    answer(page(8), { status: 'error', error: 'no_media', detail: 'Unsupported URL' });
    assert.strictEqual((await click).error, 'no_media', 'a real "nothing found" stays no_media'); n++;
    answer(page(5), ok('five'));
  }
  // ── phase 3 ──────────────────────────────────────────────────────────────
  const tick = (ms) => new Promise(res => setTimeout(res, ms || 5));
  // A browser-like storage.session shared across "worker restarts".
  const sessionArea = () => {
    const data = {};
    return { data, get: (k) => Promise.resolve(k in data ? { [k]: J(data[k]) } : {}),
             set: (o) => { for (const k in o) data[k] = J(o[k]); return Promise.resolve(); },
             remove: (k) => { delete data[k]; return Promise.resolve(); } };
  };
  function loadWith(nativeReply, o) {
    o = o || {};
    const native = [], menusMade = [], filters = [];
    const ev = (rec) => { const l = []; return { addListener: (f, filt) => { l.push(f); if (rec && filt) filters.push(filt); }, fire: (...a) => l.map(f => f(...a)) }; };
    const chrome = {
      runtime: { id: 'x', lastError: null, getURL: p => 'chrome-extension://x/' + p, onMessage: ev(), onInstalled: ev(), onStartup: ev(),
        connectNative: () => { const onMsg = ev(); return { onMessage: onMsg, onDisconnect: ev(), disconnect() {},
          postMessage(m) { native.push(m); const r = nativeReply(m); setTimeout(() => onMsg.fire(Object.assign({ id: m.id }, r)), 1); } }; } },
      downloads: { onCreated: ev(), onDeterminingFilename: ev() },
      storage: { local: { get: () => Promise.resolve({}), set: () => Promise.resolve() }, onChanged: ev() },
      cookies: { getAll: () => Promise.resolve([]) },
      notifications: { create: () => Promise.resolve('') },
      contextMenus: { removeAll: () => Promise.resolve(), create: (m) => { menusMade.push(m); }, onClicked: ev() },
      tabs: { onUpdated: ev(), onRemoved: ev(), create: () => Promise.resolve({}) },
      webRequest: { onResponseStarted: ev(true), onBeforeRequest: ev() },
    };
    if (o.session) chrome.storage.session = o.session;
    const ctx = { console, setTimeout, clearTimeout, URL, navigator: { userAgent: 'UA' }, Promise, Date, JSON, Map, Set, chrome };
    vm.createContext(ctx);
    vm.runInContext(fs.readFileSync(file, 'utf8'), ctx);
    const msg = (m, sender) => new Promise(res => chrome.runtime.onMessage.fire(m, sender, res));
    const wr = (d) => chrome.webRequest.onResponseStarted.fire(Object.assign({ tabId: 7, statusCode: 200, frameId: 0 }, d));
    return { native, msg, wr, chrome, menusMade, filters };
  }
  // C. X timeline: an unrelated GIF post's MP4 never shows in another video's menu
  {
    const { msg, wr } = loadWith(() => ({ status: 'accepted' }));
    const tab = { tab: { id: 7, url: 'https://x.com/home', title: 'Home / X' }, url: 'https://x.com/home', frameId: 0 };
    const MINE = '2125048626397478912';
    wr({ url: 'https://video.twimg.com/tweet_video/GabcGifPost1.mp4', type: 'media', responseHeaders: hdr('video/mp4', 900000) });
    wr({ url: 'https://video.twimg.com/amplify_video/2125054593751187456/vid/avc1/720x1280/other.mp4', type: 'media', responseHeaders: hdr('video/mp4', 4e6) });
    wr({ url: 'https://video.twimg.com/amplify_video/' + MINE + '/vid/avc1/720x1280/mine.mp4', type: 'media', responseHeaders: hdr('video/mp4', 5e6) });
    let r = await msg({ action: 'mediaSniffed', src: '', kind: 'video', mediaId: MINE, single: false }, tab);
    assert.deepStrictEqual(J(r.items.map(i => i.url)), ['https://video.twimg.com/amplify_video/' + MINE + '/vid/avc1/720x1280/mine.mp4'],
                           'only the clicked video\'s own file (media id), never the GIF / other post'); n++;
    r = await msg({ action: 'mediaSniffed', src: '', kind: 'video', single: false }, tab);
    assert.deepStrictEqual(J(r.items), [], 'MSE player on a busy feed, no media id: no stranger\'s files'); n++;
    r = await msg({ action: 'mediaSniffed', src: 'https://video.twimg.com/tweet_video/GabcGifPost1.mp4', kind: 'video', single: false }, tab);
    assert.deepStrictEqual(J(r.items.map(i => i.url)), ['https://video.twimg.com/tweet_video/GabcGifPost1.mp4'], 'the GIF itself: its own src'); n++;
    r = await msg({ action: 'mediaSniffed', src: '', kind: 'video', single: true }, Object.assign({}, tab, { frameId: 3 }));
    assert.deepStrictEqual(J(r.items), [], 'single player of ANOTHER frame: the main frame\'s files are not its own'); n++;
  }
  // D. sniffed media survive a service-worker restart (storage.session)
  {
    const session = sessionArea();
    const reply = (m) => m.action !== 'getMedia' ? { status: 'accepted' }
      : /\.m3u8/.test(m.pageUrl) ? { status: 'ok', title: 'Live show', formats: [{ label: 'Best (720p MP4)', kind: 'video', format: 'bv*+ba/b', ext: 'mp4' }] }
      : { status: 'error', error: 'no_media' };
    const tab = { tab: { id: 7, url: 'https://news.example/live', title: 'News' }, url: 'https://news.example/live' };
    const man = 'https://cdn.example/s/master.m3u8';
    let w = loadWith(reply, { session });
    w.chrome.tabs.onUpdated.fire(7, { status: 'loading', url: tab.tab.url }, tab.tab); await tick(10);
    w.wr({ url: man, type: 'xmlhttprequest', responseHeaders: hdr('application/vnd.apple.mpegurl', 500) });
    let r = await w.msg({ action: 'mediaList', pageUrl: tab.tab.url, kind: 'video' }, tab);
    assert.strictEqual(r.probeUrl, man);
    await tick(300);                                         // debounced write
    w = loadWith(reply, { session });                        // the worker was unloaded and starts again
    r = await w.msg({ action: 'mediaPick', item: { label: 'Best (720p MP4)', format: 'bv*+ba/b', ext: 'mp4' }, probeUrl: man }, tab);
    assert.strictEqual(r.status, 'accepted', 'after a restart the sniffed manifest is still allowed (no bad_url)'); n++;
    assert.strictEqual((w.native.filter(m => m.action === 'addDownload').pop() || {}).filename, 'Live show.mp4',
                       'the probe\'s title survives the restart too'); n++;
  }
  // D2. navigations: #fragment keeps, reload / another page clears (one worker life)
  {
    const session = sessionArea();
    const tab = { tab: { id: 7, url: 'https://news.example/live', title: 'News' }, url: 'https://news.example/live' };
    const man = 'https://cdn.example/s/master.m3u8';
    const w = loadWith(() => ({ status: 'accepted' }), { session });
    w.chrome.tabs.onUpdated.fire(7, { status: 'loading', url: tab.tab.url }, tab.tab); await tick(10);
    w.wr({ url: man, type: 'xmlhttprequest', responseHeaders: hdr('application/vnd.apple.mpegurl', 500) });
    let r;
    // same-document change (#fragment) keeps the list ...
    w.chrome.tabs.onUpdated.fire(7, { url: tab.tab.url + '#t=30' }, { url: tab.tab.url + '#t=30' }); await tick(10);
    w.chrome.tabs.onUpdated.fire(7, { status: 'loading', url: tab.tab.url + '#t=60' }, { url: tab.tab.url + '#t=60' }); await tick(10);
    r = await w.msg({ action: 'mediaPick', item: { label: 'x', format: 'bv*+ba/b', ext: 'mp4' }, probeUrl: man }, tab);
    assert.strictEqual(r.status, 'accepted', '#fragment navigation keeps the tab\'s media'); n++;
    // ... a reload (same address, no url in changeInfo) clears it
    w.chrome.tabs.onUpdated.fire(7, { status: 'loading' }, { url: tab.tab.url + '#t=60' }); await tick(10);
    r = await w.msg({ action: 'mediaPick', item: { label: 'x', format: 'bv*+ba/b', ext: 'mp4' }, probeUrl: man }, tab);
    assert.strictEqual(r.error, 'bad_url', 'reload clears the tab\'s media'); n++;
    await tick(300);
    assert.deepStrictEqual(session.data['ndmTab:7'] && session.data['ndmTab:7'].items, [], 'cleared in storage.session too'); n++;
    // a new page whose address has a # clears as well
    w.wr({ url: man, type: 'xmlhttprequest', responseHeaders: hdr('application/vnd.apple.mpegurl', 500) });
    w.chrome.tabs.onUpdated.fire(7, { status: 'loading', url: 'https://news.example/other#top' }, { url: 'https://news.example/other#top' }); await tick(10);
    r = await w.msg({ action: 'mediaPick', item: { label: 'x', format: 'bv*+ba/b', ext: 'mp4' }, probeUrl: man }, tab);
    assert.strictEqual(r.error, 'bad_url', 'navigation to another page with a #fragment clears'); n++;
    // closing the tab drops its record
    w.wr({ url: man, type: 'xmlhttprequest', responseHeaders: hdr('application/vnd.apple.mpegurl', 500) });
    await tick(300);
    assert.ok(session.data['ndmTab:7'], 'record written');
    w.chrome.tabs.onRemoved.fire(7); await tick(300);
    assert.ok(!('ndmTab:7' in session.data), 'tab closed: record removed'); n++;
  }
  // D'. webNavigation path
  {
    const reply = () => ({ status: 'accepted' });
    const native = [], ev = () => { const l = []; return { addListener: f => l.push(f), fire: (...a) => l.map(f => f(...a)) }; };
    const chrome = {
      runtime: { id: 'x', lastError: null, getURL: p => 'chrome-extension://x/' + p, onMessage: ev(), onInstalled: ev(), onStartup: ev(),
        connectNative: () => { const onMsg = ev(); return { onMessage: onMsg, onDisconnect: ev(), disconnect() {},
          postMessage(m) { native.push(m); setTimeout(() => onMsg.fire(Object.assign({ id: m.id }, reply(m))), 1); } }; } },
      downloads: { onCreated: ev(), onDeterminingFilename: ev() },
      storage: { local: { get: () => Promise.resolve({}), set: () => Promise.resolve() }, onChanged: ev() },
      cookies: { getAll: () => Promise.resolve([]) }, notifications: { create: () => Promise.resolve('') },
      contextMenus: { removeAll: () => Promise.resolve(), create: () => {}, onClicked: ev() },
      tabs: { onUpdated: ev(), onRemoved: ev() }, webRequest: { onResponseStarted: ev() },
      webNavigation: { onCommitted: ev() },
    };
    const ctx = { console, setTimeout, clearTimeout, URL, navigator: { userAgent: 'UA' }, Promise, Date, JSON, Map, Set, chrome };
    vm.createContext(ctx); vm.runInContext(fs.readFileSync(file, 'utf8'), ctx);
    const msg = (m, s) => new Promise(res => chrome.runtime.onMessage.fire(m, s, res));
    const tab = { tab: { id: 7, url: 'https://site.example/p', title: 'P' }, url: 'https://site.example/p' };
    const fire = () => chrome.webRequest.onResponseStarted.fire({ tabId: 7, statusCode: 200, frameId: 0, type: 'media',
      url: 'https://cdn.example/movie.mp4', responseHeaders: hdr('video/mp4', 5e6) });
    fire();
    chrome.tabs.onUpdated.fire(7, { status: 'loading' }, tab.tab);              // ignored: webNavigation decides
    chrome.webNavigation.onCommitted.fire({ tabId: 7, frameId: 2, url: 'https://ads.example/' });
    let r = await msg({ action: 'mediaSniffed', src: '', kind: 'video', single: true }, tab);
    assert.strictEqual(r.items.length, 1, 'webNavigation: a sub-frame load keeps the list'); n++;
    chrome.webNavigation.onCommitted.fire({ tabId: 7, frameId: 0, url: 'https://site.example/p', transitionType: 'reload' });
    r = await msg({ action: 'mediaSniffed', src: '', kind: 'video', single: true }, tab);
    assert.strictEqual(r.items.length, 0, 'webNavigation: top-frame commit (reload) clears'); n++;
  }
  // G. the webRequest listener only wakes for media: X's fetch()ed .m3u8 still matches
  {
    const { filters, wr, msg } = loadWith(() => ({ status: 'accepted' }));
    const glob = (p) => new RegExp('^' + p.replace(/[.+?^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*') + '$');
    const wakes = (url, type) => filters.some(f => (f.types || []).includes(type) && (f.urls || []).some(p => glob(p).test(url)));
    assert.ok(filters.length >= 2 && filters.every(f => Array.isArray(f.urls) && Array.isArray(f.types)), 'filters registered');
    assert.ok(wakes('https://video.twimg.com/amplify_video/1961497281439453184/pl/p7k23gjvCYKdTNrE.m3u8?tag=21', 'xmlhttprequest'),
              'X master playlist (fetch) wakes the worker'); n++;
    assert.ok(wakes('https://video.twimg.com/amplify_video/1/pl/mp4a/128000/pKQmN.m3u8', 'xmlhttprequest'), 'X audio rendition too'); n++;
    assert.ok(wakes('https://cdn.example/a/stream.mpd', 'xmlhttprequest') && wakes('https://cdn.example/song.mp3?x=1', 'xmlhttprequest'), 'DASH / audio files');
    assert.ok(wakes('https://cdn.example/any/path?id=5', 'media'), '<video>/<audio> loads: any address'); n++;
    assert.ok(!wakes('https://x.com/i/api/graphql/abc/TweetDetail?variables=%7B%7D', 'xmlhttprequest') &&
              !wakes('https://www.example.com/api/feed.json', 'xmlhttprequest') && !wakes('https://pbs.twimg.com/media/G.jpg', 'xmlhttprequest'),
              'ordinary fetch / XHR (API, JSON, images) never wakes it'); n++;
    assert.ok(!filters.some(f => (f.types || []).some(t => t !== 'media') && (f.urls || []).includes('https://*/*')),
              'no catch-all address for script fetches'); n++;
    // noticed through the narrowed filter: the X pair path still works end to end
    wr({ url: 'https://video.twimg.com/amplify_video/1961497281439453184/pl/p7k23gjvCYKdTNrE.m3u8?tag=21', type: 'xmlhttprequest',
         responseHeaders: hdr('application/x-mpegURL', 900) });
    const r = await msg({ action: 'mediaPick', item: { label: 'x', format: 'bv*+ba/b', ext: 'mp4' },
                          probeUrl: 'https://video.twimg.com/amplify_video/1961497281439453184/pl/p7k23gjvCYKdTNrE.m3u8?tag=21' },
                        { tab: { id: 7, url: 'https://x.com/home', title: 'X' }, url: 'https://x.com/home' });
    assert.strictEqual(r.status, 'accepted', 'sniffed X manifest usable'); n++;
  }
  // F4. "Download all links" only on the page / a selection (links and images have their own entry)
  {
    const { chrome, menusMade } = loadWith(() => ({ status: 'ok' }));
    chrome.runtime.onInstalled.fire({ reason: 'update' }); await tick(20);
    const all = menusMade.find(m => m.id === 'ndm_all');
    assert.deepStrictEqual(J(all && all.contexts), ['page', 'selection'], 'ndm_all: page + selection only'); n++;
    assert.deepStrictEqual(J(menusMade.find(m => m.id === 'ndm_link').contexts), ['link']);
  }
  // F3. Firefox manifest: one menus permission, not both aliases
  {
    const man = require('path').join(require('path').dirname(file), 'manifest.json');
    if (fs.existsSync(man)) {
      const m = JSON.parse(fs.readFileSync(man, 'utf8'));
      const p = m.permissions || [];
      assert.ok(!(p.includes('menus') && p.includes('contextMenus')), 'manifest: not both "menus" and "contextMenus"'); n++;
      if (m.manifest_version === 2) assert.ok(p.includes('menus'), 'Firefox keeps "menus"');
    }
  }
  console.log('all background media tests passed: ' + n + ' cases (' + file + ')');
  process.exit(0);
})().catch(e => { console.error('FAIL', e); process.exit(1); });
