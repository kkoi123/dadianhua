/* Actual page read-card action: no automatic reads, no stale data or HTML injection. */
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
class E {
  constructor() { this.handlers = {}; this.children = []; this.style = {}; this.textContent = ''; this.classList = { toggle() {}, add() {}, remove() {} }; }
  addEventListener(k, f) { this.handlers[k] = f; } setAttribute() {} removeAttribute() {} reset() {}
  append(...items) { this.children.push(...items); } replaceChildren(...items) { this.children = [...items]; }
  querySelectorAll() { return this.children.flatMap(c => [c, ...c.querySelectorAll()]); }
  get firstElementChild() { return this.children[0] || (this.child ||= new E()); }
  get lastElementChild() { return this.firstElementChild; }
  set innerHTML(_) { throw Error('Card data must not use HTML'); }
}
const nodes = new Map(), document = { body: { dataset: {} }, getElementById(id) { if (!nodes.has(id)) nodes.set(id, new E()); return nodes.get(id); },
  querySelectorAll() { return []; }, addEventListener() {}, createElement() { return new E(); }, createDocumentFragment() { return new E(); } };
const ready = { ok: true, state: 'ready', cards: [{ eid: '89000000000000000000000000000001', firmware: '1.2.3', freeBytes: 65536,
  profiles: [{ iccid: '89860000000000000001', name: '<script>卡片</script>', provider: '测试运营商', enabled: true }] }] };
let result = ready, fail = false, pending = null, active = false; const requests = [];
const context = { document, location: { hostname: '192.168.225.1', search: '', hash: '#apps', pathname: '/' }, URLSearchParams, AbortController, TextEncoder, TypeError, Date, console,
  history: { replaceState() {} }, setTimeout, clearTimeout, clearInterval() {}, addEventListener() {},
  createQDCCommunications() { return { auth() {}, page() {}, refresh() {}, activeCall() { return active; } }; },
  async fetch(path, options) { requests.push({ path, options }); if (pending) await pending;
    if (fail) throw TypeError('offline'); return { ok: true, json: async () => structuredClone(result) }; } };
context.window = context; vm.createContext(context);
vm.runInContext(fs.readFileSync(__dirname + '/static/app.js', 'utf8').replace('  start();', '  globalThis.esimTest = { state, resetEsim };'), context);
function allText(e) { return [e.textContent, ...e.children.map(allText)].join(' '); }
(async () => {
  assert.equal(requests.length, 0, 'page setup must not read cards');
  const action = nodes.get('esim-read').handlers.click; await action(); assert.equal(requests.length, 0, 'no unauthenticated read');
  context.esimTest.state.authenticated = true; context.esimTest.state.csrf = 'fixture'; context.esimTest.resetEsim();
  await action(); assert.equal(requests.length, 1); assert(requests[0].path.endsWith('/esim-list'));
  assert.equal(requests[0].options.method, 'POST'); assert.equal(JSON.parse(requests[0].options.body).csrfToken, 'fixture');
  assert.equal(nodes.get('esim-content').hidden, false); assert.match(allText(nodes.get('esim-content')), /<script>卡片<\/script>/); assert.match(allText(nodes.get('esim-content')), /已启用/);
  fail = true; await action(); assert(nodes.get('esim-content').hidden); assert.equal(nodes.get('esim-content').children.length, 0); assert.match(nodes.get('esim-message').textContent, /无法连接/);
  fail = false; result = { ok: true, state: 'unsupported', cards: [], message: '未发现受支持的卡片' }; await action(); assert(nodes.get('esim-content').hidden); assert(!/读卡完成/.test(nodes.get('esim-message').textContent));
  active = true; const before = requests.length; await action(); assert.equal(requests.length, before); active = false;
  result = ready; let resolve; pending = new Promise(r => { resolve = r; }); const p = action();
  await action(); assert.equal(requests.length, before + 1, 'no duplicate card read');
  context.esimTest.state.authenticated = false; context.esimTest.resetEsim(); resolve(); await p; pending = null;
  assert(nodes.get('esim-content').hidden); assert.equal(nodes.get('esim-content').children.length, 0); assert(nodes.get('esim-read').disabled, 'late response after logout must stay hidden');
  console.log('eSIM UI passed: explicit reads, CSRF, escaped labels, call guard, failed/unsupported states, duplicate suppression and logout race.');
})().catch(e => { console.error(e); process.exitCode = 1; });
