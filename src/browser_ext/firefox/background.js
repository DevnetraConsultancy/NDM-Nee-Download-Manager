/**
 * Nee Download Manager — browser integration (background)
 *
 * One source for Chrome/Edge (MV3 service worker) and Firefox (MV2 event
 * page); keep src/browser_ext/chrome/background.js and firefox/background.js
 * identical.
 *
 * - Captures browser downloads (Chrome: downloads.onDeterminingFilename,
 *   Firefox: downloads.onCreated), cancels them and hands them to NDM with
 *   cookies / referrer / user agent through the native host
 *   "com.devnetra.ndm" (NDMMsgHost.exe). Only when a cached ping says NDM
 *   can take it (installed, capture on); never POST / form results. If NDM
 *   still cannot be reached a normal-window download is restarted in the
 *   browser; a private-window one never is (it would land in the normal
 *   profile).
 * - Video / audio: the content script's overlay asks for a quality list
 *   ("getMedia" -> yt-dlp in NDM) and sends the pick as kind:"media".
 *   Whole media files and HLS/DASH manifests the page loads are noticed with
 *   webRequest (observation only): files are offered in the same list (never
 *   segments, and audio only ever as "Audio"), manifests are the yt-dlp
 *   fallback for pages it does not know (master playlists first).
 * - Photos: the overlay sends the largest source as a normal download; the
 *   name gets a real image extension here (URL, ?format=, HEAD Content-Type).
 * - Context menus: Download with NDM (link / image / video / audio),
 *   Download all links with NDM (opens a picker).
 * - magnet: links clicked on pages (content script) go to NDM.
 *
 * The service worker can be killed at any time: settings live in
 * storage.local, the native ports are (re)opened lazily and closed when idle.
 */
'use strict';

const api = (typeof browser !== 'undefined' && browser.runtime) ? browser : chrome;
const IS_FIREFOX = typeof browser !== 'undefined' && !!browser.runtime && typeof browser.runtime.getBrowserInfo === 'function';

const HOST = 'com.devnetra.ndm';
const ICON = 'images/ndm128.png';
const SETTINGS_KEY = 'ndmSettings';
const HISTORY_KEY  = 'ndmHistory';
const PICKER_KEY   = 'ndmPicker';
const HISTORY_MAX  = 60;
const IDLE_DISCONNECT_MS = 120000;
const MEDIA_TIMEOUT_MS   = 75000;     // yt-dlp -J takes 5-20 s, longer on slow sites
const MEDIA_CACHE_MS     = 4 * 60000;

const DEFAULTS = {
  enabled: true,
  extensions: [
    'zip rar 7z tar gz tgz bz2 tbz xz txz zst cab arj lzh',
    'exe msi msix msixbundle appx appxbundle apk xapk aab dmg pkg deb rpm jar',
    'iso img bin cue nrg vhd vhdx torrent',
    'mp4 m4v mkv avi mov wmv flv f4v webm mpg mpeg 3gp ts m2ts mts vob ogv',
    'mp3 m4a aac flac wav ogg oga opus wma ape alac',
    'pdf doc docx xls xlsx ppt pptx odt ods odp rtf epub',
  ].join(' '),
  minSizeKB: 0,          // 0 = any size; unknown sizes are always captured
  excludedSites: [],     // host names (sub-domains included)
  videoButton: true,     // "Download" overlay on videos and large photos
  notifications: true,
};

const MIME_EXT = {
  'application/zip': 'zip', 'application/x-zip-compressed': 'zip',
  'application/x-rar-compressed': 'rar', 'application/vnd.rar': 'rar',
  'application/x-7z-compressed': '7z', 'application/gzip': 'gz', 'application/x-gzip': 'gz',
  'application/x-tar': 'tar', 'application/x-bzip2': 'bz2', 'application/x-xz': 'xz',
  'application/x-msdownload': 'exe', 'application/x-msdos-program': 'exe',
  'application/vnd.microsoft.portable-executable': 'exe',
  'application/x-msi': 'msi', 'application/x-ms-installer': 'msi',
  'application/vnd.android.package-archive': 'apk', 'application/x-apple-diskimage': 'dmg',
  'application/x-iso9660-image': 'iso', 'application/x-debian-package': 'deb',
  'application/x-rpm': 'rpm', 'application/java-archive': 'jar',
  'application/x-bittorrent': 'torrent',
  'application/pdf': 'pdf', 'application/msword': 'doc',
  'application/vnd.openxmlformats-officedocument.wordprocessingml.document': 'docx',
  'application/vnd.ms-excel': 'xls',
  'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet': 'xlsx',
  'application/vnd.ms-powerpoint': 'ppt',
  'application/vnd.openxmlformats-officedocument.presentationml.presentation': 'pptx',
  'application/epub+zip': 'epub',
  'video/mp4': 'mp4', 'video/x-matroska': 'mkv', 'video/webm': 'webm', 'video/quicktime': 'mov',
  'video/x-msvideo': 'avi', 'video/x-ms-wmv': 'wmv', 'video/x-flv': 'flv', 'video/mpeg': 'mpg',
  'video/3gpp': '3gp', 'video/mp2t': 'ts',
  'audio/mpeg': 'mp3', 'audio/mp4': 'm4a', 'audio/x-m4a': 'm4a', 'audio/aac': 'aac',
  'audio/flac': 'flac', 'audio/x-flac': 'flac', 'audio/wav': 'wav', 'audio/x-wav': 'wav',
  'audio/ogg': 'ogg', 'audio/opus': 'opus', 'audio/x-ms-wma': 'wma',
};

// ─── Settings (cached; storage is the source of truth) ──────────────────────

let settingsCache = null;

async function loadSettings() {
  try {
    const st = await api.storage.local.get(SETTINGS_KEY);
    const saved = st[SETTINGS_KEY] || {};
    settingsCache = Object.assign({}, DEFAULTS, saved);
  } catch (e) {
    settingsCache = Object.assign({}, DEFAULTS);
  }
  return settingsCache;
}

async function saveSettings(patch) {
  const cur = settingsCache || await loadSettings();
  const next = Object.assign({}, cur, sanitizeSettings(patch || {}));
  settingsCache = next;
  await api.storage.local.set({ [SETTINGS_KEY]: next });
  return next;
}

function sanitizeSettings(p) {
  const o = {};
  if ('enabled' in p)       o.enabled = !!p.enabled;
  if ('videoButton' in p)   o.videoButton = !!p.videoButton;
  if ('notifications' in p) o.notifications = !!p.notifications;
  if ('minSizeKB' in p)     o.minSizeKB = Math.max(0, Math.min(1e9, parseInt(p.minSizeKB, 10) || 0));
  if ('extensions' in p)    o.extensions = String(p.extensions || '').toLowerCase()
                                 .split(/[\s,;]+/).map(x => x.replace(/^\./, '')).filter(Boolean).join(' ');
  if ('excludedSites' in p && Array.isArray(p.excludedSites))
    o.excludedSites = [...new Set(p.excludedSites.map(normHost).filter(Boolean))].slice(0, 500);
  return o;
}

api.storage.onChanged.addListener((changes, area) => {
  if (area === 'local' && changes[SETTINGS_KEY])
    settingsCache = Object.assign({}, DEFAULTS, changes[SETTINGS_KEY].newValue || {});
});

// ─── Helpers ────────────────────────────────────────────────────────────────

// http(s) only: NDM's engine (WinHTTP) has no FTP.
function isNetUrl(u) { return typeof u === 'string' && /^https?:\/\//i.test(u); }
function isMagnet(u) { return typeof u === 'string' && /^magnet:\?/i.test(u) && u.length < 65536; }

function hostOf(u) {
  try { return new URL(u).hostname.toLowerCase(); } catch (e) { return ''; }
}

// Not a duplicate of isNetUrl: also caps the length (< 8192) for URLs passed on to NDM / the cookie API.
function isHttpUrl(u) { return typeof u === 'string' && /^https?:\/\//i.test(u) && u.length < 8192; }

// Public suffixes under which unrelated parties own sibling names: the
// registrable domain ("site") is one label below the longest match. A small
// built-in list (multi-tenant hosting + common second-level ccTLDs); for any
// other two-letter ccTLD a generic second level (com, net, org, edu, gov,
// ac, co, ...) also counts as a suffix. Unknown cases err towards "not the
// same site".
const PUBLIC_SUFFIXES = new Set([
  // multi-tenant hosting
  'github.io', 'githubusercontent.com', 'gitlab.io', 'bitbucket.io', 'codeberg.page',
  'blogspot.com', 'wordpress.com', 'tumblr.com', 'wixsite.com', 'weebly.com', 'squarespace.com',
  'herokuapp.com', 'herokussl.com', 'vercel.app', 'now.sh', 'netlify.app', 'netlify.com',
  'pages.dev', 'workers.dev', 'r2.dev', 'web.app', 'firebaseapp.com', 'appspot.com',
  'azurewebsites.net', 'cloudapp.net', 'azureedge.net', 'blob.core.windows.net', 'trafficmanager.net',
  'cloudfront.net', 's3.amazonaws.com', 'elasticbeanstalk.com', 'amplifyapp.com',
  'glitch.me', 'repl.co', 'replit.dev', 'onrender.com', 'fly.dev', 'railway.app', 'surge.sh',
  'ngrok.io', 'ngrok-free.app', 'ngrok.app', 'pythonanywhere.com', 'readthedocs.io',
  'neocities.org', 'carrd.co', 'webflow.io', 'framer.app', 'deno.dev', 'stackblitz.io',
  'sharepoint.com', 'myshopify.com', 'dyndns.org', 'duckdns.org', 'no-ip.org', 'hopto.org',
  // second-level ccTLD registries
  'co.uk', 'org.uk', 'me.uk', 'ltd.uk', 'plc.uk', 'net.uk', 'ac.uk', 'gov.uk', 'nhs.uk', 'sch.uk',
  'com.au', 'net.au', 'org.au', 'edu.au', 'gov.au', 'id.au', 'asn.au',
  'co.nz', 'net.nz', 'org.nz', 'govt.nz', 'ac.nz',
  'co.jp', 'ne.jp', 'or.jp', 'ac.jp', 'go.jp', 'gr.jp', 'ed.jp',
  'co.kr', 'or.kr', 'ne.kr', 'go.kr', 'ac.kr',
  'co.in', 'net.in', 'org.in', 'firm.in', 'gen.in', 'ind.in', 'ac.in', 'gov.in', 'edu.in', 'res.in',
  'com.br', 'net.br', 'org.br', 'gov.br', 'edu.br', 'com.cn', 'net.cn', 'org.cn', 'gov.cn', 'edu.cn',
  'com.hk', 'org.hk', 'net.hk', 'edu.hk', 'gov.hk', 'com.tw', 'org.tw', 'net.tw', 'edu.tw', 'gov.tw',
  'com.sg', 'org.sg', 'net.sg', 'edu.sg', 'gov.sg', 'com.my', 'org.my', 'net.my', 'edu.my', 'gov.my',
  'co.za', 'org.za', 'gov.za', 'ac.za', 'com.mx', 'org.mx', 'gob.mx', 'edu.mx', 'com.ar', 'gob.ar',
  'com.tr', 'org.tr', 'gov.tr', 'edu.tr', 'com.pk', 'org.pk', 'gov.pk', 'edu.pk',
  'co.il', 'org.il', 'ac.il', 'gov.il', 'co.id', 'or.id', 'ac.id', 'go.id', 'web.id',
  'co.th', 'in.th', 'ac.th', 'go.th', 'or.th', 'com.vn', 'gov.vn', 'edu.vn',
  'com.ph', 'gov.ph', 'edu.ph', 'com.ua', 'org.ua', 'gov.ua', 'com.eg', 'gov.eg', 'edu.eg',
  'com.sa', 'gov.sa', 'edu.sa', 'com.ng', 'gov.ng', 'com.co', 'gov.co', 'edu.co',
  'com.pe', 'gob.pe', 'com.ve', 'com.ec', 'com.uy', 'com.bd', 'gov.bd', 'com.np', 'com.lk',
  'co.ke', 'or.ke', 'go.ke', 'ac.ke', 'co.tz', 'co.ug', 'com.gh', 'co.zw',
  'com.ru', 'org.ru', 'net.ru', 'com.pl', 'net.pl', 'org.pl', 'com.es', 'org.es', 'com.pt', 'com.gr',
  'co.at', 'or.at', 'ac.at', 'gv.at', 'com.de', 'co.it', 'gov.it', 'edu.it',
]);
const GENERIC_SLD = new Set(['com', 'net', 'org', 'edu', 'gov', 'mil', 'ac', 'co', 'or', 'ne', 'go',
                             'gob', 'gv', 'nic', 'int', 'ltd', 'plc', 'sch', 'nom', 'info', 'biz', 'web']);

/** Registrable domain (eTLD+1) of a host; the host itself when it is an IP or a public suffix. */
function siteOfHost(h) {
  h = String(h || '').toLowerCase().replace(/\.$/, '');
  if (!h || /^[\d.]+$/.test(h) || h.includes(':') || h.startsWith('[')) return h;   // IP literal: exact host
  const l = h.split('.');
  if (l.length < 2) return h;
  let suffixLen = 1;                                   // default rule: the TLD alone
  for (let i = 0; i < l.length; i++) {
    const cand = l.slice(i).join('.');
    if (PUBLIC_SUFFIXES.has(cand)) { suffixLen = l.length - i; break; }
  }
  if (suffixLen === 1 && l.length >= 3 && l[l.length - 1].length === 2 && GENERIC_SLD.has(l[l.length - 2]))
    suffixLen = 2;                                     // e.g. example.com.xx
  if (suffixLen >= l.length) return h;                 // the host is itself a public suffix
  return l.slice(l.length - suffixLen - 1).join('.');
}
function siteOf(u) { return siteOfHost(hostOf(u)); }
function schemeOf(u) { try { return new URL(u).protocol; } catch (e) { return ''; } }
function sameSite(a, b) { const s = siteOf(a); return !!s && s === siteOf(b); }
/** Schemeful same-site (what SameSite cookies use): same registrable domain and same scheme. */
function sameSiteSchemeful(a, b) { return sameSite(a, b) && schemeOf(a) === schemeOf(b); }

function normHost(h) {
  h = String(h || '').trim().toLowerCase();
  if (!h) return '';
  if (h.includes('://')) h = hostOf(h);
  return h.replace(/^\*\./, '').replace(/^www\./, '').replace(/\/.*$/, '');
}

function siteExcluded(s, host) {
  if (!host) return false;
  host = host.replace(/^www\./, '');
  return (s.excludedSites || []).some(x => host === x || host.endsWith('.' + x));
}

function baseName(p) {
  if (!p) return '';
  const parts = String(p).split(/[\\/]/);
  return parts[parts.length - 1] || '';
}

function nameFromUrl(u) {
  try {
    const seg = new URL(u).pathname.split('/').filter(Boolean).pop() || '';
    return decodeURIComponent(seg);
  } catch (e) { return ''; }
}

function extOf(name) {
  const m = /\.([a-z0-9]{1,12})$/i.exec(name || '');
  return m ? m[1].toLowerCase() : '';
}

function extSet(s) { return new Set(String(s.extensions || '').split(/\s+/).filter(Boolean)); }

function mimeExt(mime) {
  return MIME_EXT[String(mime || '').split(';')[0].trim().toLowerCase()] || '';
}

function sizeOf(item) {
  if (item.totalBytes > 0) return item.totalBytes;
  if (item.fileSize > 0) return item.fileSize;
  return 0;
}

/** Windows-safe file name from a page/video title. */
function safeName(t) {
  let s = String(t || '').replace(/[\u0000-\u001f\u007f<>:"/\\|?*]+/g, ' ').replace(/\s+/g, ' ').trim();
  s = s.replace(/[. ]+$/, '');
  if (s.length > 150) s = s.slice(0, 150).trim();
  return s || 'video';
}

function notify(title, message) {
  const s = settingsCache || DEFAULTS;
  if (!s.notifications || !api.notifications) return;
  try {
    const p = api.notifications.create('', {
      type: 'basic', iconUrl: ICON, title: title, message: String(message || '').slice(0, 300),
    });
    if (p && typeof p.catch === 'function') p.catch(() => {});
  } catch (e) { /* ignore */ }
}

async function addHistory(entry) {
  try {
    const st = await api.storage.local.get(HISTORY_KEY);
    const list = Array.isArray(st[HISTORY_KEY]) ? st[HISTORY_KEY] : [];
    list.unshift(Object.assign({ t: Date.now() }, entry));
    await api.storage.local.set({ [HISTORY_KEY]: list.slice(0, HISTORY_MAX) });
  } catch (e) { /* ignore */ }
}

// ─── Native messaging (lazy persistent ports, request/response by id) ───────
// Each port is its own NDMMsgHost.exe process and pipe connection; the relay
// answers one request at a time, in order. So: "main" for quick requests,
// "ping" for the capture-state check, and a small pool of "media" ports for
// slow getMedia probes (one probe per port at a time, see mediaRequest).

let lastHostError = '';

function classifyHostError(msg) {
  return /not found|not registered|no such native/i.test(msg || '') ? 'host_missing' : 'host_disconnected';
}

/** opts.resetOnTimeout: drop the port when a request times out (the relay
 *  would otherwise answer the stale request before any new one). */
function makeChannel(opts) {
  const resetOnTimeout = !!(opts && opts.resetOnTimeout);
  let port = null, seq = 0, idleTimer = null;
  const pending = new Map();

  function drop(error) {
    const p = port;
    port = null;
    for (const [id, w] of pending) { clearTimeout(w.timer); w.resolve({ id, status: 'error', error }); }
    pending.clear();
    if (p) { try { p.disconnect(); } catch (e) { /* ignore */ } }
  }

  function armIdle() {
    if (idleTimer) clearTimeout(idleTimer);
    idleTimer = setTimeout(() => {
      idleTimer = null;
      if (port && pending.size === 0) { try { port.disconnect(); } catch (e) {} port = null; }
    }, IDLE_DISCONNECT_MS);
  }

  function open() {
    if (port) return port;
    const p = api.runtime.connectNative(HOST);
    p.onMessage.addListener((msg) => {
      if (msg && pending.has(msg.id)) {
        const w = pending.get(msg.id);
        pending.delete(msg.id);
        clearTimeout(w.timer);
        w.resolve(msg);
      }
      armIdle();
    });
    p.onDisconnect.addListener((dp) => {
      const err = (dp && dp.error && dp.error.message) ||
                  (api.runtime.lastError && api.runtime.lastError.message) || 'Native host disconnected';
      lastHostError = err;
      noteHostError(classifyHostError(err));
      if (port === p) port = null;
      for (const [id, w] of pending) {
        if (w.port !== p) continue;      // belongs to a newer port
        pending.delete(id);
        clearTimeout(w.timer);
        w.resolve({ id, status: 'error', error: classifyHostError(err), detail: err });
      }
    });
    port = p;
    armIdle();
    return p;
  }

  return function request(msg, timeoutMs) {
    return new Promise((resolve) => {
      let p;
      try { p = open(); }
      catch (e) { resolve({ status: 'error', error: 'host_missing', detail: String(e && e.message || e) }); return; }
      const id = ++seq;
      const timer = setTimeout(() => {
        if (!pending.delete(id)) return;
        resolve({ id, status: 'error', error: 'timeout' });
        if (resetOnTimeout && port === p) drop('host_disconnected');
      }, timeoutMs || 25000);
      pending.set(id, { resolve, timer, port: p });
      try {
        p.postMessage(Object.assign({}, msg, { id }));
      } catch (e) {
        pending.delete(id);
        clearTimeout(timer);
        if (port === p) port = null;
        resolve({ id, status: 'error', error: 'host_disconnected', detail: String(e && e.message || e) });
      }
    });
  };
}

const nativeRequest = makeChannel();
const pingRequest   = makeChannel();     // never waits behind a slow addDownload

// ── getMedia pool: a click never waits behind hover prefetches ──
// Clicks go first (FIFO) on any free port; at most MEDIA_MAX_PREFETCH
// prefetches run at once, and only the latest hovered target's prefetch is
// kept: older ones still queued are dropped (error "dropped", not cached).
const MEDIA_PORTS = 3;
const MEDIA_MAX_PREFETCH = 1;
const mediaPorts = Array.from({ length: MEDIA_PORTS }, () => ({ request: makeChannel({ resetOnTimeout: true }), busy: false }));
const mediaQueue = [];           // {msg, timeoutMs, ctl, resolve, timer}
let mediaPrefetching = 0;
let latestPrefetch = null;       // ctl of the newest prefetch

const isPrefetch = (ctl) => ctl.prio === 'prefetch';

function dropJob(job) {
  clearTimeout(job.timer);
  job.ctl.dropped = true;
  job.resolve({ status: 'error', error: 'dropped' });
}

/** Drops queued (not started) prefetches other than `keep`. */
function dropQueuedPrefetches(keep) {
  for (let i = mediaQueue.length - 1; i >= 0; i--) {
    const job = mediaQueue[i];
    if (isPrefetch(job.ctl) && job.ctl !== keep) { mediaQueue.splice(i, 1); dropJob(job); }
  }
}

function pumpMedia() {
  for (;;) {
    const port = mediaPorts.find(c => !c.busy);
    if (!port || !mediaQueue.length) return;
    let i = mediaQueue.findIndex(j => !isPrefetch(j.ctl));
    if (i < 0) {
      if (mediaPrefetching >= MEDIA_MAX_PREFETCH) return;
      i = 0;
    }
    const job = mediaQueue.splice(i, 1)[0];
    clearTimeout(job.timer);
    const pre = isPrefetch(job.ctl);
    port.busy = true;
    if (pre) mediaPrefetching++;
    port.request(job.msg, job.timeoutMs).then((r) => {
      port.busy = false;
      if (pre) mediaPrefetching--;
      job.resolve(r);
      pumpMedia();
    });
  }
}

/** getMedia on a free media port. ctl: {prio:'click'|'prefetch'} shared by one probe. */
function mediaRequest(msg, timeoutMs, ctl) {
  ctl = ctl || { prio: 'click' };
  return new Promise((resolve) => {
    const job = { msg, timeoutMs, ctl, resolve, timer: null };
    if (isPrefetch(ctl) && ctl !== latestPrefetch) { dropJob(job); return; }   // superseded meanwhile
    // Waiting for a port counts against the same limit as the probe itself.
    job.timer = setTimeout(() => {
      const i = mediaQueue.indexOf(job);
      if (i >= 0) { mediaQueue.splice(i, 1); resolve({ status: 'error', error: 'timeout' }); }
    }, timeoutMs);
    mediaQueue.push(job);
    pumpMedia();
  });
}

// ── Can NDM take captures right now? (cached ping) ──
// 'yes': installed and capture on; 'no': host missing or capture off in NDM;
// 'unknown': no answer (normal windows: capture as before; private: never).
const CAPTURE_STATE_MS = 60000;
let capState = { v: 'unknown', t: 0, ttl: 0 };
let capPing = null;

function setCaptureState(v) { capState = { v, t: Date.now(), ttl: v === 'unknown' ? 10000 : CAPTURE_STATE_MS }; }

function captureStateFromPing(r) {
  if (r.status === 'ok') return r.capture === false ? 'no' : 'yes';    // older NDM: no "capture" field
  return r.error === 'host_missing' ? 'no' : 'unknown';
}

function refreshCaptureState() {
  if (!capPing) {
    capPing = pingRequest({ action: 'ping' }, 4000).then((r) => {
      capPing = null;
      setCaptureState(captureStateFromPing(r));
      return r;
    });
  }
  return capPing;
}

/** Cached state, re-pinged when stale (so about once a minute while downloads happen). */
function captureState() {
  if (!capPing && Date.now() - capState.t < capState.ttl) return Promise.resolve(capState.v);
  return refreshCaptureState().then(() => capState.v);
}

/** Host trouble seen on any port: missing = definitive "no", else ask again next time. */
function noteHostError(error) {
  if (error === 'host_missing') setCaptureState('no');
  else capState.t = 0;
}

async function hostStatus() {
  const r = await refreshCaptureState();
  if (r.status === 'ok') return { host: true, running: !!r.running, version: r.version || '', capture: r.capture !== false };
  return { host: false, running: false, error: r.error, detail: r.detail || lastHostError };
}

// ─── Cookies ────────────────────────────────────────────────────────────────

async function cookieStoreFor(item) {
  if (item && item.cookieStoreId) return item.cookieStoreId;            // Firefox containers
  if (item && item.incognito) return IS_FIREFOX ? 'firefox-private' : '1';
  return undefined;
}

function tabStore(tab) {
  return tab && tab.cookieStoreId ? tab.cookieStoreId
       : (tab && tab.incognito ? (IS_FIREFOX ? 'firefox-private' : '1') : undefined);
}

/**
 * SameSite filter for cookies replayed by NDM. `initiator` is the page that
 * started the download (referrer / tab). When it is a different site than
 * `url` (schemeful), SameSite=Strict cookies are not sent (Lax ones are,
 * as on a browser top-level navigation). No initiator (typed / pasted URL,
 * the user's own action) = same-site.
 */
function sameSiteAllowed(c, url, initiator) {
  if (!initiator || !isHttpUrl(initiator)) return true;
  if (sameSiteSchemeful(url, initiator)) return true;
  const ss = String(c.sameSite || '').toLowerCase();
  // A download is a top-level GET navigation: browsers send Lax (and Chrome's
  // default "unspecified") cookies on it, but never Strict ones.
  if (ss === 'strict') return false;
  return true;
}

async function rawCookies(url, storeId) {
  const q = { url };
  if (storeId) q.storeId = storeId;
  try { return await api.cookies.getAll(q) || []; }
  catch (e) {
    // Firefox with first-party isolation requires firstPartyDomain
    try { return await api.cookies.getAll(Object.assign(q, { firstPartyDomain: null })) || []; }
    catch (e2) { return null; }
  }
}

async function cookiesFor(url, storeId, initiator) {
  if (!api.cookies || !isNetUrl(url)) return '';
  const list = await rawCookies(url, storeId);
  if (!list) return '';
  return list.filter(c => sameSiteAllowed(c, url, initiator)).map(c => c.name + '=' + c.value).join('; ');
}

/** Exact cookies the browser would send to url, structured (for yt-dlp's cookies.txt). */
async function cookieListFor(url, storeId, initiator) {
  if (!api.cookies || !isHttpUrl(url)) return [];
  const list = await rawCookies(url, storeId);
  if (!list) return [];
  return list.filter(c => sameSiteAllowed(c, url, initiator)).slice(0, 300).map(c => ({
    name: String(c.name || ''), value: String(c.value || ''), domain: String(c.domain || ''),
    hostOnly: !!c.hostOnly, path: String(c.path || '/'), secure: !!c.secure, httpOnly: !!c.httpOnly,
    expirationDate: typeof c.expirationDate === 'number' ? c.expirationDate : 0,
  }));
}

function storeKey(storeId) { return storeId || 'default'; }

// ─── Hand-off to NDM ────────────────────────────────────────────────────────

async function buildRequest(o) {
  const r = {
    action:    'addDownload',
    url:       o.url,
    filename:  o.filename || '',
    referrer:  o.referrer || '',
    cookies:   o.cookies != null ? o.cookies : await cookiesFor(o.pageUrl || o.url, o.storeId, o.referrer),
    userAgent: navigator.userAgent,
    mime:      o.mime || '',
    fileSize:  o.fileSize > 0 ? o.fileSize : 0,
    silent:    !!o.silent,
  };
  if (o.source === 'capture') r.source = 'capture';
  if (o.kind === 'media') {
    r.kind = 'media';
    r.pageUrl = o.pageUrl || o.url;
    r.mediaFormat = o.mediaFormat || '';
    r.qualityLabel = o.qualityLabel || '';
    r.cookieList = o.cookies === '' ? [] : await cookieListFor(r.pageUrl, o.storeId, o.referrer);
  }
  return r;
}

/** Sends one download to NDM. Returns the host reply ({status:'accepted'} on success). */
async function sendToNdm(o) {
  const req = await buildRequest(o);
  const r = await nativeRequest(req, 60000);   // allow a cold NDM start (up to 45 s)
  const ok = r.status === 'accepted';
  if (r.error === 'disabled') setCaptureState('no');
  else if (!ok) noteHostError(r.error);
  const name = req.filename || nameFromUrl(req.url) || req.url;
  await addHistory({ url: req.url, filename: name, source: o.source || '',
                     status: ok ? 'sent' : (o.fellBack ? 'browser' : 'failed'), error: ok ? '' : (r.error || '') });
  return r;
}

function describeError(r) {
  switch (r && r.error) {
    case 'host_missing':    return 'NDM browser integration is not installed. Reinstall NDM.';
    case 'ndm_unreachable': return 'Could not start or reach Nee Download Manager.';
    case 'timeout':         return 'Nee Download Manager did not answer in time.';
    case 'bad_url':         return 'This address cannot be downloaded by NDM.';
    case 'disabled':        return 'Browser capture is turned off in NDM Options.';
    default:                return 'Nee Download Manager is not reachable.';
  }
}

// "Not installed" is said once per browser session (storage.session where
// available, so a restarted service worker does not repeat it).
const HOST_MISSING_KEY = 'ndmHostMissingShown';
let hostMissingShown = false;
const sessionStore = api.storage && api.storage.session;
try {
  const p = sessionStore && sessionStore.get(HOST_MISSING_KEY);
  if (p && typeof p.then === 'function')
    p.then(st => { if (st && st[HOST_MISSING_KEY]) hostMissingShown = true; }, () => {});
} catch (e) { /* ignore */ }

function notifyFailure(title, r, extra) {
  if (r && r.error === 'host_missing') {
    if (hostMissingShown) return;
    hostMissingShown = true;
    try {
      const p = sessionStore && sessionStore.set({ [HOST_MISSING_KEY]: true });
      if (p && typeof p.catch === 'function') p.catch(() => {});
    } catch (e) { /* ignore */ }
  }
  notify(title, describeError(r) + (extra || ''));
}

// ─── Capture decision ───────────────────────────────────────────────────────

// URLs the user Alt+clicked (content script) — let the browser handle them.
const bypass = new Map();

function consumeBypass(url) {
  const now = Date.now();
  for (const [u, t] of bypass) if (now - t > 8000) bypass.delete(u);
  if (bypass.has(url)) { bypass.delete(url); return true; }
  return false;
}

// Pages / frames loaded with POST (form submissions) in the last 30 s. A
// download they produce cannot be replayed by NDM (it would be a GET without
// the form body: an error page, or a spent one-time link), so the browser
// keeps it.
const POST_MS = 30000;
const postedUrls = new Map();    // url without #fragment -> time

function urlKey(u) { return String(u || '').split('#')[0]; }

function notePost(d) {
  if (!d || d.method !== 'POST' || !isNetUrl(d.url)) return;
  const now = Date.now();
  for (const [u, t] of postedUrls) if (now - t > POST_MS) postedUrls.delete(u);
  postedUrls.delete(urlKey(d.url));
  postedUrls.set(urlKey(d.url), now);
  while (postedUrls.size > 200) postedUrls.delete(postedUrls.keys().next().value);
}

function fromPost(u) {
  if (!u) return false;
  const t = postedUrls.get(urlKey(u));
  return t != null && Date.now() - t <= POST_MS;
}

if (api.webRequest && api.webRequest.onBeforeRequest) {
  try {
    api.webRequest.onBeforeRequest.addListener(notePost,
      { urls: ['http://*/*', 'https://*/*'], types: ['main_frame', 'sub_frame'] });
  } catch (e) { /* webRequest unavailable: POST downloads are not recognised */ }
}

function shouldCapture(item, s) {
  if (!s.enabled) return false;
  if (item.byExtensionId && item.byExtensionId === api.runtime.id) return false;   // our own fallback
  if (item.state && item.state !== 'in_progress') return false;
  const url = item.url || '';
  const fin = item.finalUrl || url;
  if (!isNetUrl(url) || !isNetUrl(fin)) return false;                              // blob: data: file: ...
  if (fromPost(url) || fromPost(fin)) return false;     // the download IS the form result
  if (consumeBypass(url) || (fin !== url && consumeBypass(fin))) return false;
  if (siteExcluded(s, hostOf(url)) || siteExcluded(s, hostOf(fin)) || siteExcluded(s, hostOf(item.referrer)))
    return false;

  const exts = extSet(s);
  const name = baseName(item.filename) || nameFromUrl(fin) || nameFromUrl(url);
  let ext = extOf(name);
  if (!exts.has(ext)) ext = mimeExt(item.mime);
  if (!exts.has(ext)) return false;

  const size = sizeOf(item);
  if (s.minSizeKB > 0 && size > 0 && size < s.minSizeKB * 1024) return false;
  return true;
}

// Download ids already being handed to NDM: onDeterminingFilename and
// onCreated must never cancel / erase the same item twice.
const capturing = new Set();

async function safeDl(fn, ...args) {
  // downloads.* calls are promise-based in Chrome MV3 and Firefox: errors
  // reject (never an unchecked runtime.lastError) and are swallowed here.
  try { return await api.downloads[fn](...args); } catch (e) { return undefined; }
}

async function captureDownload(item) {
  if (capturing.has(item.id)) return;
  capturing.add(item.id);
  setTimeout(() => capturing.delete(item.id), 60000);

  // 1. Stop the browser download as early as possible.
  await safeDl('cancel', item.id);
  let cur = item;
  const found = await safeDl('search', { id: item.id });
  if (found && found[0]) cur = found[0];
  if (cur.state === 'complete') return;               // too late: the browser already has the file
  await safeDl('erase', { id: item.id });

  const filename = baseName(cur.filename || item.filename) || nameFromUrl(item.finalUrl || item.url);
  const storeId = await cookieStoreFor(item);
  const r = await sendToNdm({
    url: item.url, filename, referrer: item.referrer || '', mime: item.mime || '',
    fileSize: sizeOf(item), storeId, silent: false, source: 'capture',
  });

  if (r.status === 'accepted') return;

  // 2. Private window: never restarted (Chrome would put it in the normal
  //    profile). Only reached when the ping said yes moments ago.
  if (item.incognito) {
    notifyFailure('Download not sent to NDM', r,
                  ' The private-window download was stopped; download it again (Alt+click keeps it in the browser).');
    return;
  }

  // 3. NDM unreachable — restart the download in the browser and tell the user.
  try {
    const opts = { url: item.url, conflictAction: 'uniquify' };
    if (IS_FIREFOX && item.cookieStoreId) opts.cookieStoreId = item.cookieStoreId;
    await api.downloads.download(opts);
    if (r.error !== 'disabled')
      notifyFailure('Downloading in browser', r, ' The download continues in the browser.');
  } catch (e) {
    notify('Download failed', describeError(r) + ' Could not restart it in the browser: ' + (e && e.message || e));
  }
}

/** Intercept at all? Never when NDM cannot take it; private windows only on a definite yes. */
function captureAllowed(item, state) {
  if (state === 'no') return false;
  return !item.incognito || state === 'yes';
}

if (api.downloads.onDeterminingFilename) {
  // Chrome / Edge: fires with the final file name (Content-Disposition) and
  // before any Save As dialog. Every path either calls suggest() exactly
  // once or cancels the download. suggest() is NEVER called after cancel():
  // Chrome then reports "Download must be in progress" as an unchecked
  // runtime.lastError (the suggest callback has no way to consume it).
  api.downloads.onDeterminingFilename.addListener((item, suggest) => {
    let done = false;
    const suggestOnce = () => {
      if (done) return;
      done = true;
      try { suggest(); } catch (e) { /* ignore */ }
    };
    const decide = (s) => {
      if (capturing.has(item.id)) { done = true; return; }   // already cancelled by us
      let capture = false;
      try { capture = shouldCapture(item, s); } catch (e) { capture = false; }
      if (!capture) { suggestOnce(); return; }
      // The download waits for suggest() meanwhile: nothing is lost if NDM says no.
      captureState().then((state) => {
        if (done) return;
        if (capturing.has(item.id)) { done = true; return; }
        if (!captureAllowed(item, state)) { suggestOnce(); return; }
        done = true;                     // cancelled below: no suggest() any more
        captureDownload(item).catch(() => {});
      }, suggestOnce);
    };
    if (settingsCache) decide(settingsCache);
    else loadSettings().then(decide, suggestOnce);
    return true;      // suggest() may be called asynchronously
  });
} else {
  // Firefox: onCreated (file name already chosen).
  api.downloads.onCreated.addListener(async (item) => {
    const s = settingsCache || await loadSettings();
    if (!shouldCapture(item, s) || capturing.has(item.id)) return;
    const state = await captureState();
    if (captureAllowed(item, state) && !capturing.has(item.id)) captureDownload(item).catch(() => {});
  });
}

// ─── Media seen by the browser (webRequest observation) ─────────────────────
// Per tab: whole media files the page played and HLS/DASH manifests. Sites
// handled by yt-dlp from the page address are ignored (their requests are
// segments). Never offered: segments (.m4s, .ts, init segments, byte-range
// URLs), MSE fragments fetched by script (XHR/fetch video), and — as "video" —
// anything that is audio only (audio MIME, audio file types, audio renditions).

const SKIP_HOSTS = /(^|\.)(youtube\.com|youtube-nocookie\.com|googlevideo\.com|ytimg\.com|facebook\.com|fbcdn\.net|instagram\.com|cdninstagram\.com|doubleclick\.net|googlesyndication\.com|google-analytics\.com)$/i;
const MEDIA_EXT = /\.(mp4|m4v|webm|mov|mkv|flv|3gp|ogv|mp3|m4a|aac|ogg|oga|opus|flac|wav|weba|m3u8|mpd)$/i;
const AUDIO_EXT = /\.(mp3|m4a|aac|ogg|oga|opus|flac|wav|weba)$/i;
const SEGMENT_EXT = /\.(ts|m4s|m4f|m4v?s|cmfv|cmfa|vtt|webvtt|key|init|frag)(\?|#|$)|[?&](range|bytestart|byteend|byterange|bytes)=|\/range\/\d+-\d+|\/bytes=\d/i;
// init / fragment / chunk names: init.mp4, seg-12.mp4, chunk_3.m4a, /segments/...
const SEGMENT_NAME = /(^|[\/_.-])(init|seg|segment|chunk|frag|fragment)[-_.]?\d*(\.[a-z0-9]{2,5})?$|\/(seg|segments?|chunks?|frags?|fragments?)\//i;
// Audio renditions / audio-only representations in manifests and file URLs.
const AUDIO_HINT = /(^|[\/_.=-])(audio|aud|mp4a|aac|sound|soundtrack)(?=[\/_.=-]|\d|$)|DASH_AUDIO/i;
// HLS/DASH variant playlists (one resolution, usually video only).
const VARIANT_HINT = /\/(avc1|avc3|hvc1|hev1|vp09|vp9|av01)\/|(^|[\/_.-])\d{3,4}x\d{3,4}([\/_.-]|$)|(^|[\/_-])\d{3,4}p([\/_.-]|$)|chunklist|[\/_-](video|vid)[_-]?\d{3,4}|\/v\d+\/(prog_)?index\.m3u8$/i;

// ── Per-tab media, kept across service-worker restarts ──
// MV3 unloads the worker ~30 s after the last event: what the tab was seen
// loading (and so which addresses its overlay may use) is mirrored to
// storage.session (browser memory, gone with the session, not readable by
// content scripts) and restored lazily per tab. Without storage.session
// (older Firefox: a longer-lived event page) it stays in memory only.
// Cleared on a real navigation of the tab (new document or reload), never on
// a same-document change (#fragment, history.pushState).
const TAB_KEY = 'ndmTab:';
const TAB_MAX_ITEMS = 80;
const TAB_MAX_AGE_MS = 6 * 3600000;
const TAB_MAX_URL = 4096;           // longer addresses are kept in memory only
const tabMedia = new Map();         // tabId -> [{url, type, kind, mime, size, t, frameId}]
const tabPage = new Map();          // tabId -> top-level address last seen (with #fragment)
const tabLoaded = new Map();        // tabId -> promise: storage.session merged in (this worker)
const tabGen = new Map();           // tabId -> clear counter (a late restore never revives cleared data)
const tabSaveTimers = new Map();
const SESSION_OK = !!(sessionStore && typeof sessionStore.get === 'function' && typeof sessionStore.set === 'function');
const RESOLVED = Promise.resolve();

function sessionCall(fn) {
  try { const p = fn(); if (p && typeof p.catch === 'function') p.catch(() => {}); } catch (e) { /* ignore */ }
}

function restoreTab(tabId) {
  if (typeof tabId !== 'number' || tabId < 0) return RESOLVED;
  let p = tabLoaded.get(tabId);
  if (p) return p;
  if (!SESSION_OK) { tabLoaded.set(tabId, RESOLVED); return RESOLVED; }
  const gen = tabGen.get(tabId) || 0;
  p = (async () => {
    let st = null;
    try { st = await sessionStore.get(TAB_KEY + tabId); } catch (e) { return; }
    const rec = st && st[TAB_KEY + tabId];
    if (!rec || typeof rec !== 'object' || (tabGen.get(tabId) || 0) !== gen) return;
    if (!tabPage.has(tabId) && typeof rec.page === 'string') tabPage.set(tabId, rec.page);
    const now = Date.now();
    const saved = (Array.isArray(rec.items) ? rec.items : [])
      .filter(m => m && typeof m === 'object' && isNetUrl(m.url) && typeof m.t === 'number' && now - m.t < TAB_MAX_AGE_MS);
    const cur = tabMedia.get(tabId) || [];          // noted since this worker started: newer
    const merged = saved.filter(m => !cur.some(c => c.url === m.url)).concat(cur).slice(-TAB_MAX_ITEMS);
    if (merged.length) tabMedia.set(tabId, merged);
  })();
  tabLoaded.set(tabId, p);
  return p;
}

function saveTab(tabId) {
  if (!SESSION_OK || tabSaveTimers.has(tabId)) return;
  tabSaveTimers.set(tabId, setTimeout(() => {
    tabSaveTimers.delete(tabId);
    const items = (tabMedia.get(tabId) || []).filter(m => m.url.length <= TAB_MAX_URL);
    const page = tabPage.get(tabId) || '';
    if (!items.length && !page) { sessionCall(() => sessionStore.remove(TAB_KEY + tabId)); return; }
    sessionCall(() => sessionStore.set({ [TAB_KEY + tabId]: { page, items } }));
  }, 200));
}

/** The tab shows a new document (or the same one reloaded): its media list starts empty. */
function clearTab(tabId, page) {
  if (typeof tabId !== 'number' || tabId < 0) return;
  tabGen.set(tabId, (tabGen.get(tabId) || 0) + 1);
  tabLoaded.set(tabId, RESOLVED);
  tabMedia.delete(tabId);
  if (isNetUrl(page)) tabPage.set(tabId, page); else tabPage.delete(tabId);
  saveTab(tabId);
}

function forgetTab(tabId) {
  clearTab(tabId, '');
  tabPage.delete(tabId);
  clearTimeout(tabSaveTimers.get(tabId));
  tabSaveTimers.delete(tabId);
  if (SESSION_OK) sessionCall(() => sessionStore.remove(TAB_KEY + tabId));
}

/** 'hls' | 'dash' | 'file' | '' (not media / a segment). */
function classifyMedia(url, mime) {
  mime = String(mime || '').split(';')[0].trim().toLowerCase();
  const path = String(url || '').split(/[?#]/)[0];
  if (/mpegurl/.test(mime) || /\.m3u8$/i.test(path)) return 'hls';
  if (mime === 'application/dash+xml' || /\.mpd$/i.test(path)) return 'dash';
  if (SEGMENT_EXT.test(url) || mime === 'video/mp2t' || mime === 'video/iso.segment' || mime === 'audio/mp2t') return '';
  if (SEGMENT_NAME.test(path)) return '';
  if (/^(video|audio)\//.test(mime) || MEDIA_EXT.test(path)) return 'file';
  return '';
}

/** 'audio' when the address / type says audio only, else 'video'. */
function mediaKind(url, mime) {
  mime = String(mime || '').split(';')[0].trim().toLowerCase();
  const path = String(url || '').split(/[?#]/)[0];
  if (/^audio\//.test(mime) || AUDIO_EXT.test(path)) return 'audio';
  if (/^video\//.test(mime) && !AUDIO_HINT.test(path)) return 'video';
  return AUDIO_HINT.test(path) ? 'audio' : 'video';
}

/** Manifest preference: 2 master-like, 1 unknown, 0 video variant, -1 audio rendition. */
function manifestRank(url) {
  const path = String(url || '').split(/[?#]/)[0];
  if (AUDIO_HINT.test(path)) return -1;
  if (VARIANT_HINT.test(path)) return 0;
  return /master|manifest|playlist|index|\/pl\/[^/]+\.m3u8$/i.test(path) ? 2 : 1;
}

function headerOf(headers, name) {
  const h = (headers || []).find(x => x.name && x.name.toLowerCase() === name);
  return h ? (h.value || '') : '';
}

function noteMedia(d) {
  if (d.tabId == null || d.tabId < 0 || !isNetUrl(d.url) || d.statusCode >= 400) return;
  if (SKIP_HOSTS.test(hostOf(d.url))) return;
  const mime = headerOf(d.responseHeaders, 'content-type');
  const type = classifyMedia(d.url, mime);
  if (!type) return;
  const kind = type === 'file' ? mediaKind(d.url, mime) : (manifestRank(d.url) < 0 ? 'audio' : 'video');
  const crange = headerOf(d.responseHeaders, 'content-range');
  let size = 0;
  const range = /\/(\d+)\s*$/.exec(crange);
  if (range) size = +range[1];
  else size = +headerOf(d.responseHeaders, 'content-length') || 0;
  if (type === 'file') {
    // Script-fetched media (XHR / fetch) is a Media Source fragment or a
    // separate DASH audio / video representation, never a playable file —
    // except a whole audio file (Web Audio players load complete MP3s).
    if (d.type === 'xmlhttprequest' &&
        (kind !== 'audio' || crange || d.statusCode === 206 || /\.(mp4|m4a)$/i.test(d.url.split('?')[0]))) return;
    if (size > 0 && size < (kind === 'audio' ? 100 : 300) * 1024) return;   // thumbnails, UI sounds, init segments
  }
  const entry = { url: d.url, type, kind, mime: mime.split(';')[0], size, t: Date.now(), frameId: d.frameId || 0 };
  const tabId = d.tabId;
  if (tabLoaded.has(tabId)) addMedia(tabId, entry);
  else restoreTab(tabId).then(() => addMedia(tabId, entry));
}

function addMedia(tabId, entry) {
  const list = tabMedia.get(tabId) || [];
  const old = list.find(m => m.url === entry.url);
  if (old) {
    old.t = entry.t;
    if (entry.size > old.size) old.size = entry.size;
    saveTab(tabId);
    return;
  }
  // HLS variant playlists: keep only the first (master) manifest per directory.
  if (entry.type === 'hls') {
    const dir = entry.url.split('?')[0].replace(/[^/]*$/, '');
    if (list.some(m => m.type === 'hls' && m.url.split('?')[0].replace(/[^/]*$/, '') === dir)) return;
  }
  list.push(entry);
  // Busy feeds load many players: drop the oldest non-master first, so the
  // master playlists (video + audio) stay findable.
  while (list.length > TAB_MAX_ITEMS) {
    const i = list.findIndex(m => m.type === 'file' || manifestRank(m.url) < 2);
    list.splice(i >= 0 ? i : 0, 1);
  }
  tabMedia.set(tabId, list);
  saveTab(tabId);
}

// Which responses wake the worker: every <video>/<audio> load ('media'), and
// script / plugin fetches only when the address names a manifest or a media
// file (X fetches its .m3u8 playlists with fetch(); YouTube needs nothing
// sniffed, yt-dlp reads the page). Ordinary XHR / fetch traffic (APIs, JSON,
// images, analytics) never reaches noteMedia. Known gap: a manifest served
// from an address without .m3u8 / .mpd is seen only when a media element loads it.
const SNIFF_EXTS = ['m3u8', 'mpd', 'mp4', 'm4v', 'webm', 'mov', 'mkv', 'flv', '3gp', 'ogv',
                    'mp3', 'm4a', 'aac', 'ogg', 'oga', 'opus', 'flac', 'wav', 'weba'];
const SNIFF_URLS = SNIFF_EXTS.reduce((a, e) => a.concat(['http://*/*.' + e + '*', 'https://*/*.' + e + '*']), []);
const SNIFF_FILTERS = [
  { urls: ['http://*/*', 'https://*/*'], types: ['media'] },
  { urls: SNIFF_URLS, types: ['xmlhttprequest', 'other', 'object'] },
];
if (api.webRequest && api.webRequest.onResponseStarted) {
  for (const f of SNIFF_FILTERS) {
    try { api.webRequest.onResponseStarted.addListener(noteMedia, f, ['responseHeaders']); }
    catch (e) { /* webRequest unavailable: overlay still works through yt-dlp */ }
  }
}

// Real navigations of the top frame. webNavigation (when the browser grants
// it) tells new documents and reloads (onCommitted) from same-document
// changes; otherwise tabs.onUpdated: status "loading" for another address or
// the same address again (reload) clears, a changed #fragment alone does not.
const WEBNAV = !!(api.webNavigation && api.webNavigation.onCommitted && typeof api.webNavigation.onCommitted.addListener === 'function');
if (WEBNAV) {
  try {
    api.webNavigation.onCommitted.addListener((d) => { if (d && d.frameId === 0) clearTab(d.tabId, d.url); });
  } catch (e) { /* ignore */ }
}
if (api.tabs && api.tabs.onUpdated) {
  api.tabs.onUpdated.addListener((tabId, info, tab) => {
    if (WEBNAV || !info) return;
    const url = info.url || (tab && tab.url) || '';
    if (info.status !== 'loading' && !info.url) return;
    restoreTab(tabId).then(() => {
      const prev = tabPage.get(tabId) || '';
      if (info.status === 'loading') {
        const fragmentOnly = !!info.url && !!prev && info.url !== prev && urlKey(info.url) === urlKey(prev);
        if (!fragmentOnly) { clearTab(tabId, url); return; }
      }
      if (isNetUrl(url) && url !== prev) { tabPage.set(tabId, url); saveTab(tabId); }
    });
  });
}
if (api.tabs && api.tabs.onRemoved) api.tabs.onRemoved.addListener((tabId) => forgetTab(tabId));

// ─── Quality list for the overlay ───────────────────────────────────────────

// Key: cookie store (normal / incognito / container) + page, so a private
// window never reuses a normal window's result and vice versa.
const probeCache = new Map();    // storeKey|url -> {t, promise, tabId}
const ANON_RETRY_HOSTS = /(^|\.)(x\.com|twitter\.com)$/i;

/** prio 'click' (the user waits for it) or 'prefetch' (hover; may be dropped). */
function probe(pageUrl, tab, referrer, prio) {
  if (!isHttpUrl(pageUrl)) return Promise.resolve({ status: 'error', error: 'bad_url' });
  prio = prio === 'prefetch' ? 'prefetch' : 'click';
  const now = Date.now();
  for (const [k, v] of probeCache) if (now - v.t > MEDIA_CACHE_MS) probeCache.delete(k);
  const store = tabStore(tab);
  const key = storeKey(store) + '|' + pageUrl;
  const hit = probeCache.get(key);
  if (hit && !hit.ctl.dropped) {
    if (prio === 'click' && hit.ctl.prio !== 'click') { hit.ctl.prio = 'click'; pumpMedia(); }   // jump the queue
    return hit.promise;
  }
  const ctl = { prio, dropped: false };
  if (prio === 'prefetch') { latestPrefetch = ctl; dropQueuedPrefetches(ctl); }
  // Initiator for SameSite: the top-level page of the tab (else the referrer).
  const initiator = (tab && isHttpUrl(tab.url)) ? tab.url : (isHttpUrl(referrer) ? referrer : '');
  const entry = { t: now, promise: null, ctl };
  entry.promise = (async () => {
    const req = (cookies, cookieList) => mediaRequest({
      action: 'getMedia', pageUrl, referrer: isHttpUrl(referrer) ? referrer : '',
      cookies, cookieList, cookieStore: storeKey(store), userAgent: navigator.userAgent,
    }, MEDIA_TIMEOUT_MS, ctl);
    const cookies = await cookiesFor(pageUrl, store, initiator);
    let r = await req(cookies, await cookieListFor(pageUrl, store, initiator));
    // X: yt-dlp's signed-in API path breaks now and then while the public
    // one still works; such a post then downloads without cookies.
    if (r.status !== 'ok' && cookies && ANON_RETRY_HOSTS.test(hostOf(pageUrl)) && !r.timedOut &&
        !/unreachable|timeout|rate_limited|bad_url|dropped|host_/.test(String(r.error || ''))) {
      const r2 = await req('', []);
      if (r2.status === 'ok') r = Object.assign({}, r2, { anon: true });
    }
    if (r.status !== 'ok' && probeCache.get(key) === entry) probeCache.delete(key);   // retry next time
    if (r.status === 'ok') saveProbeMeta(key, r);
    return r;
  })();
  probeCache.set(key, entry);
  return entry.promise;
}

// What a pick needs from its probe (file name, "download without cookies"),
// kept in storage.session so a pick after a service-worker restart still has it.
const PROBE_META_KEY = 'ndmProbeMeta';
const PROBE_META_MAX = 40;
const PROBE_META_MS = 30 * 60000;

function saveProbeMeta(key, r) {
  if (!SESSION_OK) return;
  sessionCall(async () => {
    const st = await sessionStore.get(PROBE_META_KEY);
    const all = (st && st[PROBE_META_KEY] && typeof st[PROBE_META_KEY] === 'object') ? st[PROBE_META_KEY] : {};
    const now = Date.now();
    all[key] = { t: now, title: String(r.title || '').slice(0, 300), anon: !!r.anon };
    const keys = Object.keys(all).filter(k => all[k] && now - all[k].t < PROBE_META_MS)
      .sort((x, y) => all[y].t - all[x].t).slice(0, PROBE_META_MAX);
    const next = {};
    for (const k of keys) next[k] = all[k];
    await sessionStore.set({ [PROBE_META_KEY]: next });
  });
}

async function probeMeta(key) {
  if (!SESSION_OK) return null;
  try {
    const st = await sessionStore.get(PROBE_META_KEY);
    const m = st && st[PROBE_META_KEY] && st[PROBE_META_KEY][key];
    return m && typeof m.t === 'number' && Date.now() - m.t < PROBE_META_MS ? m : null;
  } catch (e) { return null; }
}

/** Timed out anywhere (queue, port, or yt-dlp inside NDM). */
function isTimeout(r) { return !!r && r.status !== 'ok' && (r.error === 'timeout' || r.timedOut === true); }

/** Finished probe for this tab's store (never from the page script), or null. */
async function probed(pageUrl, tab) {
  const key = storeKey(tabStore(tab)) + '|' + pageUrl;
  const hit = probeCache.get(key);
  if (!hit) {                                   // worker restarted since the probe?
    const m = await probeMeta(key);
    return m ? { status: 'ok', title: m.title, anon: m.anon } : null;
  }
  try { const r = await hit.promise; return r && r.status === 'ok' ? r : null; }
  catch (e) { return null; }
}
async function probedTitle(pageUrl, tab) {
  const r = await probed(pageUrl, tab);
  return r ? String(r.title || '') : '';
}

// ─── Trust checks for content-script messages ──────────────────────────────
// A content script runs inside the page's renderer: everything it sends is
// treated as page-controlled. Page addresses must belong to the tab's site
// (or the frame's own site, e.g. an embedded player), or be media the tab
// was seen loading (webRequest).

function sniffedFor(tab, url) {
  return !!(tab && (tabMedia.get(tab.id) || []).some(m => m.url === url));
}

function pageAllowed(url, sender) {
  if (!isHttpUrl(url) || !sender || !sender.tab) return false;
  if (sameSite(url, sender.tab.url)) return true;
  if (sender.url && isHttpUrl(sender.url) && sameSite(url, sender.url)) return true;
  return sniffedFor(sender.tab, url);
}

const magnetTimes = [];          // rate limit for page-triggered torrent dialogs
function magnetAllowed() {
  const now = Date.now();
  while (magnetTimes.length && now - magnetTimes[0] > 60000) magnetTimes.shift();
  if (magnetTimes.length && now - magnetTimes[magnetTimes.length - 1] < 2000) return false;
  if (magnetTimes.length >= 5) return false;
  magnetTimes.push(now);
  return true;
}

function fmtExt(m) {
  return (extOf(nameFromUrl(m.url)) || mimeExt(m.mime) || (m.kind === 'audio' ? 'm4a' : 'mp4')).toUpperCase();
}

/**
 * Instant part of the list: the clicked element's own file, plus whole files
 * the tab loaded that belong to that element, of the wanted kind ('video' |
 * 'audio'). A file belongs when it is the element's src / currentSrc, carries
 * the element's X media id in its address, or came from the element's frame
 * while nothing else could have loaded it: the frame has a single <video>
 * (opts.single), or — audio — a custom player without an address of its own.
 * Never another post's media from a busy feed (X GIF posts, other players).
 * Labels say what the file is: "MP4" for a video file, "Audio MP3" for an audio one.
 * opts: {frameId, mediaId, single}
 */
function sniffedItems(tabId, src, want, opts) {
  want = want === 'audio' ? 'audio' : 'video';
  opts = opts || {};
  const frameId = typeof opts.frameId === 'number' ? opts.frameId : 0;
  const mediaId = /^\d{5,25}$/.test(String(opts.mediaId || '')) ? String(opts.mediaId) : '';
  const belongs = (m) => (mediaId && hasMediaId(m.url, mediaId)) ||
    ((m.frameId || 0) === frameId && (want === 'audio' ? !src : opts.single === true));
  const out = [];
  const seen = new Set();
  const list = (tabMedia.get(tabId) || []).slice().reverse();
  const item = (url, mime, size) => {
    const kind = mediaKind(url, mime);
    const ext = fmtExt({ url, mime, kind });
    return { label: (kind === 'audio' ? 'Audio ' : '') + ext, url, ext: ext.toLowerCase(), filesize: size || 0, kind };
  };
  if (isNetUrl(src) && classifyMedia(src, '') === 'file') {
    const known = list.find(m => m.url === src);
    const it = item(src, known && known.mime, known && known.size);
    // The element's own address: a <video> playing an .mp3 is audio, an <audio> playing .mp4 too.
    if (want === 'audio') { it.kind = 'audio'; if (!/^Audio /.test(it.label)) it.label = 'Audio ' + it.label; }
    if (it.kind === want) { out.push(it); seen.add(src); }
  }
  for (const m of list) {
    if (m.type !== 'file' || seen.has(m.url) || m.kind !== want || !belongs(m)) continue;
    seen.add(m.url);
    out.push(item(m.url, m.mime, m.size));
    if (out.length >= 4) break;
  }
  return out;
}

/** Sniffed manifests worth probing for this kind, best first (masters, oldest first).
 *  mediaId (X): only that video's playlists. */
function manifestsFor(tabId, want, mediaId) {
  const list = (tabMedia.get(tabId) || []).filter(m => m.type !== 'file' && (!mediaId || hasMediaId(m.url, mediaId)));
  const ranked = list.map((m, i) => ({ m, i, r: manifestRank(m.url) }))
    .filter(x => want === 'audio' ? true : x.r >= 0)
    .sort((a, b) => (want === 'audio' ? (a.r < 0) === (b.r < 0) ? 0 : (a.r < 0 ? -1 : 1) : 0) || b.r - a.r || a.i - b.i);
  return ranked.map(x => x.m);
}

function hasMediaId(url, id) {
  return String(url || '').split(/[?#]/)[0].indexOf('/' + id + '/') >= 0;
}

/** Height from a variant playlist address ("/1280x720/" or "/720p/"), else 0. */
function variantHeight(url) {
  const path = String(url || '').split(/[?#]/)[0];
  const m = /[\/_.-]\d{3,4}x(\d{3,4})(?=[\/_.-]|$)/.exec(path) || /[\/_-](\d{3,4})p(?=[\/_.-]|$)/.exec(path);
  return m ? +m[1] : 0;
}

// A video-only variant plus its audio rendition (master not seen): NDM joins
// them ("pair|<audio>|<selector>" -> a local master playlist for yt-dlp).
const PAIR_MAX_URL = 1500;
/**
 * MP3 bitrates (kbps) worth offering for a source of `kbps`: the highest
 * standard rate not above the source (2 kbps tolerance), plus 128 when the
 * source reaches it, plus 320 for sources of 256 kbps and more — e.g.
 * 160 -> [160, 128], 128 -> [128], 256 -> [320, 256, 128], 106 -> [96].
 * Same rule as Mp3Rates() in ndm_msghost.cpp. [] when the rate is unknown.
 */
function mp3Rates(kbps) {
  if (!(kbps > 0)) return [];
  const out = kbps >= 256 ? [320] : [];
  const top = [256, 192, 160, 128, 96, 64, 48, 32].find(b => b <= kbps + 2);
  if (top) out.push(top);
  if (kbps + 2 >= 128 && !out.includes(128)) out.push(128);
  return out;
}

// Every resolution the player loaded, highest first, each joined with the
// best audio; then the audio renditions themselves and MP3 conversions.
// "pair|<audio>|v=<variant>|<selector>". X's renditions are AAC ("/mp4a/"
// in the address, the bitrate next to it): "x:m4a" copies AAC into .m4a
// without re-encoding; MP3 is an encode at the stated bitrate.
function pairFormats(videos, audios) {
  const audio = audios[0];
  const a = 'pair|' + audio.url + '|';
  const vs = videos.map(v => ({ v, h: variantHeight(v.url) })).sort((x, y) => y.h - x.h);
  const out = vs.map((x, i) => ({
    label: (i === 0 ? 'Best (' : '') + (x.h ? x.h + 'p MP4' : 'MP4') + (i === 0 ? ')' : ''),
    kind: 'video', format: a + 'v=' + x.v.url + '|bv*+ba/b', ext: 'mp4', height: x.h,
  }));
  const v0 = 'v=' + vs[0].v.url + '|';
  const aac = (m) => /\/mp4a\//i.test(String(m.url).split(/[?#]/)[0]);
  const kb = (m) => Math.round(audioRate(m.url) / 1000);
  const desc = (m) => (aac(m) ? 'M4A' : 'audio') + (kb(m) ? ' ' + kb(m) + ' kbps' : '');
  out.push({ label: 'Best Audio (' + desc(audio) + ')', kind: 'audio', format: a + v0 + 'x:m4a|ba/b', ext: 'm4a' });
  const rates = new Set([kb(audio)]);
  for (const m of audios.slice(1)) {
    if (rates.has(kb(m)) || !kb(m) || rates.size >= 4) continue;
    rates.add(kb(m));
    out.push({ label: 'Audio ' + desc(m), kind: 'audio', format: 'pair|' + m.url + '|' + v0 + 'x:m4a|ba/b', ext: 'm4a' });
  }
  const mp3 = mp3Rates(kb(audio));
  for (const b of mp3)
    out.push({ label: 'MP3 ' + b + ' kbps (converted)', kind: 'audio', format: a + v0 + 'x:mp3:' + b + '|ba/b', ext: 'mp3' });
  if (!mp3.length) out.push({ label: 'MP3 (converted)', kind: 'audio', format: a + v0 + 'x:mp3|ba/b', ext: 'mp3' });
  return out;
}

/** Audio rendition bitrate from "/mp4a/128000/", else 0. */
function audioRate(url) {
  const m = /\/mp4a\/(\d{4,7})\//.exec(String(url || '').split(/[?#]/)[0]);
  return m ? +m[1] : 0;
}

// A lone audio rendition of unknown codec: "x:m4a" copies AAC and encodes
// anything else, so no "Best" / bitrate claim; MP3 is always an encode.
const AUDIO_FORMATS = [
  { label: 'Audio M4A', kind: 'audio', format: 'x:m4a|ba/b', ext: 'm4a' },
  { label: 'MP3 (converted)', kind: 'audio', format: 'x:mp3|ba/b', ext: 'mp3' },
];

async function mediaList(msg, sender) {
  const tab = sender.tab;
  const want = msg.kind === 'audio' ? 'audio' : 'video';
  const pageUrl = isHttpUrl(msg.pageUrl) ? msg.pageUrl : (tab && tab.url) || '';
  const referrer = isHttpUrl(msg.referrer) && pageAllowed(msg.referrer, sender) ? msg.referrer : '';
  if (!pageAllowed(pageUrl, sender)) return { status: 'error', error: 'bad_url' };
  const mediaId = /^\d{5,25}$/.test(String(msg.mediaId || '')) ? String(msg.mediaId) : '';
  let r = await probe(pageUrl, tab, referrer);
  let probeUrl = pageUrl, manifest = null, timedOut = isTimeout(r);
  if (r.status !== 'ok' && tab) {
    const mans = manifestsFor(tab.id, want, mediaId);
    // Only a video variant and the audio rendition of this video were seen
    // (no master): offer them joined, never the silent variant alone.
    const best = mans[0];
    if (mediaId && best && manifestRank(best.url) === 0) {
      const audios = manifestsFor(tab.id, 'audio', mediaId).filter(m => manifestRank(m.url) < 0 && m.url.length < PAIR_MAX_URL)
        .sort((x, y) => audioRate(y.url) - audioRate(x.url));
      const videos = mans.filter(m => manifestRank(m.url) === 0 && m.url.length < PAIR_MAX_URL);
      if (audios.length && videos.length) {
        const formats = pairFormats(videos.slice(0, 6), audios).filter(f => want === 'video' || f.kind === 'audio');
        return { status: 'ok', title: '', probeUrl: best.url, formats };
      }
    }
    // Page itself unknown to yt-dlp: try the manifests the tab loaded — a
    // master playlist (video + audio) first, never an audio rendition as video.
    for (const man of mans.slice(0, 2)) {
      const r2 = await probe(man.url, tab, pageUrl);
      if (r2.status === 'ok') { r = r2; probeUrl = man.url; manifest = man; break; }
      if (isTimeout(r2)) timedOut = true;
    }
  }
  // A timeout is not "nothing found": the overlay says so (and a retry probes again).
  if (r.status !== 'ok') return timedOut ? { status: 'error', error: 'timeout', detail: r.detail || '' } : r;
  let formats = (r.formats || []).filter(f => f && typeof f === 'object');
  if (manifest) {
    const rank = manifestRank(manifest.url);
    if (rank < 0) formats = AUDIO_FORMATS.slice();                 // audio rendition: audio, honestly
    else if (rank === 0)                                            // one video variant: may lack audio
      formats = formats.map(f => (f.kind === 'audio' || /^Audio /.test(f.label || '')) ? f
        : Object.assign({}, f, { label: String(f.label || '').replace(/^Best quality$/, 'Video stream') }));
  }
  formats = formats.filter(f => (f.kind || (/^Audio /.test(f.label || '') ? 'audio' : 'video')) === want || want === 'video');
  return { status: 'ok', title: r.title || '', probeUrl, formats: formats.slice(0, 16) };
}

const IMAGE_EXT = /^(jpe?g|png|gif|webp|avif|bmp|svg|heic|heif|jfif|tiff?|ico)$/i;
const IMAGE_MIME_EXT = { 'image/jpeg': 'jpg', 'image/jpg': 'jpg', 'image/pjpeg': 'jpg', 'image/png': 'png', 'image/gif': 'gif',
  'image/webp': 'webp', 'image/avif': 'avif', 'image/bmp': 'bmp', 'image/svg+xml': 'svg', 'image/heic': 'heic',
  'image/heif': 'heif', 'image/tiff': 'tif', 'image/x-icon': 'ico', 'image/vnd.microsoft.icon': 'ico' };

/**
 * Extension for a photo address: the URL's own image extension, else a
 * ?format= / ?fm= / ?ext= hint (pbs.twimg.com, imgix, CDNs), else the
 * Content-Type of a cookie-less HEAD request, else "jpg".
 */
async function imageExt(url) {
  const e = extOf(nameFromUrl(url));
  if (IMAGE_EXT.test(e)) return e.toLowerCase().replace('jpeg', 'jpg');
  try {
    const q = new URL(url).searchParams;
    for (const k of ['format', 'fm', 'ext', 'type', 'f']) {
      const v = String(q.get(k) || '').toLowerCase();
      if (IMAGE_EXT.test(v)) return v.replace('jpeg', 'jpg');
    }
  } catch (err) { /* ignore */ }
  if (typeof fetch === 'function' && typeof AbortController === 'function') {
    const ac = new AbortController();
    const timer = setTimeout(() => ac.abort(), 4000);
    try {
      const r = await fetch(url, { method: 'HEAD', credentials: 'omit', redirect: 'follow', signal: ac.signal, cache: 'force-cache' });
      const ct = String(r.headers.get('content-type') || '').split(';')[0].trim().toLowerCase();
      if (IMAGE_MIME_EXT[ct]) return IMAGE_MIME_EXT[ct];
    } catch (err) { /* offline / blocked: fall through */ }
    finally { clearTimeout(timer); }
  }
  return 'jpg';
}

/** The user picked an entry of the overlay list (content script). */
async function sendMediaItem(msg, sender) {
  const tab = sender.tab;
  const it = (msg.item && typeof msg.item === 'object') ? msg.item : {};
  const ext = /^[a-z0-9]{1,5}$/i.test(String(it.ext || '')) ? String(it.ext).toLowerCase() : 'mp4';
  const referrer = (tab && isHttpUrl(tab.url)) ? tab.url : '';
  const storeId = tabStore(tab);
  const size = typeof it.filesize === 'number' && it.filesize > 0 ? it.filesize : 0;
  const mime = /^(image|video|audio)\/[\w.+*-]{1,60}$/i.test(String(msg.mime || '')) ? String(msg.mime) : '';
  if (isNetUrl(it.url)) {
    // Direct file or photo: normal download. Name derived here, never taken
    // from the page. Cookies only for the tab's own site or the exact media
    // address the tab loaded.
    if (!isHttpUrl(it.url)) return { status: 'error', error: 'bad_url' };
    const own = sameSite(it.url, tab && tab.url) || sniffedFor(tab, it.url);
    const fromUrl = safeName(nameFromUrl(it.url));
    let filename;
    if (/^image\//i.test(mime)) {
      // Photos: a real image extension (URL, ?format=, else the server's Content-Type).
      const stem = (extOf(fromUrl) ? fromUrl.replace(/\.[^.]*$/, '') : nameFromUrl(it.url) ? fromUrl : '') ||
                   safeName(tab && tab.title);
      filename = safeName(stem) + '.' + await imageExt(it.url);
    } else {
      filename = extOf(fromUrl) ? fromUrl : safeName(tab && tab.title) + '.' + ext;
    }
    const r = await sendToNdm({ url: it.url, referrer, storeId, filename, source: 'video_button',
                                cookies: own ? undefined : '', fileSize: size, mime });
    if (r.status !== 'accepted') notifyFailure('Nee Download Manager', r);
    return r;
  }
  const pageUrl = isHttpUrl(msg.probeUrl) ? msg.probeUrl : '';
  if (!pageUrl || !pageAllowed(pageUrl, sender) || typeof it.format !== 'string' || !it.format ||
      it.format.length > 3200 || /[\u0000-\u001f]/.test(it.format))
    return { status: 'error', error: 'bad_url' };
  if (/^pair\|/.test(it.format)) {
    // Both playlists must be ones this tab actually loaded.
    const m = /^pair\|([^|\s"]+)\|v=([^|\s"]+)\|([^|\s"].*)$/.exec(it.format);
    if (!m || !isHttpUrl(m[1]) || !isHttpUrl(m[2]) || !sniffedFor(tab, m[1]) || !sniffedFor(tab, m[2]) ||
        !sniffedFor(tab, pageUrl) || m[3].length > 400)
      return { status: 'error', error: 'bad_url' };
  } else if (it.format.length > 400) return { status: 'error', error: 'bad_url' };
  const pr = await probed(pageUrl, tab);
  const title = safeName((pr && pr.title) || (tab && tab.title) || '');
  const r = await sendToNdm({
    kind: 'media', url: pageUrl, pageUrl, mediaFormat: it.format,
    cookies: pr && pr.anon ? '' : undefined,
    qualityLabel: String(it.label || '').replace(/[\u0000-\u001f]/g, '').slice(0, 60), referrer, storeId,
    filename: title + '.' + ext, fileSize: size,
    source: 'video_button',
  });
  if (r.status !== 'accepted') notifyFailure('Nee Download Manager', r);
  return r;
}

// ─── Context menus ──────────────────────────────────────────────────────────

// Firefox: "menus" (its own name; the manifest asks for it alone), Chrome: "contextMenus".
const menus = api.menus || api.contextMenus;

async function buildMenus() {
  if (!menus) return;
  try { await menus.removeAll(); } catch (e) { /* ignore */ }
  const mk = (o) => { try { menus.create(o, () => void api.runtime.lastError); } catch (e) {} };
  mk({ id: 'ndm_link',  title: 'Download with NDM',            contexts: ['link'] });
  mk({ id: 'ndm_image', title: 'Download image with NDM',      contexts: ['image'] });
  mk({ id: 'ndm_media', title: 'Download media with NDM',      contexts: ['video', 'audio'] });
  // Page / selection only: on a link or an image the menu already has "Download (image) with NDM".
  mk({ id: 'ndm_all',   title: 'Download all links with NDM',  contexts: ['page', 'selection'] });
}

if (menus) {
  menus.onClicked.addListener((info, tab) => {
    if (info.menuItemId === 'ndm_all') { openPicker(tab).catch(() => {}); return; }
    const url = info.menuItemId === 'ndm_link' ? info.linkUrl : info.srcUrl;
    const page = info.frameUrl || info.pageUrl || (tab && tab.url) || '';
    if (info.menuItemId === 'ndm_media' && !isNetUrl(url) && isNetUrl(page)) {
      // Streamed (blob:) video: let yt-dlp take it from the page, best quality.
      sendToNdm({ kind: 'media', url: page, pageUrl: page, mediaFormat: 'bv*+ba/b', qualityLabel: 'Best',
                  referrer: page, storeId: tabStore(tab), filename: safeName(tab && tab.title) + '.mp4',
                  source: 'menu_media' })
        .then(r => { if (r.status !== 'accepted') notifyFailure('Nee Download Manager', r); }, () => {});
      return;
    }
    if (info.menuItemId === 'ndm_image' && isHttpUrl(url) && !IMAGE_EXT.test(extOf(nameFromUrl(url)))) {
      imageExt(url).then(e => downloadOne(url, page, tab, 'menu_image',
        { filename: safeName(nameFromUrl(url) || (tab && tab.title)) + '.' + e })).catch(() => {});
      return;
    }
    downloadOne(url, page, tab, info.menuItemId.replace('ndm_', 'menu_')).catch(() => {});
  });
}

async function downloadOne(url, referrer, tab, source, extra) {
  if (!isNetUrl(url) && !isMagnet(url)) {
    notify('Cannot download with NDM', /^blob:/i.test(url || '')
      ? 'This media is streamed (blob:) and has no direct address NDM can download.'
      : 'Only http, https and magnet addresses can be sent to NDM.');
    return { status: 'error', error: 'bad_url' };
  }
  const r = await sendToNdm(Object.assign({ url, referrer, storeId: tabStore(tab), silent: false, source,
                                            filename: isMagnet(url) ? '' : safeName(nameFromUrl(url)) }, extra || {}));
  if (r.status !== 'accepted') notifyFailure('Nee Download Manager', r);
  return r;
}

// ─── Download all links (picker) ────────────────────────────────────────────

function collectLinksInPage() {
  const out = [];
  const seen = new Set();
  const add = (u, text, kind) => {
    try {
      const abs = new URL(u, location.href).href;
      if (!/^https?:\/\//i.test(abs) || seen.has(abs)) return;
      seen.add(abs);
      out.push({ url: abs, text: String(text || '').replace(/\s+/g, ' ').trim().slice(0, 160), kind });
    } catch (e) { /* ignore */ }
  };
  document.querySelectorAll('a[href], area[href]').forEach(a => add(a.href, a.textContent || a.title, 'link'));
  document.querySelectorAll('video, audio').forEach(m => {
    add(m.currentSrc || m.src, m.title, 'media');
    m.querySelectorAll('source[src]').forEach(s => add(s.src, m.title, 'media'));
  });
  document.querySelectorAll('img[src]').forEach(i => add(i.currentSrc || i.src, i.alt, 'image'));
  return { url: location.href, title: document.title, links: out.slice(0, 5000) };
}

async function collectFromTab(tab) {
  if (api.scripting && api.scripting.executeScript) {
    const res = await api.scripting.executeScript({ target: { tabId: tab.id }, func: collectLinksInPage });
    return res && res[0] && res[0].result;
  }
  const res = await api.tabs.executeScript(tab.id, { code: '(' + collectLinksInPage.toString() + ')()' });
  return res && res[0];
}

async function openPicker(tab) {
  if (!tab || tab.id == null) return { ok: false, error: 'no_tab' };
  let data;
  try { data = await collectFromTab(tab); }
  catch (e) {
    notify('Download all links', 'This page cannot be read by extensions (browser or store page).');
    return { ok: false, error: 'blocked' };
  }
  if (!data || !data.links || !data.links.length) {
    notify('Download all links', 'No downloadable links were found on this page.');
    return { ok: false, error: 'empty' };
  }
  data.tabIncognito = !!tab.incognito;
  data.cookieStoreId = tab.cookieStoreId || '';
  await api.storage.local.set({ [PICKER_KEY]: data });
  await api.tabs.create({ url: api.runtime.getURL('links.html'), index: tab.index + 1, openerTabId: tab.id });
  return { ok: true, count: data.links.length };
}

async function sendLinks(links, referrer, storeId) {
  links = (links || []).filter(l => l && isNetUrl(l.url)).slice(0, 5000);
  if (!links.length) return { status: 'error', error: 'no_links' };
  // Cookies per origin (the batch may span several hosts).
  const byOrigin = new Map();
  for (const l of links) {
    let origin = '';
    try { origin = new URL(l.url).origin; } catch (e) { continue; }
    if (!byOrigin.has(origin) && byOrigin.size < 100) byOrigin.set(origin, await cookiesFor(l.url, storeId, referrer));
  }
  const payload = links.map(l => {
    let origin = '';
    try { origin = new URL(l.url).origin; } catch (e) {}
    return { url: l.url, filename: l.filename || nameFromUrl(l.url), cookies: byOrigin.get(origin) || '' };
  });
  const r = await nativeRequest({
    action: 'addDownloads', referrer: referrer || '', userAgent: navigator.userAgent,
    silent: true, links: payload,
  }, 60000);
  await addHistory({ url: referrer || '', filename: links.length + ' links', source: 'all_links',
                     status: r.status === 'accepted' ? 'sent' : 'failed', error: r.error || '' });
  if (r.status !== 'accepted') notifyFailure('Nee Download Manager', r);
  return r;
}

// ─── Messages from extension pages and the content script ───────────────────

function fromExtensionPage(sender) {
  const base = api.runtime.getURL('');
  return !sender.tab || (sender.url && sender.url.startsWith(base));
}

async function handleMessage(msg, sender) {
  const ext = fromExtensionPage(sender);
  const tab = sender.tab;
  if (!ext) {
    // Content script (page-controlled input): a small, validated set only.
    if (!tab) return { ok: false, error: 'forbidden' };
    // What this tab was seen loading, also after a service-worker restart.
    if (/^media/.test(String(msg.action))) await restoreTab(tab.id);
    switch (msg.action) {
      case 'bypassNext':
        if (isNetUrl(msg.url) && msg.url.length < 8192) bypass.set(msg.url, Date.now());
        while (bypass.size > 50) bypass.delete(bypass.keys().next().value);
        return { ok: true };
      case 'mediaSniffed': {
        const list = tabMedia.get(tab.id) || [];
        return { items: sniffedItems(tab.id, isHttpUrl(msg.src) ? msg.src : '', msg.kind,
                                     { frameId: sender.frameId, mediaId: msg.mediaId, single: msg.single === true }),
                 // whole audio files, or audio streams on a page without video
                 audioSeen: list.some(m => m.kind === 'audio' && m.type === 'file') ||
                            (list.some(m => m.kind === 'audio') && !list.some(m => m.kind === 'video')) };
      }
      case 'mediaList':
        return mediaList(msg, sender);
      case 'mediaPrefetch':
        if (pageAllowed(msg.pageUrl, sender))
          probe(msg.pageUrl, tab, pageAllowed(msg.referrer, sender) ? msg.referrer : '', 'prefetch').catch(() => {});
        return { ok: true };
      case 'mediaPick':
        return sendMediaItem(msg, sender);
      case 'magnet': {
        const s = settingsCache || await loadSettings();
        if (!s.enabled || !isMagnet(msg.url)) return { status: 'error', error: 'disabled' };
        if (!magnetAllowed()) return { status: 'error', error: 'rate_limited' };
        return sendToNdm({ url: msg.url, referrer: isHttpUrl(tab.url) ? tab.url : '', cookies: '', source: 'magnet' });
      }
    }
    return { ok: false, error: 'forbidden' };
  }
  switch (msg.action) {
    case 'getState': {
      const s = settingsCache || await loadSettings();
      return { settings: s, defaults: DEFAULTS };
    }
    case 'saveSettings':
      return { settings: await saveSettings(msg.settings) };
    case 'status':
      return hostStatus();
    case 'addUrl': {
      const urls = String(msg.url || '').split(/\s+/).filter(u => isNetUrl(u) || isMagnet(u));
      if (!urls.length) return { status: 'error', error: 'bad_url' };
      if (urls.length === 1) return downloadOne(urls[0], msg.referrer || '', null, 'popup');
      return sendLinks(urls.filter(isNetUrl).map(u => ({ url: u })), msg.referrer || '');
    }
    case 'grabLinks': {
      const tabs = await api.tabs.query({ active: true, currentWindow: true });
      return openPicker(tabs[0]);
    }
    case 'getPicker': {
      const st = await api.storage.local.get(PICKER_KEY);
      return st[PICKER_KEY] || null;
    }
    case 'sendLinks': {
      const store = msg.cookieStoreId || (msg.incognito ? (IS_FIREFOX ? 'firefox-private' : '1') : undefined);
      return sendLinks(msg.links, msg.referrer, store);
    }
    case 'showNdm':
      return nativeRequest({ action: 'show' }, 25000);
    case 'getHistory': {
      const st = await api.storage.local.get(HISTORY_KEY);
      return st[HISTORY_KEY] || [];
    }
    case 'clearHistory':
      await api.storage.local.set({ [HISTORY_KEY]: [] });
      return { ok: true };
  }
  return { ok: false, error: 'unknown_action' };
}

api.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (!msg || typeof msg.action !== 'string') return false;
  handleMessage(msg, sender).then(sendResponse,
    (e) => sendResponse({ status: 'error', error: 'internal', detail: String(e && e.message || e) }));
  return true;
});

// ─── Lifecycle ──────────────────────────────────────────────────────────────

api.runtime.onInstalled.addListener(async (details) => {
  await loadSettings();
  buildMenus();
  refreshCaptureState();
  if (details.reason === 'install') {
    const p = api.tabs.create({ url: api.runtime.getURL('welcome.html') });
    if (p && typeof p.catch === 'function') p.catch(() => {});
  }
});

// Capture state: pinged at browser start, then lazily (captureState()) when
// downloads happen — not on every service-worker wake.
if (api.runtime.onStartup) api.runtime.onStartup.addListener(() => { loadSettings(); buildMenus(); refreshCaptureState(); });

loadSettings();
