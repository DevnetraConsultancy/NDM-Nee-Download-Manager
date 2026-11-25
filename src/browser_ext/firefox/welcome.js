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

async function check() {
  $('status').className = 'status wait';
  $('statusText').textContent = 'Checking connection';
  const s = await bg({ action: 'status' });
  const help = $('help');
  help.className = '';
  if (s && s.host) {
    $('status').className = 'status ok';
    $('statusText').textContent = s.running ? 'Connected to NDM ' + (s.version || '') : 'Connected (NDM starts on demand)';
  } else if (s && s.error === 'host_missing') {
    $('status').className = 'status bad';
    $('statusText').textContent = 'NDM integration not found';
    help.textContent = 'Install or repair Nee Download Manager, then restart the browser.';
    help.className = 'show';
  } else {
    $('status').className = 'status bad';
    $('statusText').textContent = 'Cannot reach NDM';
    help.textContent = (s && s.detail) || 'The NDM native host could not be started.';
    help.className = 'show';
  }
}

$('retry').addEventListener('click', check);
check();
