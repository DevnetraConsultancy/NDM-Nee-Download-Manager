/**
 * Nee Download Manager — content script (Chrome/Edge/Firefox, identical copies)
 *
 *  - Icon-only download button in the top-right corner of any visible /
 *    playing video (YouTube, X, Facebook, Instagram, Reddit, Vimeo, TikTok,
 *    generic HTML5 players, also inside iframes and open shadow roots): click
 *    -> quality list from NDM (yt-dlp: "Best (1080p MP4)", the other
 *    resolutions, "Best Audio (Opus 160 kbps)", the site's other audio
 *    tracks, MP3 conversions) plus whole media files of that element -> pick
 *    one -> NDM's dialog.
 *  - The same button on audio: visible <audio> players, and the play
 *    control of custom players on music / podcast sites or wherever the tab
 *    was seen loading audio.
 *  - Icon-only button on large photos on any site, sending the largest
 *    source (srcset / <picture> / lazy attributes / site original rules).
 *  - Alt+click on a link: let the browser download it (NDM bypass).
 *  - magnet: links go to NDM (capture on, site not excluded, no Alt).
 *
 * UI lives in a closed shadow root; page clicks are never blocked except a
 * magnet: link we hand to NDM (re-clicked for the OS if NDM is unreachable).
 * File names are never sent from here: the background derives them.
 */
(function () {
  'use strict';
  if (window.__ndmContentLoaded) return;
  window.__ndmContentLoaded = true;

  const api = (typeof browser !== 'undefined' && browser.runtime) ? browser : chrome;
  const KEY = 'ndmSettings';
  const HOSTNAME = location.hostname.toLowerCase().replace(/^www\./, '');
  const onHost = (re) => re.test(HOSTNAME);
  const SITE = onHost(/(^|\.)youtube(-nocookie)?\.com$/) ? 'yt'
             : onHost(/(^|\.)facebook\.com$/) ? 'fb'
             : onHost(/(^|\.)instagram\.com$/) ? 'ig'
             : onHost(/(^|\.)(x|twitter)\.com$/) ? 'x'
             : onHost(/(^|\.)reddit\.com$/) ? 'reddit'
             : onHost(/(^|\.)tiktok\.com$/) ? 'tiktok'
             : onHost(/(^|\.)soundcloud\.com$/) ? 'sc'
             : onHost(/(^|\.)google\.[a-z.]+$/) ? 'google' : '';
  // Music / podcast sites: a play control means audio even before it plays.
  const AUDIO_SITE = SITE === 'sc' || onHost(/(^|\.)(bandcamp\.com|mixcloud\.com|audiomack\.com|music\.youtube\.com|podcasts\.apple\.com|podbean\.com|buzzsprout\.com|anchor\.fm|podcasters\.spotify\.com|audioboom\.com|spreaker\.com|castbox\.fm|iheart\.com|tunein\.com|jamendo\.com|freesound\.org|archive\.org|hearthis\.at|reverbnation\.com)$/);
  const AD_FRAME = onHost(/(^|\.)(doubleclick\.net|googlesyndication\.com|adservice\.google\.[a-z.]+|imasdk\.googleapis\.com|adnxs\.com|teads\.tv)$/);
  const NO_PHOTOS = SITE === 'google' && /^\/maps/.test(location.pathname);
  const MIN_W = 200, MIN_H = 110, MIN_PHOTO = 200;

  let enabled = true, videoButton = true, excluded = false;

  function apply(s) {
    s = s || {};
    enabled = s.enabled !== false;
    videoButton = s.videoButton !== false;
    excluded = (s.excludedSites || []).some(x => HOSTNAME === x || HOSTNAME.endsWith('.' + x));
    if (!overlayOn()) hide(true);
  }
  function overlayOn() { return videoButton && !excluded && !AD_FRAME; }

  /** Message to the background; resolves to the reply or null. */
  function ask(msg) {
    return new Promise((resolve) => {
      try {
        const p = api.runtime.sendMessage(msg, (r) => { void api.runtime.lastError; resolve(r || null); });
        if (p && typeof p.then === 'function') p.then(r => resolve(r || null), () => resolve(null));
      } catch (e) { resolve(null); }          // extension reloaded
    });
  }

  try {
    const r = api.storage.local.get(KEY, (st) => { void api.runtime.lastError; apply(st && st[KEY]); });
    if (r && typeof r.then === 'function') r.then(st => apply(st && st[KEY]), () => {});
    api.storage.onChanged.addListener((ch, area) => {
      if (area === 'local' && ch[KEY]) apply(ch[KEY].newValue);
    });
  } catch (e) { /* ignore */ }

  // ── Alt+click bypass, magnet: links ───────────────────────────────────────
  // Only real user clicks: a page script calling a.click() / dispatchEvent()
  // never reaches NDM (the browser then handles the link as usual).
  let lastMagnet = 0;
  document.addEventListener('click', (e) => {
    const a = e.target && e.target.closest ? e.target.closest('a[href]') : null;
    // Our own fallback re-click (untrusted) goes to the OS handler; the flag is
    // reset right after it (see below), never left for the next real click.
    if (a && a.__ndmPass) return;
    if (!e.isTrusted) return;
    if (e.button !== 0 || !a) return;
    const href = a.href || '';
    if (e.altKey) {
      if (/^https?:/i.test(href)) ask({ action: 'bypassNext', url: href });
      return;                                   // Alt+click magnet: OS handler
    }
    if (!/^magnet:\?/i.test(href) || !enabled || excluded) return;
    e.preventDefault();
    e.stopPropagation();
    const now = Date.now();
    if (now - lastMagnet < 1500) return;          // one torrent dialog per click burst
    lastMagnet = now;
    ask({ action: 'magnet', url: href }).then((r) => {
      if (r && (r.status === 'accepted' || r.error === 'rate_limited')) return;
      a.__ndmPass = true;                       // NDM unreachable / capture off: normal handling
      try { a.click(); } finally { a.__ndmPass = false; }   // click() dispatches synchronously
    });
  }, true);

  // ── Overlay (button + quality list) ───────────────────────────────────────
  // Icon-only button tucked into the media's top-right corner: a small dark
  // glass square with a white Lucide-style "download" arrow (photos: "image
  // down"), gold on hover / while the list is open. Text appears only for a
  // moment after a pick ("Sent to NDM").
  const ICON = '<svg class="ic dl" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" ' +
               'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 15V3"/>' +
               '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><path d="m7 10 5 5 5-5"/></svg>' +
               '<svg class="ic im" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" ' +
               'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
               '<path d="M10.3 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2v10l-3.1-3.1a2 2 0 0 0-2.814.014L6 21"/>' +
               '<path d="m14 19 3 3v-6"/><path d="m17 22 3-3"/><circle cx="9" cy="9" r="2"/></svg>';
  const GOLD = '#E8B640';
  const CSS =
    ':host{all:initial}' +
    '*{box-sizing:border-box;font-family:"Segoe UI",system-ui,sans-serif}' +
    '.b{all:unset;box-sizing:border-box;display:flex;align-items:center;justify-content:center;gap:6px;height:30px;' +
    'min-width:30px;padding:0 9px 0 7px;cursor:pointer;font-size:12px;font-weight:600;color:#fff;' +
    'background:rgba(0,0,0,.55);-webkit-backdrop-filter:blur(6px);backdrop-filter:blur(6px);' +
    'border:1px solid rgba(255,255,255,.16);border-radius:8px;box-shadow:0 1px 6px rgba(0,0,0,.35);' +
    'white-space:nowrap;user-select:none;transition:background-color .12s ease,color .12s ease}' +
    '.b.i{width:30px;padding:0}.b.i span{display:none}' +
    '.b:hover,.b.on{background:rgba(0,0,0,.8);color:' + GOLD + '}' +
    '.b:focus-visible{outline:2px solid ' + GOLD + ';outline-offset:2px}' +
    '.b svg{width:16px;height:16px;flex:none;display:block}' +
    '.b .im,.b.ph .dl{display:none}.b.ph .im{display:block}' +
    '.m{position:absolute;left:0;top:34px;min-width:200px;max-width:300px;max-height:320px;overflow:auto;display:none;' +
    'background:#1A1D23;border:1px solid #3A3F4B;border-radius:8px;padding:4px;box-shadow:0 8px 24px rgba(0,0,0,.55)}' +
    '.m.open{display:block}' +
    '.r{display:flex;align-items:center;gap:14px;justify-content:space-between;padding:6px 9px;border-radius:5px;' +
    'font-size:12px;color:#DCDCE1;cursor:pointer;white-space:nowrap}' +
    '.r.a1{margin-top:4px;box-shadow:0 -4px 0 -3px #2E323C}' +
    '.r:hover{background:#282C36;color:#F0D264}' +
    '.r .s{color:#8C8E96;font-size:11px}' +
    '.r.d{cursor:default;color:#8C8E96}.r.d:hover{background:none;color:#8C8E96}' +
    '.sp{width:11px;height:11px;border:2px solid #373C48;border-top-color:#D4AF37;border-radius:50%;' +
    'animation:k .8s linear infinite;flex:none}@keyframes k{to{transform:rotate(360deg)}}';

  let host = null, root = null, btn = null, menu = null, label = null;
  let target = null, targetKind = '', hideTimer = null, followTimer = null, menuOpen = false, flashTimer = null;
  let prefetchTimer = null;
  const PREFETCH_DWELL_MS = 500;
  const lists = new Map();       // kind|probe page url -> {t, r}

  // Top layer (popover API, Chrome 114+ / Firefox 125+): the button then sits
  // in viewport coordinates whatever the page does to <html> (Dark Reader's
  // filter, transforms, zoom wrappers). Elsewhere place() measures and
  // corrects the offset such an ancestor gives position:fixed.
  const CAN_POPOVER = typeof HTMLElement === 'function' && !!HTMLElement.prototype &&
                      typeof HTMLElement.prototype.showPopover === 'function';
  let vx = 0, vy = 0;            // where the host should be (viewport px)

  function inTopLayer() {
    try { return CAN_POPOVER && !!host && host.matches(':popover-open'); } catch (e) { return false; }
  }
  function topLayer(on) {
    if (!CAN_POPOVER || !host) return;
    try {
      const open = host.matches(':popover-open');
      if (on && !open && host.isConnected) host.showPopover();
      else if (!on && open) host.hidePopover();
    } catch (e) { /* not connected / page removed us */ }
  }

  function build() {
    if (host) return;
    host = document.createElement('ndm-overlay');
    host.style.cssText = 'all:initial;position:fixed;z-index:2147483647;top:0;left:0;display:none;';
    if (CAN_POPOVER) { try { host.popover = 'manual'; } catch (e) { /* ignore */ } }
    root = host.attachShadow({ mode: 'closed' });
    root.innerHTML = '<style>' + CSS + '</style><div class="b i" role="button" tabindex="0" title="Download with NDM" ' +
                     'aria-label="Download with NDM">' + ICON + '<span></span></div><div class="m" role="menu"></div>';
    btn = root.querySelector('.b');
    menu = root.querySelector('.m');
    label = btn.querySelector('span');
    // Keep the page (player play/pause, feed links) from seeing our clicks.
    for (const ev of ['click', 'mousedown', 'mouseup', 'pointerdown', 'pointerup', 'dblclick', 'contextmenu', 'touchstart'])
      host.addEventListener(ev, (e) => e.stopPropagation());
    btn.addEventListener('click', (e) => { if (e.isTrusted) onButton(); });
    btn.addEventListener('keydown', (e) => {
      if (e.isTrusted && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); onButton(); }
    });
    // Prefetch the quality list only when the pointer rests on the button
    // (PREFETCH_DWELL_MS): sweeping over a feed must not queue probes.
    btn.addEventListener('mouseenter', () => {
      clearTimeout(prefetchTimer);
      if (targetKind !== 'video' && targetKind !== 'audio') return;
      const el = target;
      prefetchTimer = setTimeout(() => {
        prefetchTimer = null;
        if (target !== el || menuOpen || (targetKind !== 'video' && targetKind !== 'audio')) return;
        const p = probeTarget();
        ask({ action: 'mediaPrefetch', pageUrl: p.pageUrl, referrer: p.referrer });
      }, PREFETCH_DWELL_MS);
    });
    btn.addEventListener('mouseleave', () => { clearTimeout(prefetchTimer); prefetchTimer = null; });
    host.addEventListener('mouseenter', () => clearTimeout(hideTimer));
    host.addEventListener('mouseleave', () => { if (!menuOpen) scheduleHide(); });
    attach();
  }

  function attach() {
    const fs = document.fullscreenElement || document.webkitFullscreenElement;
    const parent = (fs && fs.tagName !== 'VIDEO' && fs.tagName !== 'IMG') ? fs : (document.documentElement || document.body);
    if (host && parent && host.parentNode !== parent) {
      parent.appendChild(host);                  // leaves the top layer: show again above the fullscreen element
      if (host.style.display === 'block') topLayer(true);
    }
  }

  /** Something between us and the viewport that re-bases position:fixed (filter, transform, ...)? */
  function rebasedFixed() {
    if (typeof getComputedStyle !== 'function') return false;
    for (let n = host.parentElement, i = 0; n && i < 30; n = n.parentElement, i++) {
      let cs = null;
      try { cs = getComputedStyle(n); } catch (e) { return false; }
      if (!cs) return false;
      const set = (v) => !!v && v !== 'none';
      if (set(cs.transform) || set(cs.filter) || set(cs.perspective) || set(cs.backdropFilter) || set(cs.translate) ||
          set(cs.scale) || set(cs.rotate) || /paint|layout|strict|content/.test(cs.contain || '') ||
          /transform|filter|perspective/.test(cs.willChange || '')) return true;
    }
    return false;
  }

  /** Puts the host at viewport (x, y). */
  function moveHost(x, y) {
    vx = x; vy = y;
    host.style.left = x + 'px';
    host.style.top = y + 'px';
    if (inTopLayer() || !rebasedFixed()) return;
    // Measured, not guessed: browsers differ (Firefox exempts a filter on the root element).
    const r = host.getBoundingClientRect();
    const dx = x - r.left, dy = y - r.top;
    if (Math.abs(dx) >= 0.5 || Math.abs(dy) >= 0.5) {
      host.style.left = (x + dx) + 'px';
      host.style.top = (y + dy) + 'px';
    }
  }

  /** Quality list anchored to the button, kept inside the viewport on every side. */
  function layoutMenu() {
    if (!menuOpen || !menu || !btn) return;
    const M = 8;
    const bw = btn.offsetWidth || 30, bh = btn.offsetHeight || 30;
    const maxH = Math.max(120, Math.min(320, innerHeight - 2 * M));
    menu.style.maxHeight = maxH + 'px';
    const mw = Math.min(menu.offsetWidth || 220, innerWidth - 2 * M);
    const mh = Math.min(menu.offsetHeight || 0, maxH);
    // right edge under the button's right edge, then clamped (left wins on a tiny viewport)
    let left = Math.min(bw - mw, innerWidth - M - vx - mw);
    left = Math.max(left, M - vx);
    let top = bh + 4;                                        // below the button
    if (vy + top + mh > innerHeight - M) {
      const above = -mh - 4;
      top = vy + above >= M ? above : Math.max(M - vy, innerHeight - M - vy - mh);
    }
    menu.style.left = Math.round(left) + 'px';
    menu.style.top = Math.round(top) + 'px';
  }

  function rectOf(el) {
    // YouTube: the player box (the <video> can be letterboxed inside it).
    const box = SITE === 'yt' && el.closest && el.closest('#movie_player, .html5-video-player');
    return (box || el).getBoundingClientRect();
  }

  function inView(r) { return r.bottom > 0 && r.top < innerHeight && r.right > 0 && r.left < innerWidth; }

  function usable(el, kind) {
    if (!el || !el.isConnected) return false;
    const r = rectOf(el);
    if (kind === 'photo') {
      if (r.width < MIN_PHOTO || r.height < MIN_PHOTO || !bestImage(el)) return false;
    } else if (kind === 'audio') {
      if (el.tagName === 'AUDIO') { if (r.width < 100 || r.height < 16) return false; }
      else if (r.width < 12 || r.height < 12 || r.width > 200 || r.height > 200) return false;
    } else {
      if (r.width < MIN_W || r.height < MIN_H) return false;
      const player = SITE === 'yt' && el.closest('.html5-video-player');
      if (player && player.classList.contains('ad-showing')) return false;
    }
    return inView(r);
  }

  function place() {
    if (!target) return;
    if (!usable(target, targetKind) || !overlayOn()) { hide(true); return; }
    const r = rectOf(target);
    const w = host.offsetWidth || 30;
    const INSET = 8;                                                     // tucked into the corner
    let top, left;
    if (targetKind === 'audio' && target.tagName !== 'AUDIO') {          // next to a play control
      top = r.top + (r.height - 30) / 2;
      left = r.right + 6;
      if (left + w > innerWidth - 4) left = r.left - w - 6;
    } else if (targetKind === 'audio' && r.height < 60) {                // above a slim <audio> bar
      top = r.top >= 38 ? r.top - 34 : r.bottom + 4;
      left = r.right - w;
    } else {
      top = r.top + INSET;
      left = r.right - INSET - w;
    }
    moveHost(Math.min(Math.max(4, left), innerWidth - w - 4), Math.min(Math.max(4, top), innerHeight - 34));
    layoutMenu();
  }

  function setLabel(t) {
    if (!label) return;
    label.textContent = t;
    btn.classList[t ? 'remove' : 'add']('i');           // at rest: icon only
    if (target) place();                                // a wider button grows to the left
  }

  function show(el, kind) {
    if (!overlayOn() || el.__ndmHidden) return;
    build();
    clearTimeout(hideTimer);
    if (target !== el) {
      if (menuOpen) return;                        // keep the open list on its element
      target = el;
      targetKind = kind;
      clearTimeout(flashTimer);
      btn.classList[kind === 'photo' ? 'add' : 'remove']('ph');
      const tip = kind === 'photo' ? 'Download photo with NDM' : kind === 'audio' ? 'Download audio with NDM' : 'Download with NDM';
      btn.title = tip;
      if (btn.setAttribute) btn.setAttribute('aria-label', tip);
      setLabel('');
    }
    attach();
    host.style.display = 'block';
    topLayer(true);
    place();
    if (!followTimer) followTimer = setInterval(place, 300);
  }

  function hide(force) {
    clearTimeout(hideTimer);
    if (menuOpen && !force) return;
    clearTimeout(prefetchTimer);
    prefetchTimer = null;
    closeMenu();
    topLayer(false);
    if (host) host.style.display = 'none';
    target = null;
    clearInterval(followTimer);
    followTimer = null;
  }

  function scheduleHide(ms) {
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => hide(false), ms || 1500);
  }

  function flash(t) {
    setLabel(t);
    clearTimeout(flashTimer);
    flashTimer = setTimeout(() => setLabel(''), 1800);
  }

  // ── What to download ──────────────────────────────────────────────────────

  function absUrl(u) {
    if (!u || typeof u !== 'string') return '';
    try { u = new URL(u.trim(), location.href).href; } catch (e) { return ''; }
    return /^https?:\/\//i.test(u) ? u : '';
  }

  function mediaSrc(el) {
    if (!el || (el.tagName !== 'VIDEO' && el.tagName !== 'AUDIO')) return '';
    return absUrl(el.currentSrc || el.src || (el.querySelector && (el.querySelector('source[src]') || {}).src) || '');
  }

  /** Larger originals on sites with well-known image CDNs. */
  function upgradeImage(u) {
    let x;
    try { x = new URL(u); } catch (e) { return u; }
    const h = x.hostname.toLowerCase();
    if (h === 'pbs.twimg.com' && /^\/media\//.test(x.pathname)) {
      // /media/ID?format=jpg&name=small | /media/ID.jpg:large -> ?format=jpg&name=orig
      const m = /^(\/media\/[^./:]+)(?:\.(\w+))?(?::\w+)?$/.exec(x.pathname);
      if (m) return 'https://pbs.twimg.com' + m[1] + '?format=' + encodeURIComponent(x.searchParams.get('format') || m[2] || 'jpg') + '&name=orig';
    }
    if (h === 'i.pinimg.com' && /^\/\d+x\d*\//.test(x.pathname))
      return 'https://i.pinimg.com/originals/' + x.pathname.replace(/^\/\d+x\d*\//, '');
    if (h === 'preview.redd.it') {
      const name = x.pathname.split('/').pop() || '';
      const m = /-v0-([a-z0-9]+)\.(\w+)$/i.exec(name);
      if (m || /^[a-z0-9]+\.\w+$/i.test(name)) return 'https://i.redd.it/' + (m ? m[1] + '.' + m[2] : name);
    }
    return u;
  }

  /** "image-300x200@2x_small" and "image" share a stem. */
  function imgStem(u) {
    try {
      const seg = decodeURIComponent(new URL(u).pathname.split('/').pop() || '');
      return seg.replace(/\.[a-z0-9]{2,5}$/i, '').replace(/([-_]\d{2,4}x\d{2,4}|@\dx|[-_](small|medium|large|thumb|thumbnail|preview|scaled))+$/i, '').toLowerCase();
    } catch (e) { return ''; }
  }

  /** The page's og:image when it is the same picture as u (the article's main image, full size). */
  function ogFor(u) {
    if (!document.querySelector) return '';
    const m = document.querySelector('meta[property="og:image"], meta[name="og:image"], meta[property="og:image:url"]');
    const og = absUrl(m && m.getAttribute('content'));
    const st = imgStem(u);
    return og && og !== u && st.length >= 4 && imgStem(og) === st ? og : '';
  }

  /** srcset candidates (HTML tokenizer rules: URLs may contain commas, "a 1x,b 2x" needs no space). */
  function srcsetBest(set, natural, cands) {
    const s = String(set || '');
    let i = 0;
    while (i < s.length) {
      while (i < s.length && /[\s,]/.test(s[i])) i++;
      let j = i;
      while (j < s.length && !/\s/.test(s[j])) j++;
      let url = s.slice(i, j);
      let desc = '';
      if (/,+$/.test(url)) url = url.replace(/,+$/, '');                 // "url," : no descriptor
      else { const k = s.indexOf(',', j); desc = s.slice(j, k < 0 ? s.length : k).trim(); j = k < 0 ? s.length : k + 1; }
      i = j;
      if (!url) continue;
      const m = /^(\d+(?:\.\d+)?)([wx])$/.exec(desc);
      const w = !m ? (natural || 1) : m[2] === 'w' ? +m[1] : +m[1] * (natural || 1000);
      cands.push({ u: url, w });
    }
  }

  const BIG_ATTRS = ['data-orig-file', 'data-large-file', 'data-full-src', 'data-full', 'data-hi-res-src', 'data-highres',
                     'data-zoom-image', 'data-zoom-src', 'data-large_image', 'data-original-src'];
  const LAZY_ATTRS = ['data-src', 'data-original', 'data-lazy-src', 'data-lazy', 'data-url'];

  /**
   * Largest source of a photo element: <img> srcset / <picture> sources /
   * lazy-load attributes / site rules; a CSS background-image URL for other
   * elements. "" when there is nothing http(s) worth offering.
   */
  function bestImage(el) {
    if (!el) return '';
    let u = '';
    if (el.tagName === 'IMG') {
      const natural = +el.naturalWidth || 0;
      if (natural && natural < 120) return '';                          // icon stretched up / sprite / pixel
      if (SITE === 'google') {                                          // Google Images: the original page's file
        const a = el.closest && el.closest('a[href*="imgurl="]');
        if (a) { try { const o = absUrl(new URL(a.getAttribute('href'), location.href).searchParams.get('imgurl')); if (o) return o; } catch (e) {} }
      }
      const cands = [];
      srcsetBest(el.getAttribute('srcset'), natural, cands);
      srcsetBest(el.getAttribute('data-srcset'), natural, cands);
      const pic = el.parentElement;
      if (pic && pic.tagName === 'PICTURE')
        for (const s of pic.children || []) if (s.tagName === 'SOURCE') {
          srcsetBest(s.getAttribute('srcset'), natural, cands);
          srcsetBest(s.getAttribute('data-srcset'), natural, cands);
        }
      for (const k of BIG_ATTRS) { const v = el.getAttribute(k); if (v) cands.push({ u: v, w: 1e9 }); }
      const cur = el.currentSrc || el.src || '';
      cands.push({ u: cur, w: natural || 1 });
      if (!absUrl(cur)) for (const k of LAZY_ATTRS) { const v = el.getAttribute(k); if (v) cands.push({ u: v, w: natural || 1 }); }
      let bestW = -1;
      for (const c of cands) {
        const a = absUrl(c.u);
        if (a && c.w > bestW) { bestW = c.w; u = a; }
      }
    } else {
      u = bgImage(el);
    }
    if (!u || /\.svg(\?|#|$)/i.test(u)) return '';
    u = upgradeImage(u);
    return ogFor(u) || u;
  }

  /** CSS background-image of an element that is clearly a photo (big, single url, no text). */
  function bgImage(el) {
    if (typeof getComputedStyle !== 'function' || !el || el === document.documentElement || el === document.body) return '';
    let bg = '';
    try { bg = getComputedStyle(el).backgroundImage || ''; } catch (e) { return ''; }
    const m = /^url\(\s*["']?([^"')]+)["']?\s*\)$/.exec(bg.trim());
    if (!m) return '';
    if (String(el.textContent || '').trim().length > 40) return '';
    const u = absUrl(m[1]);
    return u && !/sprite|icon|logo|avatar|placeholder|blank|spacer|pixel|gradient/i.test(u) ? u : '';
  }

  /** Parent across shadow-root boundaries (players inside web components). */
  function up(n) {
    if (!n) return null;
    if (n.parentElement) return n.parentElement;
    const r = n.getRootNode && n.getRootNode();
    return r && r !== n && r.host ? r.host : null;
  }

  function closestDeep(el, sel) {
    for (let n = el, i = 0; n && i < 60; n = up(n), i++) if (n.matches && n.matches(sel)) return n;
    return null;
  }

  function absHref(a) { try { return a ? new URL(a.getAttribute('href'), location.href).href : ''; } catch (e) { return ''; } }

  function nearLink(el, selector, levels) {
    for (let n = el, i = 0; n && i < levels; n = up(n), i++) {
      if (n.matches && n.matches(selector)) return absHref(n);
      const a = n.querySelector && n.querySelector(selector);
      if (a) return absHref(a);
    }
    return '';
  }

  /** X / Twitter: the post the video belongs to (feeds, replies, quotes). */
  function xStatus(el) {
    const clean = (u) => { const m = /^(https?:\/\/[^/]+\/[^/]+\/status\/\d+)/.exec(u || ''); return m ? m[1] : ''; };
    const art = el && closestDeep(el, 'article');
    if (art && art.querySelectorAll) {
      const links = [...art.querySelectorAll('a[href*="/status/"]')];
      const timed = links.find(a => a.querySelector && a.querySelector('time'));
      const u = clean(absHref(timed || links.find(a => /\/status\/\d+\/?$/.test(a.getAttribute('href') || '')) || links[0]));
      if (u) return u;
    }
    return clean(location.href);
  }

  function within(outer, n) {
    for (let p = n, i = 0; p && i < 80; p = up(p), i++) if (p === outer) return true;
    return false;
  }

  /**
   * X / Twitter: 1-based position of the clicked video among its post's
   * media (photos and videos, X's own /photo/N and /video/N numbering, which
   * yt-dlp follows), or 0: a single-media post, or a video inside a quoted post.
   */
  function xMediaIndex(el) {
    const art = el && closestDeep(el, 'article');
    if (!art || !art.querySelectorAll) return 0;
    const quoteOf = (n) => {
      for (let p = n, i = 0; p && p !== art && i < 60; p = up(p), i++)
        if (p.tagName === 'DIV' && p.getAttribute && p.getAttribute('role') === 'link') return p;
      return null;
    };
    if (quoteOf(el)) return 0;
    // The grid cell's own link, when X renders one (/status/ID/video/2).
    for (let n = el, i = 0; n && n !== art && i < 20; n = up(n), i++) {
      const m = n.tagName === 'A' && /\/status\/\d+\/(?:video|photo)\/([1-9])(?:[/?#]|$)/.exec(n.getAttribute('href') || '');
      if (m) return +m[1];
    }
    const cells = [...art.querySelectorAll('[data-testid="tweetPhoto"], [data-testid="videoPlayer"], [data-testid="videoComponent"]')]
      .filter(c => !quoteOf(c));
    const outer = cells.filter(c => !cells.some(o => o !== c && within(o, c)));
    if (outer.length < 2) return 0;
    const i = outer.findIndex(c => within(c, el));
    return i >= 0 && i < 9 ? i + 1 : 0;
  }

  /** X / Twitter: media id of the clicked video (from its poster), so the
   *  background matches the right sniffed playlists on a busy timeline. */
  function xMediaId(el) {
    const re = /\/(?:amplify_video_thumb|ext_tw_video_thumb)\/(\d{5,25})\//;
    const own = el && (el.getAttribute && el.getAttribute('poster') || '');
    let m = re.exec(own);
    if (m) return m[1];
    const box = el && (closestDeep(el, '[data-testid="videoComponent"], [data-testid="videoPlayer"]') || closestDeep(el, 'article'));
    const img = box && box.querySelector && box.querySelector('img[src*="_video_thumb/"]');
    m = img && re.exec(img.getAttribute('src') || '');
    return m ? m[1] : '';
  }

  /** Page address yt-dlp should read for the target. */
  function probeTarget() {
    const el = target;
    let pageUrl = location.href, mediaId = '';
    const referrer = window === window.top ? '' : document.referrer;
    if (SITE === 'yt') {
      const v = new URL(location.href).searchParams.get('v');
      const inMain = el && el.closest('#movie_player, #shorts-player, ytd-reel-video-renderer');
      if (inMain && /\/shorts\//.test(location.pathname)) pageUrl = location.href.split('?')[0];
      else if (inMain && v) pageUrl = 'https://www.youtube.com/watch?v=' + encodeURIComponent(v);
      else if (el) pageUrl = nearLink(el, 'a#media-container-link[href], a#thumbnail[href*="/watch"], a[href*="/watch?v="], a[href*="/shorts/"]', 12) || pageUrl;
      if (/\/embed\//.test(location.pathname)) pageUrl = location.href.split('?')[0];
    } else if (SITE === 'fb') {
      if (!/\/(videos|reel|watch|stories|story\.php)/.test(location.href) && el)
        pageUrl = nearLink(el, 'a[href*="/videos/"], a[href*="/reel/"], a[href*="/watch/?v="], a[href*="/watch?v="]', 25) || pageUrl;
    } else if (SITE === 'ig') {
      if (!/\/(p|reel|reels|tv|stories)\//.test(location.pathname) && el) {
        const art = el.closest('article') || el;
        pageUrl = nearLink(art, 'a[href*="/p/"], a[href*="/reel/"]', 3) || pageUrl;
      }
    } else if (SITE === 'x') {
      const st = xStatus(el);
      // 2nd / 3rd video of a post: "/status/ID/video/N" (yt-dlp then takes that one alone).
      const idx = st ? xMediaIndex(el) : 0;
      pageUrl = st ? st + (idx ? '/video/' + idx : '') : pageUrl;
      mediaId = xMediaId(el);
    } else if (SITE === 'reddit') {
      const post = el && closestDeep(el, 'shreddit-post, [data-permalink]');
      const perm = post && (post.getAttribute('permalink') || post.getAttribute('data-permalink'));
      pageUrl = (perm && absUrl(perm)) || (el && nearLink(el, 'a[href*="/comments/"]', 10)) || pageUrl;
    } else if (SITE === 'tiktok') {
      if (!/\/video\/\d+/.test(location.pathname) && el) pageUrl = nearLink(el, 'a[href*="/video/"]', 12) || pageUrl;
    } else if (SITE === 'sc') {
      if (el) pageUrl = nearLink(el, 'a.soundTitle__title[href], a.trackItem__trackTitle[href], a.playbackSoundBadge__titleLink[href]', 8) || pageUrl;
      pageUrl = pageUrl.split('?')[0];
    }
    return { pageUrl, referrer, mediaId };
  }

  // ── Menu ──────────────────────────────────────────────────────────────────

  function fmtSize(b, approx) {
    if (!(b > 0)) return '';
    const u = b >= 1073741824 ? [b / 1073741824, 'GB', 2] : b >= 1048576 ? [b / 1048576, 'MB', b >= 104857600 ? 0 : 1] : [b / 1024, 'KB', 0];
    return (approx ? '~' : '') + u[0].toFixed(u[2]) + ' ' + u[1];
  }

  function row(text, size, cls, onPick) {
    const d = document.createElement('div');
    d.className = 'r' + (cls ? ' ' + cls : '');
    const a = document.createElement('span');
    a.textContent = text;
    d.appendChild(a);
    if (size) { const s = document.createElement('span'); s.className = 's'; s.textContent = size; d.appendChild(s); }
    if (onPick) d.addEventListener('click', (e) => { e.stopPropagation(); if (e.isTrusted) onPick(); });
    return d;
  }

  /** "Best (1080p MP4)" + size -> "Best (1080p MP4 · 52 MB)"; "Best Audio (Opus 160 kbps)" alike. */
  function fmtRow(f, onPick) {
    const size = fmtSize(f.filesize, f.approx);
    const lbl = String(f.label || '');
    if (/^Best( Audio)? \(.*\)$/.test(lbl)) return row(size ? lbl.replace(/\)$/, ' · ' + size + ')') : lbl, '', '', onPick);
    return row(lbl, size, '', onPick);
  }

  function spinnerRow() {
    const d = row('Finding qualities…', '', 'd');
    const sp = document.createElement('span');
    sp.className = 'sp';
    d.insertBefore(sp, d.firstChild);
    d.style.justifyContent = 'flex-start';
    d.style.gap = '8px';
    return d;
  }

  // Every open / close starts a new generation: a list request still running
  // from an earlier opening must never fill the menu of a later one.
  let menuGen = 0;

  function closeMenu() {
    menuGen++;
    menuOpen = false;
    if (menu) { menu.classList.remove('open'); menu.textContent = ''; }
    if (btn) btn.classList.remove('on');
  }

  function pick(item, ctx) {
    closeMenu();
    flash('Sending…');
    // The background derives the file name itself (tab title / probe result).
    ask({ action: 'mediaPick', item, probeUrl: ctx.probeUrl || '', mime: ctx.mime || '' })
      .then(r => flash(r && r.status === 'accepted' ? 'Sent to NDM' : 'NDM not reachable'));
  }

  const isAudioFmt = (f) => (f.kind || (/^Audio /.test(f.label || '') ? 'audio' : 'video')) === 'audio';

  async function onButton() {
    if (!target) return;
    if (menuOpen) { closeMenu(); return; }
    if (targetKind === 'photo') {
      const url = bestImage(target);
      if (url) pick({ url, ext: 'jpg' }, { mime: 'image/*' });
      return;
    }
    const el = target, kind = targetKind;
    const gen = ++menuGen;
    const stale = () => gen !== menuGen || !menuOpen || target !== el;
    menuOpen = true;
    btn.classList.add('on');
    menu.textContent = '';
    menu.classList.add('open');

    const spin = spinnerRow();
    menu.appendChild(spin);
    layoutMenu();
    const src = mediaSrc(el);
    const p = probeTarget();
    // Files the tab loaded are offered only when they belong to THIS element:
    // its own address, its X media id, or the only <video> of this frame.
    const single = kind === 'video' && !!document.querySelectorAll && document.querySelectorAll('video').length <= 1;
    const sniff = await ask({ action: 'mediaSniffed', src, kind, mediaId: p.mediaId, single });
    if (stale()) return;
    const shown = new Set();
    const directRows = [];
    for (const it of (sniff && sniff.items) || []) {
      if (!it || !it.url || shown.has(it.url)) continue;
      shown.add(it.url);
      let lbl = it.label;
      if (it.url === src && kind === 'video' && el.videoHeight > 0) lbl = el.videoHeight + 'p ' + String(it.ext || 'mp4').toUpperCase();
      const d = row(lbl, fmtSize(it.filesize), '', () => pick(it, { mime: kind === 'audio' ? 'audio/*' : 'video/*' }));
      directRows.push(d);
      menu.appendChild(d);
    }
    layoutMenu();

    const ck = kind + '|' + p.pageUrl + '|' + p.mediaId;
    let cached = lists.get(ck);
    if (cached && Date.now() - cached.t > 240000) { lists.delete(ck); cached = null; }
    const r = cached ? cached.r : await ask(Object.assign({ action: 'mediaList', kind }, p));
    if (r && r.status === 'ok') lists.set(ck, { t: Date.now(), r });
    if (stale()) return;
    spin.remove();
    const formats = ((r && r.status === 'ok' && r.formats) || []).filter(f => f && !(f.url && shown.has(f.url)));
    // Order: yt-dlp video ("Best" first, then the other resolutions), the
    // page's own whole files, then audio. Audio menus: exact files first.
    const mk = (f) => fmtRow(f, () => pick(f, f.url ? { mime: isAudioFmt(f) ? 'audio/*' : 'video/*' }
                                                    : { probeUrl: r.probeUrl }));
    const first = directRows[0] || null;
    let audioRows = 0;
    for (const f of formats) {
      if (kind === 'video' && !isAudioFmt(f) && first) { menu.insertBefore(mk(f), first); continue; }
      const d = mk(f);
      // a faint line between the video entries and the audio ones
      if (kind === 'video' && isAudioFmt(f) && audioRows++ === 0 && menu.children.length) d.classList.add('a1');
      menu.appendChild(d);
    }
    if (!formats.length && !directRows.length)
      menu.appendChild(row(r && r.error === 'host_missing' ? 'NDM is not installed'
                         : r && (r.error === 'ndm_unreachable' || r.error === 'host_disconnected') ? 'NDM not reachable'
                         : r && r.error === 'timeout' ? 'Timed out — try again'
                         : kind === 'audio' ? 'No downloadable audio found' : 'No downloadable video found', '', 'd'));
    layoutMenu();
  }

  // ── Finding the element under the pointer ─────────────────────────────────

  /** elementsFromPoint, descending into open shadow roots (Reddit, web-component players). */
  function stackAt(x, y) {
    if (!document.elementsFromPoint) return [];
    const out = [];
    const walk = (list, depth) => {
      for (let i = 0; i < list.length && out.length < 40; i++) {
        const n = list[i];
        const sr = n && n.shadowRoot;
        if (sr && depth < 4 && sr.elementsFromPoint) {
          const inner = sr.elementsFromPoint(x, y).filter(e => e !== n && !out.includes(e));
          walk(inner, depth + 1);
        }
        if (!out.includes(n)) out.push(n);
      }
    };
    walk(document.elementsFromPoint(x, y), 0);
    return out;
  }

  function findVideo(n, depth) {
    if (!n || depth > 3) return null;
    const v = n.querySelector && n.querySelector('video');
    if (v) return v;
    if (n.shadowRoot) return findVideo(n.shadowRoot, depth + 1);
    return null;
  }

  // Audio context for custom players: music site, an <audio> on the page,
  // or audio the background saw this tab load (refreshed at most every 4 s).
  let audioSeen = false, audioAskedAt = 0;
  function audioContext() {
    if (AUDIO_SITE || audioSeen) return true;
    const now = Date.now();
    if (now - audioAskedAt > 4000) {
      audioAskedAt = now;
      ask({ action: 'mediaSniffed', src: '', kind: 'audio' }).then(r => { audioSeen = !!(r && (r.audioSeen || (r.items && r.items.length))); });
    }
    return !!(document.querySelector && document.querySelector('audio[src], audio source[src]'));
  }

  const PLAY_RE = /(^|[^a-z])(play|pause)([^a-z]|$)|play[-_]?(button|btn|control|pause)|btn[-_]?play|playbutton/i;
  function playControl(n) {
    for (let e = n, i = 0; e && i < 3; e = e.parentElement, i++) {
      if (!e.getAttribute) continue;
      const tag = e.tagName;
      const hint = [e.getAttribute('aria-label'), e.getAttribute('title'), e.getAttribute('class'),
                    e.getAttribute('data-testid'), e.getAttribute('data-test')].filter(Boolean).join(' ');
      if ((tag === 'BUTTON' || tag === 'A' || e.getAttribute('role') === 'button' || /play/i.test(hint)) && PLAY_RE.test(hint)) {
        const r = e.getBoundingClientRect();
        if (r.width >= 12 && r.height >= 12 && r.width <= 200 && r.height <= 200) return e;
      }
    }
    return null;
  }

  function candidateAt(x, y) {
    const els = stackAt(x, y);
    let photo = null, control = null;
    for (let i = 0; i < els.length && i < 20; i++) {
      const n = els[i];
      if (n === host) return { el: target, kind: targetKind };
      if (n.tagName === 'VIDEO') return usable(n, 'video') ? { el: n, kind: 'video' } : null;
      if (n.tagName === 'AUDIO' && usable(n, 'audio')) return { el: n, kind: 'audio' };
      if (!photo && !control && i < 4) {
        const c = playControl(n);
        if (c && audioContext() && usable(c, 'audio')) control = c;
      }
      if (!NO_PHOTOS && !photo && n.tagName === 'IMG' && usable(n, 'photo')) photo = { el: n, kind: 'photo' };
    }
    // Players whose <video> ignores the pointer (pointer-events:none under the controls).
    for (let i = 0; i < els.length && i < 6; i++) {
      const v = findVideo(els[i], 0);
      if (!v) continue;
      const r = v.getBoundingClientRect();
      if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom && usable(v, 'video')) return { el: v, kind: 'video' };
    }
    if (control) return { el: control, kind: 'audio' };
    if (photo) return photo;
    if (!NO_PHOTOS) {
      for (let i = 0; i < els.length && i < 6; i++) {
        const n = els[i];
        const r = n.getBoundingClientRect ? n.getBoundingClientRect() : null;
        if (!r || r.width < 250 || r.height < MIN_PHOTO || r.width > innerWidth * 0.98) continue;
        if (bgImage(n) && usable(n, 'photo')) return { el: n, kind: 'photo' };
      }
    }
    return null;
  }

  // At most one look-up per 120 ms, and always one for where the pointer came
  // to rest (a trailing call): a quick move onto a video still finds it.
  const MOVE_MS = 120;
  let lastMove = 0, trailTimer = null, trailX = 0, trailY = 0;
  function onPointer(e) {
    if (!overlayOn()) return;
    const now = Date.now();
    if (now - lastMove < MOVE_MS) {
      trailX = e.clientX; trailY = e.clientY;
      if (!trailTimer) trailTimer = setTimeout(() => {
        trailTimer = null;
        lastMove = Date.now();
        evalPointer(trailX, trailY);
      }, MOVE_MS - (now - lastMove));
      return;
    }
    clearTimeout(trailTimer);
    trailTimer = null;
    lastMove = now;
    evalPointer(e.clientX, e.clientY);
  }

  function evalPointer(x, y) {
    if (!overlayOn()) return;
    const e = { clientX: x, clientY: y };
    const c = candidateAt(e.clientX, e.clientY);
    if (c && c.el) { show(c.el, c.kind); return; }
    if (target && !menuOpen) {
      const r = rectOf(target);
      if (e.clientX >= r.left - 4 && e.clientX <= r.right + 120 && e.clientY >= r.top - 32 && e.clientY <= r.bottom + 4) {
        clearTimeout(hideTimer);
        return;
      }
      scheduleHide();
    }
  }

  document.addEventListener('mousemove', onPointer, { passive: true, capture: true });
  document.addEventListener('mouseover', onPointer, { passive: true, capture: true });
  // A video / audio that starts playing gets the button for a few seconds
  // (without hovering), as long as nothing else is shown.
  document.addEventListener('play', (e) => {
    const el = e.target;
    if (!overlayOn() || menuOpen || !el || (target && target !== el)) return;
    const kind = el.tagName === 'VIDEO' ? 'video' : el.tagName === 'AUDIO' ? 'audio' : '';
    if (!kind || !usable(el, kind)) return;
    show(el, kind);
    scheduleHide(3500);
  }, true);
  document.addEventListener('mousedown', (e) => {
    if (menuOpen && host && !(e.composedPath && e.composedPath().includes(host))) closeMenu();
  }, true);
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && menuOpen) closeMenu(); }, true);
  for (const ev of ['fullscreenchange', 'webkitfullscreenchange'])
    document.addEventListener(ev, () => { attach(); if (target) place(); });
  window.addEventListener('scroll', () => { if (target) place(); }, { passive: true, capture: true });
  window.addEventListener('resize', () => { if (target) place(); }, { passive: true });
  // SPA navigation (YouTube, Facebook, Instagram, X): the old list belongs to the old page.
  const reset = () => { hide(true); audioSeen = false; };
  window.addEventListener('yt-navigate-finish', reset);
  window.addEventListener('popstate', reset);
  // Feeds swap or remove videos without events: drop the button with its element.
  if (window.MutationObserver) {
    let pending = false;
    new MutationObserver(() => {
      if (pending || !target) return;
      pending = true;
      setTimeout(() => { pending = false; if (target && !target.isConnected) hide(true); }, 250);
    }).observe(document.documentElement, { childList: true, subtree: true });
  }
})();
