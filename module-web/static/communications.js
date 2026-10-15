(() => {
  'use strict';
  const $ = id => document.getElementById(id), codec = window.QDCSmsCodec;
  window.createQDCCommunications = ({ api, toast, preview, audio = null }) => {
    let authenticated = false, page = '', busy = false, sim = '', canMutate = false;
    let next = null, messages = [], activeCalls = [], haveCallState = false, generation = 0;
    let history = [], historyNext = null, historyPending = false, legacyHistory = [], smsPending = false, phonePanel = 'dial', smsPanel = 'list', selectedSMS = null;
    let outbox = [], outboxNext = null, outboxLoaded = false, selectedOutbox = null, detailOrigin = 'list';
    try { const stored = JSON.parse(localStorage.getItem('qdc-module-call-history') || '[]'); if (Array.isArray(stored)) legacyHistory = stored.filter(x => x && typeof x.number === 'string' && /^\+?\d{0,80}$/.test(x.number) && Number.isSafeInteger(x.started) && x.started > 0 && (x.direction === 1 || x.direction === 2)).slice(0, 100); } catch (_) {}
    const status = (id, value, error = false) => { const node = $(id); node.textContent = value || ''; node.hidden = !value; node.classList.toggle('error', error); };
    function phoneView(value) {
      phonePanel = value;
      for (const name of ['dial', 'history']) {
        const active = name === value, tab = $(`phone-tab-${name}`);
        tab.setAttribute('aria-selected', String(active)); tab.tabIndex = active ? 0 : -1;
        $(`phone-panel-${name}`).hidden = !active;
      }
    }
    function smsView(value) {
      smsPanel = value;
      for (const name of ['list', 'outbox', 'compose']) {
        const active = name === (value === 'detail' ? detailOrigin : value), tab = $(`sms-tab-${name}`);
        tab.setAttribute('aria-selected', String(active)); tab.tabIndex = active ? 0 : -1;
        $(`sms-panel-${name}`).hidden = name !== value;
      }
      $('sms-panel-detail').hidden = value !== 'detail';
    }
    function wireTabs(prefix, names, change) {
      names.forEach((name, index) => {
        const tab = $(`${prefix}-tab-${name}`);
        tab.addEventListener('click', () => change(name));
        tab.addEventListener('keydown', event => {
          if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
          event.preventDefault();
          const target = event.key === 'Home' ? 0 : event.key === 'End' ? names.length - 1 : (index + (event.key === 'ArrowRight' ? 1 : -1) + names.length) % names.length;
          change(names[target]); $(`${prefix}-tab-${names[target]}`).focus();
        });
      });
    }
    wireTabs('phone', ['dial', 'history'], value => { phoneView(value); if(value === 'history')fetchHistory(); });
    wireTabs('sms', ['list', 'outbox', 'compose'], value => {
      selectedSMS = null; selectedOutbox = null; renderMessages(); renderOutbox(); smsView(value);
      if (value === 'outbox' && !outboxLoaded) fetchOutbox();
    });
    $('sms-back').addEventListener('click', () => { selectedSMS = null; selectedOutbox = null; renderMessages(); renderOutbox(); smsView(detailOrigin); });
    const request = (name, body = {}) => api(name, { method: 'POST', body, timeout: 40000 });
    function identifier() { return Array.from(crypto.getRandomValues(new Uint8Array(16)), n => n.toString(16).padStart(2, '0')).join(''); }
    function controls() {
      $('sms-send').disabled = busy || !authenticated || !canMutate || preview;
      $('sms-refresh').disabled = busy; $('sms-more').disabled = busy;
      $('sms-outbox-refresh').disabled = busy; $('sms-outbox-more').disabled = busy;
      $('call-dial-button').disabled = busy || !authenticated || !haveCallState || activeCalls.some(c => c.state !== 9) || preview;
      $('call-refresh').disabled = busy; $('call-clear').disabled = busy || !authenticated || preview; $('call-history-more').disabled = busy;
      document.querySelectorAll('[data-comm-action]').forEach(n => { n.disabled = busy || preview; });
      document.querySelectorAll('[data-call-back]').forEach(n => {
        n.disabled = busy || preview || !authenticated || !haveCallState || activeCalls.some(c => c.state !== 9);
      });
    }
    async function operation(work, id) {
      if (busy || !authenticated) return;
      busy = true; controls(); const current = generation;
      try { await work(() => authenticated && current === generation); }
      catch (error) { if (authenticated && current === generation) status(id, error.message, true); }
      finally { busy = false; controls(); if(historyPending && authenticated && page === 'phone' && phonePanel === 'history'){historyPending = false; Promise.resolve().then(() => fetchHistory());} if (smsPending && authenticated && page === 'sms') { smsPending = false; Promise.resolve().then(() => fetchMessages()); } }
    }
    function button(label, action, danger = false) {
      const node = document.createElement('button'); node.type = 'button'; node.className = danger ? 'secondary danger' : 'secondary'; node.textContent = label; node.dataset.commAction = '1'; node.addEventListener('click', action); return node;
    }
    function card(title, subtitle, body) {
      const item = document.createElement('article'); item.className = 'comm-item';
      const head = document.createElement('div'); head.className = 'comm-item-head';
      const text = document.createElement('div'), strong = document.createElement('strong'), caption = document.createElement('p');
      strong.textContent = title; caption.className = 'muted'; caption.textContent = subtitle; text.append(strong, caption);head.append(text);item.append(head);
      if (body !== undefined) { const content = document.createElement('p'); content.className = 'sms-content'; content.textContent = body; item.append(content); }
      const actions = document.createElement('div'); actions.className = 'comm-actions'; item.append(actions); return { item, actions, head };
    }
    const date = value => value ? new Date(value).toLocaleString('zh-CN', { hour12: false }) : '时间未读取';
    async function snapshot(expectedSim, valid) {
      const items = [], cursors = new Set(); let cursor = 0, result;
      do {
        if (!valid()) return null;
        if (cursors.has(cursor) || cursors.size >= 128) throw new Error('短信分页异常，请刷新后再试。');
        cursors.add(cursor);
        result = await request('sms-list', { cursor: String(cursor) });
        if (!valid()) return null;
        if (!expectedSim || result.sim !== expectedSim || result.canMutate !== true) throw new Error('SIM 身份已变化或无法确认，请重新刷新短信列表。');
        if (result.readErrors || result.storageErrors) throw new Error('部分短信存储无法读取，暂不能确认所有副本，请刷新后再试。');
        if (!Array.isArray(result.messages)) throw new Error('短信列表数据不完整。');
        for (const item of result.messages) if (!items.some(x => x.storage === item.storage && x.index === item.index)) items.push(item);
        cursor = result.next;
        if (cursor !== null && (!Number.isInteger(cursor) || cursor < 0)) throw new Error('短信分页参数无效。');
      } while (cursor !== null);
      return items;
    }
    function renderMessages() {
      const decoded = messages.map(raw => {
        try { if (raw.format !== 6) throw new Error('此短信格式暂不支持'); return { ...codec.decode(raw.pdu), slots: [raw] }; }
        catch (error) { return { number: '无法解析的短信', body: error.message, slots: [raw], timestamp: null }; }
      });
      const grouped = [], segments = new Map();
      for (const item of decoded) {
        if (!item.concat) { grouped.push(item); continue; }
        const { ref, total, seq } = item.concat;
        const key = `${item.number}:${item.outgoing}:${ref}:${total}`;
        let buckets = segments.get(key) || [];
        let group = buckets.find(g => Math.abs((item.timestamp || 0) - (g.timestamp || 0)) < 300000 && (!g.parts.has(seq) || g.parts.get(seq) === item.body));
        if (!group) { group = { ...item, parts: new Map(), slots: [] }; buckets.push(group); segments.set(key, buckets); grouped.push(group); }
        group.parts.set(seq, item.body); group.slots.push(...item.slots);
      }
      grouped.sort((a, b) => (b.timestamp || 0) - (a.timestamp || 0));
      const fragment = document.createDocumentFragment(); let selectedView = null;
      for (const item of grouped) {
        let body = item.body, partNote = '';
        if (item.parts) { body = ''; for (let i = 1; i <= item.concat.total; i++) body += item.parts.get(i) ?? '[缺失分段]'; if (item.parts.size < item.concat.total) partNote = ` · 分段 ${item.parts.size}/${item.concat.total}`; }
        const key = item.slots.map(slot => `${slot.storage}:${slot.index}:${slot.pdu}`).join('|');
        const view = card(item.number, `${item.outgoing ? '发出' : '收到'} · ${date(item.timestamp)}${partNote}`, body);
        if (item.slots.some(slot => slot.storage === 0)) {
          const badge = document.createElement('span'); badge.className = 'sms-storage-card'; badge.textContent = '卡';
          badge.title = item.slots.every(slot => slot.storage === 0) ? '存储在 SIM 卡中' : '包含存储在 SIM 卡中的分段或副本';
          badge.setAttribute('aria-label', badge.title); view.head.children[0].children[0].append(badge);
        }
        view.item.className += ' sms-preview';
        const open = document.createElement('button'); open.type = 'button'; open.className = 'sms-open'; open.textContent = '查看'; open.setAttribute('aria-label', `查看 ${item.number} 的短信`);
        open.addEventListener('click', () => { detailOrigin = 'list'; selectedOutbox = null; selectedSMS = key; $('sms-back').textContent = '‹ 收件箱'; renderMessages(); smsView('detail'); $('sms-back').focus(); });
        view.actions.append(open);
        if (/^\+?\d+$/.test(item.number)) view.actions.append(button('回复', () => { $('sms-number').value = item.number; selectedSMS = null; smsView('compose'); $('sms-body').focus(); }));
        if (canMutate) view.actions.append(button('删除', () => {
          const partText = item.parts && item.parts.size < item.concat.total ? '当前已读取的分段' : item.slots.length > 1 ? '对应的所有分段' : '短信';
          if (!confirm(`删除 ${item.number} 的这条短信？此操作会从模块存储中删除${partText}。`)) return;
          const expectedSim = sim, slots = item.slots.slice();
          operation(async valid => {
            status('sms-message', '正在删除并复查模块短信…');
            let removed = 0;
            for (const slot of slots) {
              if (!valid()) return;
              try {
                const result = await request('sms-delete', { storage: String(slot.storage), index: String(slot.index), pdu: slot.pdu, sim: expectedSim });
                if (result.deleted !== true || result.verified !== true) throw new Error('模块未确认实际删除，请刷新列表。');
                if (!valid()) return;
                removed++; messages = messages.filter(x => x.storage !== slot.storage || x.index !== slot.index);
              } catch (error) {
                /* Only read after an error. Do not repeat a delete with an
                   uncertain result, or remove an unverified row locally. */
                if (!valid()) return;
                let detail = '';
                try { const latest = await snapshot(expectedSim, valid); if (latest) { messages = latest; next = null; } }
                catch (refreshError) { detail = ` 列表复查失败：${refreshError.message}`; }
                if (!valid()) return;
                renderMessages(); throw new Error(`${removed ? `已确认删除 ${removed} 个存储项；` : ''}${error.message}${detail}`);
              }
            }
            if (!valid()) return;
            const latest = await snapshot(expectedSim, valid);
            if (!latest) return;
            messages = latest; next = null; renderMessages();
            if (messages.some(slot => slots.some(target => target.storage === slot.storage && target.index === slot.index && target.pdu === slot.pdu))) throw new Error('模块仍保留这条短信，未确认完全删除。');
            status('sms-message', '短信已从模块删除并复查。');
          }, 'sms-message');
        }, true));
        if (selectedSMS === key) {
          selectedView = view.item; selectedView.className = 'comm-item sms-full'; open.hidden = true;
        } else fragment.append(view.item);
      }
      if (detailOrigin === 'list') {
        $('sms-detail-content').replaceChildren(...(selectedView ? [selectedView] : []));
        if (selectedSMS && !selectedView) { selectedSMS = null; if (smsPanel === 'detail') smsView('list'); }
      }
      if (!grouped.length) { const empty = document.createElement('p'); empty.className = 'empty'; empty.textContent = '当前已读取的短信列表为空'; fragment.append(empty); }
      $('sms-list').replaceChildren(fragment);$('sms-count').textContent = `共 ${grouped.length} 条短信${next !== null ? ' · 还有更多' : ''}${canMutate || preview ? '' : ' · 仅可查看'}`;
      $('sms-more').hidden = next === null; controls();
    }
    async function fetchMessages(more = false) {
      await operation(async valid => {
        status('sms-message', '正在读取模块短信…');
        if (preview) {
          messages = ['您的快递已送达，请及时领取。', '这是一条演示短信。点开查看完整内容，也可以切换到新建短信页面。'].map((body, index) => ({ storage: 0, index, format: 6, pdu: codec.encode('13800138000', body, index)[0] }));
          renderMessages(); status('sms-message', '预览内容，不读取或发送真实短信。'); return;
        }
        if (!more) { next = null; canMutate = false; controls(); }
        const result = await request('sms-list', { cursor: String(more ? next || 0 : 0) });
        if (!valid()) return;
        if (more && sim && result.sim !== sim) { messages = []; next = null; canMutate = false; sim = ''; renderMessages(); throw new Error('SIM 卡已变化，请重新刷新短信列表。'); }
        sim = typeof result.sim === 'string' ? result.sim : ''; canMutate = result.canMutate === true;
        if (!more) messages = [];
        const items = Array.isArray(result.messages) ? result.messages : [];
        for (const item of items) { if (!messages.some(x => x.storage === item.storage && x.index === item.index)) messages.push(item); }
        next = Number.isInteger(result.next) && result.next >= 0 ? result.next : null;
        renderMessages();
        status('sms-message', result.readErrors || result.storageErrors ? '部分短信或存储暂未读到，可以刷新重试。' : next !== null ? `模块有 ${result.total} 个存储项，可加载更多。` : '短信已更新。', !!(result.readErrors || result.storageErrors));
      }, 'sms-message');
    }
    function sentContent(record) {
      try {
        const parts = record.pdus.split(',').map(pdu => codec.decode(pdu));
        if (!parts.length || parts.some(p => !p.outgoing || p.number !== parts[0].number)) throw new Error('发送记录的号码不一致');
        if (parts[0].concat) {
          const header = parts[0].concat, seen = new Set();
          if (parts.length !== header.total) throw new Error('发送记录的分段不完整');
          for (const part of parts) {
            if (!part.concat || part.concat.ref !== header.ref || part.concat.total !== header.total || seen.has(part.concat.seq)) throw new Error('发送记录的分段不完整');
            seen.add(part.concat.seq);
          }
          parts.sort((a,b) => a.concat.seq-b.concat.seq);
        } else if (parts.length !== 1) throw new Error('发送记录的分段无效');
        return {number: parts[0].number, body: parts.map(p => p.body).join(''), editable: true};
      } catch (error) { return {number: '无法解析的发送记录', body: error.message, editable: false}; }
    }
    function renderOutbox() {
      const fragment = document.createDocumentFragment(); let selectedView = null;
      for (const record of outbox) {
        const content = sentContent(record), states = { submitted: '已提交', partial: '部分发送', failed: '发送失败', uncertain: '结果未确认' };
        const state = states[record.status] || states.uncertain;
        const view = card(content.number, `${date(record.timestamp * 1000)}${record.timeExact ? '' : ' · 旧记录时间，仅供参考'}${record.currentSIM === false ? ' · 其他 SIM 卡' : ''}`, content.body);
        view.item.className += ' sms-preview';
        const badge = document.createElement('span'); badge.className = `sms-send-state ${record.status}`;
        badge.textContent = `${state} · ${record.sentParts}/${record.totalParts} 段`; view.head.children[0].append(badge);
        const open = document.createElement('button'); open.type = 'button'; open.className = 'sms-open'; open.textContent = '查看';
        open.setAttribute('aria-label', `查看发给 ${content.number} 的短信`);
        open.addEventListener('click', () => { detailOrigin = 'outbox'; selectedSMS = null; selectedOutbox = record.id; $('sms-back').textContent = '‹ 已发送'; renderOutbox(); smsView('detail'); $('sms-back').focus(); });
        view.actions.append(open);
        if (content.editable) view.actions.append(button('再次编辑', () => {
          $('sms-number').value = content.number; $('sms-body').value = content.body; updateParts();
          selectedOutbox = null; smsView('compose'); $('sms-body').focus();
        }));
        view.actions.append(button('删除', () => {
          if (!confirm('删除这条发送记录？不会撤回短信。')) return;
          operation(async valid => {
            const result = await request('sms-outbox-delete', {id: record.id});
            if (!valid()) return;
            if (result.deleted !== true) throw new Error('删除未确认，请刷新发送记录。');
            await readOutbox(valid); if (!valid()) return;
            if (outbox.some(x => x.id === record.id)) throw new Error('记录仍在模块中，请刷新核对。');
            status('sms-message', '发送记录已删除。');
          }, 'sms-message');
        }, true));
        if (selectedOutbox === record.id) {
          selectedView = view.item; selectedView.className = 'comm-item sms-full'; open.hidden = true;
          const note = document.createElement('p'); note.className = 'muted';
          note.textContent = record.status === 'submitted' ? '模块已提交短信，不代表对方已收到。' : record.error || '发送结果尚未确认，请核实后再决定是否重新发送。';
          view.item.append(note);
        } else fragment.append(view.item);
      }
      if (detailOrigin === 'outbox') {
        $('sms-detail-content').replaceChildren(...(selectedView ? [selectedView] : []));
        if (selectedOutbox && !selectedView) { selectedOutbox = null; if (smsPanel === 'detail') smsView('outbox'); }
      }
      if (!outbox.length) { const empty = document.createElement('p'); empty.className = 'empty'; empty.textContent = outboxLoaded ? '暂无发送记录' : '尚未读取发送记录'; fragment.append(empty); }
      $('sms-outbox-list').replaceChildren(fragment);
      $('sms-outbox-count').textContent = `已读取 ${outbox.length} 条${outboxNext !== null ? ' · 还有更多' : ''}`;
      $('sms-outbox-more').hidden = outboxNext === null; controls();
    }
    async function readOutbox(valid, more = false) {
      let result;
      if (preview) {
        result = {messages: [
          {id: 'a'.repeat(32), pdus: codec.encode('13800138000', '你好，明天下午三点见。', 1).join(','), timestamp: Date.now()/1000-300, timeExact: true, status: 'submitted', sentParts: 1, totalParts: 1},
          {id: 'b'.repeat(32), pdus: codec.encode('10086', '查询本月剩余流量', 2).join(','), timestamp: Date.now()/1000-3600, timeExact: true, status: 'failed', sentParts: 0, totalParts: 1, error: '模块未接受发送请求。'}
        ], next: null};
      } else result = await request('sms-outbox-list', {cursor: String(more ? outboxNext || 0 : 0), sim});
      if (!valid()) return;
      if (!Array.isArray(result.messages) || result.messages.some(x => !x || !/^[a-f0-9]{32}$/.test(x.id) || typeof x.pdus !== 'string')) throw new Error('发送记录数据不完整，请刷新重试。');
      if (!more) outbox = [];
      for (const record of result.messages) { const i = outbox.findIndex(x => x.id === record.id); if (i < 0) outbox.push(record); else outbox[i] = record; }
      outboxNext = Number.isInteger(result.next) && result.next >= 0 ? result.next : null; outboxLoaded = true; renderOutbox();
      if (result.readErrors) status('sms-message', '部分旧发送记录暂无法读取。', true);
    }
    async function fetchOutbox(more = false) {
      await operation(async valid => { status('sms-message', ''); await readOutbox(valid, more); }, 'sms-message');
    }
    async function migrateHistory(valid) {
      if (preview || !legacyHistory.length) return;
      let migrated = false;
      try { migrated = localStorage.getItem('qdc-module-history-migrated') === '1'; } catch (_) {}
      if (migrated) { legacyHistory = []; return; }
      for (let i = 0; i < legacyHistory.length; i += 20) {
        const records = legacyHistory.slice(i, i + 20).map(x => `${x.direction} ${x.started} ${Number.isSafeInteger(x.ended) && x.ended >= x.started ? x.ended : 0} ${x.number || '-'}`).join('\n');
        await request('call-history-import', { records });
        if (!valid()) return;
      }
      try { localStorage.setItem('qdc-module-history-migrated', '1'); } catch (_) {}
      legacyHistory = [];
    }
    async function readHistory(valid, more = false) {
      if (preview) { history = []; historyNext = null; renderHistory(); return; }
      await migrateHistory(valid); if (!valid()) return;
      const result = await request('call-history-list', { cursor: String(more ? historyNext ?? 0 : 0) });
      if (!valid()) return;
      if (!Array.isArray(result.records) || (result.next !== null && (!Number.isInteger(result.next) || result.next < 0 || result.next > 500))) throw new Error('通话记录返回不完整，请刷新。');
      history = more ? [...history, ...result.records.filter(x => !history.some(row => row.id === x.id))] : result.records;
      historyNext = result.next; renderHistory(); controls();
    }
    async function fetchHistory(more = false) {
      if (busy) { historyPending = true; return; }
      historyPending = false;
      await operation(async valid => { status('call-message', ''); await readHistory(valid, more); }, 'call-message');
    }
    function renderHistory() {
      const fragment = document.createDocumentFragment();
      for (const entry of history) {
        const label = entry.status === 0 ? '进行中' : entry.status === 2 ? '状态中断' : entry.status === 3 ? (entry.ended ? '历史记录 · 已结束' : '历史记录 · 状态未确认') : entry.answered ? `已结束 · ${Math.floor(entry.duration / 60)}分${entry.duration % 60}秒` : entry.direction === 2 ? '未接来电' : '未接通';
        const view = card(entry.number || '号码未提供', `${entry.direction === 2 ? '呼入' : '呼出'} · ${date(entry.started)} · ${label}`);
        view.item.className += ' call-history-item';
        if (/^\+?\d{1,20}$/.test(entry.number)) {
          const callback = button('回拨', () => { $('call-number').value = entry.number; phoneView('dial'); dialNumber(entry.number); });
          callback.dataset.callBack = '1'; view.head.append(callback);
        }
        fragment.append(view.item);
      }
      if (!history.length) { const empty = document.createElement('p'); empty.className = 'empty'; empty.textContent = '暂无记录';fragment.append(empty); }
      $('call-history').replaceChildren(fragment); $('call-history-more').hidden = historyNext === null;
    }
    function renderCalls(calls) {
      const changed = JSON.stringify(activeCalls) !== JSON.stringify(calls);
      activeCalls = calls; haveCallState = true; audio?.calls(calls);
      if (changed && page === 'phone' && phonePanel === 'history') fetchHistory();
      const fragment = document.createDocumentFragment();
      for (const c of calls.filter(x => x.state !== 9)) {
        const view = card(c.number || '号码未提供', c.stateText || '状态未知');
        if (c.state === 2 || c.state === 7) view.actions.append(button('接听', () => performCall('call-answer', c.id)));
        view.actions.append(button('挂断', () => performCall('call-end', c.id), true));fragment.append(view.item);
      }
      if (!fragment.childNodes.length) { const empty = document.createElement('p');empty.className = 'empty';empty.textContent = '当前没有电话';fragment.append(empty); }
      $('call-list').replaceChildren(fragment); $('call-live').hidden = !calls.some(c => c.state !== 9); $('phone-state').textContent = preview ? '演示状态' : calls.some(c => c.state !== 9) ? '通话进行中' : '已连接'; $('phone-state').classList.toggle('ready', !preview); controls();
    }
    async function fetchCalls() {
      await operation(async valid => {
        if (preview) { renderCalls([]);status('call-message', '预览不操作真实电话。');return; }
        try { const result = await request('call-list');if (valid()) { renderCalls(Array.isArray(result.calls) ? result.calls : []);status('call-message', ''); } }
        catch (error) { haveCallState = false; $('phone-state').textContent = '暂未连接'; $('phone-state').classList.toggle('ready', false); throw error; }
      }, 'call-message');
    }
    function performCall(action, id) {
      operation(async valid => {
        if (action === 'call-answer') await audio?.prepare();
        if (!valid()) return;
        // Release the audio socket before sending hangup, so the module can
        // stop its PCM session and finish an in-flight call watchdog query.
        if (action === 'call-end') { audio?.stop('正在挂断电话…');status('call-message', '正在挂断电话…'); }
        await request(action, { callId: String(id), requestId: identifier() });
        if (!valid()) return;
        status('call-message', action === 'call-end' ? '已请求挂断。' : '已请求接听。');
        const result = await request('call-list');if(valid())renderCalls(result.calls || []);
      }, 'call-message');
    }
    $('sms-refresh').addEventListener('click', () => fetchMessages());$('sms-more').addEventListener('click', () => fetchMessages(true));
    $('sms-outbox-refresh').addEventListener('click', () => fetchOutbox()); $('sms-outbox-more').addEventListener('click', () => fetchOutbox(true));
    function updateParts() { const n = $('sms-body').value.length; $('sms-parts').textContent = `已输入 ${n} 个 UTF-16 字符，预计 ${Math.max(1, Math.ceil(n / (n <= 70 ? 70 : 67)))} 段。`; }
    $('sms-body').addEventListener('input', updateParts);
    $('sms-compose').addEventListener('submit', event => {
      event.preventDefault();if(busy||!canMutate||preview)return;
      let pdus;try { pdus = codec.encode($('sms-number').value, $('sms-body').value, crypto.getRandomValues(new Uint8Array(1))[0]); } catch(error) { status('sms-message', error.message, true); return; }
      const number = $('sms-number').value.trim();if (!confirm(`发送短信给 ${number}？共 ${pdus.length} 段，可能产生短信费用。`)) return;
      const requestId = identifier(), expectedSim = sim;
      operation(async valid => {
        status('sms-message', '正在发送，请等待结果…');
        let result;
        try { result = await request('sms-send', { requestId, pdus: pdus.join(','), sim: expectedSim }); }
        catch (error) {
          if (!valid()) return;
          try { await readOutbox(valid); } catch (_) {}
          throw error; // Read the journal only; never retry an uncertain send.
        }
        if (!valid()) return;
        $('sms-body').value = ''; $('sms-parts').textContent = '中文每段最多 70 字；长短信按多段发送和计费。';
        selectedSMS = null; selectedOutbox = null; smsView('outbox');
        try { await readOutbox(valid); } catch (error) {
          if (valid()) status('sms-message', `模块已提交 ${result.sentParts} 段；发送记录读取失败，请刷新已发送列表。`, true);
          return;
        }
        if (valid()) status('sms-message', `模块已提交 ${result.sentParts} 段短信；不代表对方已收到。`);
      }, 'sms-message');
    });
    $('call-refresh').addEventListener('click', () => fetchHistory()); $('call-history-more').addEventListener('click', () => fetchHistory(true));
    for (const [key, letters] of [['1',''],['2','ABC'],['3','DEF'],['4','GHI'],['5','JKL'],['6','MNO'],['7','PQRS'],['8','TUV'],['9','WXYZ'],['*',''],['0','+'],['#','']]) {
      const node = document.createElement('button');node.type = 'button';node.className = 'dial-key';node.setAttribute('aria-label', key);
      const digit = document.createElement('strong'), hint = document.createElement('small');digit.textContent = key;hint.textContent = letters;node.append(digit,hint);
      node.addEventListener('click', () => { if ($('call-number').value.length < 21) $('call-number').value += key; });$('dial-pad').append(node);
    }
    $('dial-backspace').addEventListener('click', () => { $('call-number').value = $('call-number').value.slice(0,-1); });
    function dialNumber(value) {
      if (busy || preview || !authenticated) return;
      if (!haveCallState) { status('call-message', '电话状态未就绪，请刷新后再试。', true); return; }
      if (activeCalls.some(c => c.state !== 9)) { status('call-message', '当前已有电话，请先结束通话。', true); return; }
      const number = value.trim();
      if (!/^\+?\d{1,20}$/.test(number)) { status('call-message', '请输入正确的电话号码；本版暂不支持服务代码。', true);return; }
      operation(async valid => { await audio?.prepare();if(!valid())return;await request('call-dial', { number, requestId: identifier() });if(valid()) { status('call-message','已请求拨号。');const result=await request('call-list');if(valid())renderCalls(result.calls||[]); } },'call-message');
    }
    $('call-dial').addEventListener('submit', event => { event.preventDefault(); dialNumber($('call-number').value); });
    $('call-clear').addEventListener('click', () => { if(busy || preview || !authenticated || !confirm('清除模块中的所有通话记录？手机和电脑都会同步清除，不会挂断当前电话。')) return; operation(async valid => { await migrateHistory(valid); if(!valid())return; await request('call-history-clear'); if(valid()){history=[];historyNext=null;renderHistory();status('call-message','通话记录已清除。');} }, 'call-message'); });
    renderHistory();
    return {
      auth(value) { authenticated = value;generation++;if(!value) { history=[];historyNext=null;historyPending=false;renderHistory();audio?.stop();selectedSMS=null;selectedOutbox=null;outbox=[];outboxNext=null;outboxLoaded=false;detailOrigin='list';$('sms-outbox-list').replaceChildren();$('sms-outbox-count').textContent='';smsView('list');phoneView('dial');$('sms-detail-content').replaceChildren();$('call-live').hidden=true;sim='';messages=[];next=null;activeCalls=[];canMutate=false;haveCallState=false;$('sms-list').replaceChildren();$('call-list').replaceChildren();$('sms-compose').reset();status('sms-message','');status('call-message',''); } controls(); },
      activeCall() { return activeCalls.some(c => c.state === 3); },
      events(data, changedSMS) {
        if (!authenticated) return;
        if (data.callsReady && Array.isArray(data.callState?.calls)) renderCalls(data.callState.calls);
        else { haveCallState = false; $('phone-state').textContent = '暂未连接'; $('phone-state').classList.toggle('ready', false); controls(); }
        if (changedSMS) { if (page === 'sms') { if (busy) smsPending = true; else fetchMessages(); } else { messages = []; next = null; } }
      },
      page(value) { page=value;if(!authenticated)return;if(value==='sms'){if(smsPanel==='outbox'||smsPanel==='detail'&&detailOrigin==='outbox'){if(!outboxLoaded)fetchOutbox();}else if(!messages.length)fetchMessages();}if(value==='phone'){if(phonePanel==='history')fetchHistory();else fetchCalls();} },
      refresh() { if(page==='sms')return smsPanel==='outbox'||smsPanel==='detail'&&detailOrigin==='outbox'?fetchOutbox():fetchMessages();if(page==='phone')return phonePanel==='history'?fetchHistory():fetchCalls(); },
    };
  };
})();
