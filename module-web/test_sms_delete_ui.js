/* Exercise real delete click handlers; all API/storage data is local. */
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const codec = require('./static/sms-codec.js');
class Element {
  constructor() { this.children = []; this.listeners = {}; this.dataset = {}; this.textContent = ''; this.value = ''; this.classList = { toggle() {} }; }
  append(...nodes) { this.children.push(...nodes); }
  replaceChildren(...nodes) { this.children = nodes.flatMap(n => n.fragment ? n.children : [n]); }
  addEventListener(type, callback) { this.listeners[type] = callback; }
  setAttribute() {} reset() {} focus() {}
  get childNodes() { return this.children; }
}
const source = fs.readFileSync(__dirname + '/static/communications.js', 'utf8');
function setup(mode, pdus, stores = []) {
  const nodes = new Map(), deleteCalls = [], dialCalls = [], hangupCalls = [], audioActions = [];
  let audioActive = false;
  let records = pdus.map((pdu, i) => ({ storage: stores[i] ?? 0, index: i + 7, format: 6, pdu }));
  const doc = { hidden: false,
    getElementById(id) { if (!nodes.has(id)) nodes.set(id, new Element()); return nodes.get(id); },
    querySelectorAll() { return []; }, createElement() { return new Element(); },
    createDocumentFragment() { const node = new Element(); node.fragment = true; return node; } };
  const context = { document: doc, crypto: require('node:crypto').webcrypto, QDCSmsCodec: codec, confirm() { return true; },
    localStorage: { getItem() { return null; }, setItem() {} }, setInterval() { return 1; }, clearInterval() {} };
  context.window = context; vm.createContext(context); vm.runInContext(source, context);
  const ui = context.createQDCCommunications({ preview: false, toast() {}, audio: {
    async prepare() {}, calls(calls) { audioActive = calls.some(c => c.state === 3); },
    stop() { audioActions.push('stop');audioActive = false; },
  }, async api(name, options) {
    const body = options.body;
    if (name === 'sms-list') return { sim: 'test-sim', canMutate: true, next: null, messages: records.map(x => ({ ...x })), readErrors: 0, storageErrors: 0 };
    if (name === 'call-dial') { dialCalls.push({ ...body }); return { ok: true }; }
    if (name === 'call-end') {
      assert.equal(audioActive, false, 'Hangup must release audio before sending the command');
      assert.equal(audioActions.at(-1), 'stop');hangupCalls.push({ ...body });
      if (mode === 'hangup-error') throw new Error('挂断结果未确认');
      return { ok: true };
    }
    if (name === 'call-history-list') return { records: [{id:'a'.repeat(32),number:'13800138000',direction:2,started:1700000000000,ended:1700000010000,status:1,duration:10,answered:1700000000000}], next:null };
    if (name === 'call-list') return { ok: true, calls: [] };
    assert.equal(name, 'sms-delete');
    deleteCalls.push({ ...body });
    const slot = records.find(x => String(x.storage) === body.storage && String(x.index) === body.index);
    assert(slot && slot.pdu === body.pdu && body.sim === 'test-sim');
    if (mode === 'error' || mode === 'partial' && deleteCalls.length === 2) throw new Error('模块拒绝删除');
    if (mode === 'no-proof') return { ok: true };
    if (mode !== 'still-present') records = records.filter(x => x !== slot);
    if (mode === 'logout') ui.auth(false);
    return { ok: true, deleted: true, verified: true };
  } });
  function all(node) { return [node, ...node.children.flatMap(all)]; }
  async function idle() {
    for (let i = 0; i < 100; i++) { await new Promise(resolve => setImmediate(resolve)); if (!nodes.get('sms-refresh').disabled) return; }
    throw new Error('Delete operation did not finish');
  }
  return { nodes, deleteCalls, dialCalls, hangupCalls, audioActions, ui, all, async idle() { await idle(); }, click(id) { nodes.get(id).listeners.click(); }, open() { all(nodes.get('sms-list')).find(x => x.textContent === '查看').listeners.click(); }, async read() { ui.auth(true); ui.page('sms'); await idle(); },
    async remove() { const button = [...all(nodes.get('sms-detail-content')), ...all(nodes.get('sms-list'))].find(x => x.textContent === '删除'); assert(button); button.listeners.click(); await idle(); } };
}
(async () => {
  const short = codec.encode('13800138000', '测试', 1)[0], other = codec.encode('13800138000', '其他', 2)[0];
  let test = setup('success', [short, other]); await test.read();
  assert.equal(test.all(test.nodes.get('sms-list')).filter(x => x.className === 'sms-storage-card' && x.textContent === '卡').length, 2);
  test.open(); assert(test.all(test.nodes.get('sms-detail-content')).some(x => x.className === 'sms-storage-card' && x.textContent === '卡'));
  await test.remove();
  assert.equal(test.deleteCalls.length, 1); assert.equal(test.nodes.get('sms-list').children.length, 1);
  assert.match(test.nodes.get('sms-message').textContent, /已从模块删除并复查/);
  test = setup('success', [short, other]); await test.read(); test.open();
  assert.equal(test.nodes.get('sms-panel-detail').hidden, false);
  assert.equal(test.nodes.get('sms-panel-list').hidden, true);
  assert(test.all(test.nodes.get('sms-detail-content')).some(x => x.textContent === '测试'));
  test.ui.events({ callsReady: false }, true); await test.idle();
  assert.equal(test.nodes.get('sms-panel-detail').hidden, false, 'SMS event refresh keeps the selected message open');
  test.all(test.nodes.get('sms-detail-content')).find(x => x.textContent === '回复').listeners.click();
  assert.equal(test.nodes.get('sms-panel-compose').hidden, false);
  assert.equal(test.nodes.get('sms-number').value, '13800138000');
  test.nodes.get('sms-body').value = '未发送的草稿';
  test.click('sms-tab-list'); assert.equal(test.nodes.get('sms-list').children.length, 2);
  test.click('sms-tab-compose'); assert.equal(test.nodes.get('sms-body').value, '未发送的草稿', 'Switching tabs preserves draft');
  test.click('sms-tab-list'); test.open(); test.click('sms-back');
  assert.equal(test.nodes.get('sms-panel-list').hidden, false); assert.equal(test.nodes.get('sms-list').children.length, 2);
  test.open(); await test.remove();
  assert.equal(test.nodes.get('sms-panel-detail').hidden, true, 'Deleting the selected message returns to the inbox');
  assert.equal(test.nodes.get('sms-list').children.length, 1);
  test.click('phone-tab-history'); await test.idle();
  test.ui.events({ callsReady: true, callState: { calls: [{ id: 7, number: '13800138000', direction: 2, state: 2, stateText: '来电' }] } }, false);
  assert.equal(test.nodes.get('phone-panel-history').hidden, false);
  assert.equal(test.nodes.get('call-live').hidden, false, 'Incoming call controls remain visible on the history tab');
  test.all(test.nodes.get('call-history')).find(x => x.textContent === '回拨').listeners.click();
  assert.equal(test.dialCalls.length, 0, 'Callback cannot interrupt an active call');
  test.ui.events({ callsReady: true, callState: { calls: [] } }, false);
  test.all(test.nodes.get('call-history')).find(x => x.textContent === '回拨').listeners.click(); await test.idle();
  assert.equal(test.dialCalls.length, 1); assert.equal(test.dialCalls[0].number, '13800138000');
  assert.match(test.dialCalls[0].requestId, /^[a-f0-9]{32}$/);
  assert.equal(test.nodes.get('phone-panel-dial').hidden, false); assert.equal(test.nodes.get('call-number').value, '13800138000');
  test.ui.auth(false); assert.equal(test.nodes.get('sms-detail-content').children.length, 0);
  for (const mode of ['success', 'hangup-error']) {
    test = setup(mode, []);await test.read();
    test.ui.events({ callsReady: true, callState: { calls: [{ id: 4, state: 3, number: '13800138000', direction: 1 }] } }, false);
    const hangup = test.all(test.nodes.get('call-list')).find(n => n.textContent === '挂断');
    hangup.listeners.click();hangup.listeners.click();await test.idle();
    assert.equal(test.hangupCalls.length, 1, 'Repeated click does not send a duplicate hangup');
    assert.equal(test.hangupCalls[0].callId, '4');
    if (mode === 'success') { assert.equal(test.nodes.get('call-live').hidden, true);assert.match(test.nodes.get('call-message').textContent, /已请求挂断/); }
    else { assert.equal(test.nodes.get('call-live').hidden, false);assert.match(test.nodes.get('call-message').textContent, /挂断结果未确认/); }
  }
  test = setup('success', [short], [1]); await test.read();
  assert(!test.all(test.nodes.get('sms-list')).some(x => x.className === 'sms-storage-card'), 'Module storage must not be labeled as SIM');
  test = setup('success', codec.encode('13800138000', '你'.repeat(80), 3), [1,0]); await test.read();
  const badges = test.all(test.nodes.get('sms-list')).filter(x => x.className === 'sms-storage-card');
  assert.equal(badges.length, 1, 'Merged SMS with a SIM segment must show one card badge');
  assert.match(badges[0].title, /分段或副本/);
  for (const mode of ['error', 'no-proof', 'still-present']) {
    test = setup(mode, [short, other]); await test.read(); await test.remove();
    assert.equal(test.deleteCalls.length, 1, 'Do not automatically repeat a delete');
    assert.equal(test.nodes.get('sms-list').children.length, 2, 'Unconfirmed SMS must remain visible');
    assert(!test.nodes.get('sms-message').textContent.includes('已从模块删除并复查'));
  }
  test = setup('partial', codec.encode('13800138000', '你'.repeat(80), 3)); await test.read(); await test.remove();
  assert.equal(test.deleteCalls.length, 2); assert.match(test.nodes.get('sms-message').textContent, /已确认删除 1 个存储项/);
  assert.equal(test.nodes.get('sms-list').children.length, 1);
  test = setup('logout', [short]); await test.read(); await test.remove();
  assert.equal(test.nodes.get('sms-list').children.length, 0, 'A stale response must not repopulate a logged-out page');
  console.log('Communication UI checks passed: tabs, inline SMS details, draft, event refresh, incoming call controls, direct callback and active-call protection; verified success, rejected/unverified deletes stay visible, no auto retry, partial long SMS, logout race.');
})().catch(e => { console.error(e); process.exitCode = 1; });
