/* Run the actual page refresh logic with a small DOM/fetch harness.
 * Verify fresh results replace old SIM values, including failures. */
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
class Element {
  constructor() { this.classList = { toggle() {}, add() {}, remove() {} }; this.style = {}; this.textContent = ''; }
  get firstElementChild() { return this.child || (this.child = new Element()); }
  get lastElementChild() { return this.firstElementChild; }
  addEventListener() {} setAttribute() {} removeAttribute() {} append() {} replaceChildren() {} reset() {}
  querySelectorAll() { return []; }
}
const nodes = new Map();
const doc = { body: { dataset: {} }, hidden: false, getElementById(id) { if (!nodes.has(id)) nodes.set(id, new Element()); return nodes.get(id); },
  querySelectorAll() { return []; }, addEventListener() {}, createElement() { return new Element(); }, createDocumentFragment() { return new Element(); } };
let radioMode = 'current';
const base = { version: '0.2.1', storage: [], network: { mobileAddress: '10.203.20.206',
  operator: null, networkType: null, simState: null, apn: null, signalDbm: null } };
const context = { document: doc, location: { hostname: '192.168.225.1', search: '', hash: '#network', pathname: '/' },
  history: { replaceState() {} }, URLSearchParams, AbortController, TextEncoder, Date, console,
  setInterval() { return 1; }, clearInterval() {}, setTimeout, clearTimeout,
  addEventListener() {}, createQDCCommunications() { return { auth() {}, page() {}, refresh() {} }; },
  async fetch(path) {
    if (path.endsWith('/session')) return { ok: true, async json() { return { authenticated: true, csrfToken: 'test' }; } };
    if (path.endsWith('/status')) return { ok: true, async json() { return JSON.parse(JSON.stringify(base)); } };
    if (path.endsWith('/traffic')) return { ok: true, async json() { return { receivedBytes: 100, sentBytes: 50, since: 100, interfaces: 1 }; } };
    assert(path.endsWith('/radio'));
    if (radioMode === 'busy') throw new Error('通讯忙');
    const net = { operator: '中国移动', networkType: 'LTE', simState: '已就绪', signalDbm: -94,
      apn: radioMode === 'configured' ? 'cmhk' : 'cmnet', apnSource: radioMode === 'configured' ? 'configured' : 'current',
      apnAddress: radioMode === 'mismatch' ? '10.1.2.3' : null, registrationState: 1 };
    return { ok: true, async json() { return { ok: true, stateSource: 'qmi', network: net }; } };
  } };
context.window = context;
vm.createContext(context);
const source = fs.readFileSync(__dirname + '/static/app.js', 'utf8');
assert(source.includes('  start();'));
vm.runInContext(source.replace('  start();', '  globalThis.ready = start(); globalThis.refreshPage = refresh;'), context);
(async () => {
  await context.ready;
  assert.equal(nodes.get('net-operator').textContent, '中国移动');
  assert.equal(nodes.get('net-signal').textContent, '-94 dBm');
  assert.equal(nodes.get('net-apn').textContent, 'cmnet');
  radioMode = 'busy'; await context.refreshPage();
  for (const id of ['net-operator', 'net-type', 'net-signal', 'net-sim', 'net-apn']) assert.equal(nodes.get(id).textContent, '未读取');
  assert.match(nodes.get('radio-note').textContent, /通讯忙/);
  radioMode = 'configured'; await context.refreshPage();
  assert.equal(nodes.get('net-apn').textContent, 'cmhk');
  assert.equal(nodes.get('net-apn-label').textContent, 'APN（默认配置）尚未确认是当前连接使用的值。');
  assert.equal(nodes.get('radio-note').textContent, '');
  radioMode = 'mismatch'; await context.refreshPage();
  assert.equal(nodes.get('net-apn').textContent, '未读取');
  assert.match(nodes.get('radio-note').textContent, /另一条数据连接/);
  console.log('Radio page checks passed: fresh data, busy failure clears old SIM values, default APN label, mismatched context rejected.');
})().catch(e => { console.error(e); process.exitCode = 1; });
