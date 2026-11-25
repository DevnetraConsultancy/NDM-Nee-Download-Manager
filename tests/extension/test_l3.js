// L3 tests: node test_l3.js <background.js> [firefox]
'use strict';
const fs = require('fs'), vm = require('vm'), assert = require('assert');
const file = process.argv[2], firefox = process.argv[3] === 'firefox';
// universal stub: any property is a callable stub returning a resolved promise
function stub() {
  const f = function () { return Promise.resolve(undefined); };
  return new Proxy(f, { get: (t, k) => (k === 'then' ? undefined : (t[k] || (t[k] = stub()))), apply: () => Promise.resolve(undefined) });
}
const jar = [
  { name: 'strict', value: '1', sameSite: 'strict', domain: 'files.example.com', hostOnly: true, path: '/' },
  { name: 'lax', value: '2', sameSite: 'lax', domain: 'files.example.com', hostOnly: true, path: '/' },
  { name: 'unspec', value: '3', sameSite: 'unspecified', domain: 'files.example.com', hostOnly: true, path: '/' },
  { name: 'none', value: '4', sameSite: 'no_restriction', secure: true, domain: 'files.example.com', hostOnly: true, path: '/' },
];
const api = stub();
api.cookies = { getAll: async () => jar.map(c => Object.assign({}, c)) };
api.runtime = stub();
api.runtime.getURL = p => 'chrome-extension://x/' + p;
if (firefox) api.runtime.getBrowserInfo = async () => ({});
const ctx = { console, setTimeout, clearTimeout, setInterval, clearInterval, URL, navigator: { userAgent: 'UA' }, chrome: api };
if (firefox) ctx.browser = api;
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(file, 'utf8'), ctx);

const site = h => vm.runInContext('siteOfHost(' + JSON.stringify(h) + ')', ctx);
const cases = [
  ['www.bbc.co.uk', 'bbc.co.uk'], ['a.b.example.com', 'example.com'], ['alice.github.io', 'alice.github.io'],
  ['bob.github.io', 'bob.github.io'], ['x.y.blogspot.com', 'y.blogspot.com'], ['app.herokuapp.com', 'app.herokuapp.com'],
  ['my.vercel.app', 'my.vercel.app'], ['site.netlify.app', 'site.netlify.app'], ['p.pages.dev', 'p.pages.dev'],
  ['shop.example.com.au', 'example.com.au'], ['x.example.com.np', 'example.com.np'], ['github.io', 'github.io'],
  ['127.0.0.1', '127.0.0.1'], ['example.de', 'example.de'], ['cdn.example.de', 'example.de'],
];
for (const [h, want] of cases) { assert.strictEqual(site(h), want, h); }
const same = (a, b) => vm.runInContext(`sameSite(${JSON.stringify(a)}, ${JSON.stringify(b)})`, ctx);
assert.strictEqual(same('https://alice.github.io/x', 'https://bob.github.io/y'), false, 'github.io tenants differ');
assert.strictEqual(same('https://a.example.com/x', 'https://b.example.com/y'), true, 'same registrable domain');
console.log('site/public-suffix: ' + cases.length + ' cases ok');

(async () => {
  const ck = (url, init) => vm.runInContext(`cookiesFor(${JSON.stringify(url)}, undefined, ${JSON.stringify(init)})`, ctx);
  const cl = (url, init) => vm.runInContext(`cookieListFor(${JSON.stringify(url)}, undefined, ${JSON.stringify(init)})`, ctx);
  const target = 'https://files.example.com/a.zip';
  assert.strictEqual(await ck(target, 'https://www.example.com/page'), 'strict=1; lax=2; unspec=3; none=4', 'same-site initiator: all');
  assert.strictEqual(await ck(target, ''), 'strict=1; lax=2; unspec=3; none=4', 'no initiator: all');
  const cross = await ck(target, 'https://evil.test/page');
  assert.strictEqual(cross, 'lax=2; unspec=3; none=4', 'cross-site initiator drops Strict only (top-level navigation)');
  assert.strictEqual(await ck(target, 'http://www.example.com/page'), 'lax=2; unspec=3; none=4', 'schemeful: http page is cross-site');
  const list = await cl(target, 'https://alice.github.io/');
  assert.deepStrictEqual(list.map(c => c.name), ['lax', 'unspec', 'none'], 'structured list filtered too');
  assert.strictEqual(vm.runInContext("isNetUrl('ftp://a.test/x')", ctx), false, 'ftp rejected');
  console.log('L3 cookie SameSite filter ok (' + (firefox ? 'firefox' : 'chrome') + ' semantics)');
})().catch(e => { console.error('FAIL', e); process.exit(1); });
