(() => {
  'use strict';
  const assetBase = document.currentScript ? new URL(document.currentScript.src, location.href).pathname.replace(/\/app\.js$/, '') : '';
  document.body.dataset.assetBase = assetBase;
  const audio = window.QDCModuleAudio ? new window.QDCModuleAudio(api, assetBase) : null;
  const $ = (id) => document.getElementById(id);
  const preview = new URLSearchParams(location.search).get('preview') === '1' &&
    ['localhost', '127.0.0.1', '[::1]', '::1'].includes(location.hostname);
  const pages = {
    network: '网络',
    apps: '应用', system: '系统', settings: '设置', sms: '短信', phone: '电话'
  };
  const state = { authenticated: false, username: '', csrf: '', page: 'network', poll: null,
    pending: null, controller: null, lastStatus: null, toast: null, busy: false, trafficRevision: 0, resettingTraffic: false };
  const communications = window.createQDCCommunications({ api, toast, preview, audio });

  const events = window.QDCModuleEvents && !preview ? new window.QDCModuleEvents(assetBase, {
    state(data, changedSMS) { communications.events?.(data, changedSMS); if (changedSMS) toast('短信列表有更新'); },
    status(value) { text('events-status', value); $('events-status').title = value; },
    activeCall() { return communications.activeCall?.() || false; },
    reconnected() { communications.refresh(); },
    expired() { setAuthenticated(false); message('login-error', '登录已失效，请重新登录。', true); }
  }) : null;
  let applicationTab = 'esim';
  const applicationTabs = ['esim', 'oray', 'bark'];
  function setApplicationTab(tab, load = true) {
    if (!applicationTabs.includes(tab)) return;
    if (tab !== applicationTab && applicationTab === 'oray') { orayRevision++; clearOrayQR(); clearTimeout(heartbeatTimer); heartbeatRevision++; tunnelRevision++; clearTimeout(tunnelTimer); }
    applicationTab = tab;
    for (const name of applicationTabs) {
      $('apps-panel-' + name).hidden = name !== tab;
      $('apps-tab-' + name).setAttribute('aria-selected', String(name === tab));
      $('apps-tab-' + name).setAttribute('tabindex', name === tab ? '0' : '-1');
    }
    if (load && state.authenticated && state.page === 'apps') loadApplication();
  }
  function loadApplication() {
    if (applicationTab === 'esim') loadEsimJob();
    else if (applicationTab === 'oray') { loadTunnel(); loadOray(); loadHeartbeat(); }
    else loadNotify();
  }
  for (const [index, name] of applicationTabs.entries()) {
    const button = $('apps-tab-' + name);
    button.addEventListener('click', () => setApplicationTab(name));
    button.addEventListener('keydown', event => {
      let next = null;
      if (event.key === 'ArrowRight') next = (index + 1) % 3;
      if (event.key === 'ArrowLeft') next = (index + 2) % 3;
      if (event.key === 'Home') next = 0;
      if (event.key === 'End') next = 2;
      if (next !== null) { event.preventDefault(); setApplicationTab(applicationTabs[next]); $('apps-tab-' + applicationTabs[next]).focus?.(); }
    });
  }
  let notificationBusy = false;
  let esimBusy = false, esimRevision = 0;
  let pinBusy = false, pinRevision = 0, pinToken = "", recoverToken = "";
  let esimTarget = null, esimJobActive = false, esimSubmitting = false, esimNeedRead = false, esimJobTimer = null, esimJobRevision = 0;
  const esimJobKey = `qdc-esim-job:${assetBase}`;
  function esimSavedJob(value) {
    try { if (value === undefined) return localStorage.getItem(esimJobKey) || ''; if (value) localStorage.setItem(esimJobKey, value); else localStorage.removeItem(esimJobKey); } catch {}
    return '';
  }
  function closeEsimForm() {
    esimTarget = null; $('esim-manage-form').reset(); $('esim-manage-form').hidden = true;
    message('esim-form-message', '');
  }
  function esimControls() {
    $('sim-pin-open').disabled = !state.authenticated || esimBusy || pinBusy || esimJobActive;
    $('sim-recover-open').disabled = $('sim-pin-open').disabled;
    $('sim-recover-submit').disabled = pinBusy || !recoverToken || !state.authenticated;
    $('sim-pin-refresh').disabled = pinBusy;
    $('sim-pin-cancel').disabled = pinBusy;
    $('sim-pin-submit').disabled = pinBusy || !pinToken || !state.authenticated;
    $('esim-read').disabled = !state.authenticated || esimBusy || pinBusy || esimJobActive;
    $('esim-content').querySelectorAll('button').forEach(button => { button.disabled = esimBusy || pinBusy || esimJobActive || esimNeedRead; });
    $('esim-submit').disabled = !state.authenticated || esimBusy || pinBusy || esimJobActive;
  }
  function closePinForm() {
    pinToken = ''; recoverToken = ''; $('sim-recover-notice').hidden = true; $('sim-pin-input').value = ''; $('sim-pin-form').hidden = true;
    $('sim-pin-submit').hidden = true; $('sim-pin-input-field').hidden = true;
    message('sim-pin-message', '');
  }
  async function loadPinStatus() {
    if (!state.authenticated || pinBusy || esimBusy || esimJobActive) return;
    pinBusy = true; pinToken = ''; recoverToken = ''; $('sim-recover-notice').hidden = true; const revision = ++pinRevision;
    $('sim-pin-input').value = ''; $('sim-pin-form').hidden = false;
    $('sim-pin-input-field').hidden = true; $('sim-pin-submit').hidden = true;
    text('sim-pin-status', '正在读取卡片状态…'); message('sim-pin-message', ''); esimControls();
    try {
      if (preview) throw new Error('预览模式不读取卡片。');
      const result = await api('sim-pin-state', { timeout: 15000 });
      if (!state.authenticated || revision !== pinRevision) return;
      if (typeof result.required !== 'boolean' || (result.required && (!Number.isInteger(result.retries) || result.retries <= 0 || result.retries > 255)) || typeof result.state !== 'string') throw new Error('PIN 状态不完整，请刷新。');
      text('sim-pin-status', `${result.state}${result.required ? ` · 剩余 ${result.retries} 次尝试` : ''}`);
      if (result.required) {
        if (!/^[a-f0-9]{2,2048}$/.test(result.stateToken || '') || result.stateToken.length % 2) throw new Error('卡片状态不完整。');
        pinToken = result.stateToken; $('sim-pin-input-field').hidden = false; $('sim-pin-submit').hidden = false;
      }
      if (result.recoverable === true) {
        if (!/^[a-f0-9]{2,2048}$/.test(result.stateToken || '') || result.stateToken.length % 2) throw new Error('卡片状态不完整。');
        recoverToken = result.stateToken; $('sim-recover-notice').hidden = false;
      }
      if (result.state.includes('PUK')) message('sim-pin-message', 'PIN 已锁定，需要运营商提供的 PUK 码。此处不能继续尝试 PIN。', true);
    } catch (error) {
      if (state.authenticated && revision === pinRevision) { text('sim-pin-status', '未读取'); message('sim-pin-message', error.message, true); }
    } finally { if (revision === pinRevision) { pinBusy = false; esimControls(); } }
  }
  $('sim-pin-open').addEventListener('click', loadPinStatus);
  $('sim-recover-open').addEventListener('click', loadPinStatus);
  $('sim-recover-submit').addEventListener('click', async () => {
    if (!state.authenticated || pinBusy || esimBusy || esimJobActive || !recoverToken) return;
    if (communications.activeCall?.()) return message('sim-pin-message', '请先结束电话。', true);
    const payload = { stateToken: recoverToken, requestId: esimRequestId() };
    recoverToken = ''; pinToken = ''; $('sim-pin-input').value = ''; pinBusy = true; const revision = ++pinRevision; esimControls();
    message('sim-pin-message', '正在重新识别 SIM，请勿拔出卡片或模块…');
    try {
      const result = await api('sim-recover', { method: 'POST', body: payload, timeout: 25000 });
      if (state.authenticated && revision === pinRevision) {
        message('sim-pin-message', result.message, result.recovered === false); text('sim-pin-status', result.state || '请刷新状态确认'); refresh();
      }
    } catch (error) {
      if (state.authenticated && revision === pinRevision) message('sim-pin-message', error.message + '。请先刷新状态核对，勿直接重复操作。', true);
    } finally { if (revision === pinRevision) { pinBusy = false; esimControls(); } }
  });
  $('sim-pin-refresh').addEventListener('click', loadPinStatus);
  $('sim-pin-cancel').addEventListener('click', () => { if (!pinBusy) { pinRevision++; closePinForm(); } });
  $('sim-pin-form').addEventListener('submit', async event => {
    event.preventDefault();
    if (!state.authenticated || pinBusy || esimBusy || esimJobActive || !pinToken) return;
    const pin = $('sim-pin-input').value;
    if (!/^\d{4,8}$/.test(pin)) return message('sim-pin-message', 'PIN 应为 4 至 8 位数字。', true);
    if (communications.activeCall?.()) return message('sim-pin-message', '请先结束电话。', true);
    const payload = { pin, stateToken: pinToken, requestId: esimRequestId() };
    pinToken = ''; pinBusy = true; const revision = ++pinRevision; $('sim-pin-input').value = ''; esimControls();
    message('sim-pin-message', '正在验证 PIN，请稍候…');
    try {
      const result = await api('sim-pin-verify', { method: 'POST', body: payload, timeout: 20000 });
      if (state.authenticated && revision === pinRevision) {
        message('sim-pin-message', result.message || 'PIN 已验证，请刷新 SIM 卡状态。');
        text('sim-pin-status', '验证已完成'); $('sim-pin-input-field').hidden = true; $('sim-pin-submit').hidden = true;
      }
    } catch (error) {
      if (state.authenticated && revision === pinRevision) message('sim-pin-message', `${error.message}${Number.isInteger(error.retries) ? `，剩余 ${error.retries} 次尝试` : ''}。请先点“刷新状态”核对。`, true);
    } finally {
      payload.pin = ''; if (revision === pinRevision) { pinBusy = false; esimControls(); }
    }
  });
  function resetEsim() {
    pinRevision++; pinBusy = false; closePinForm();
    esimRevision++;
    esimJobRevision++; clearTimeout(esimJobTimer); esimJobActive = false; esimSubmitting = false; esimNeedRead = false;
    closeEsimForm(); $('esim-job').hidden = true;
    $('esim-content').replaceChildren(); $('esim-content').hidden = true;
    $('esim-empty').hidden = false;
    text('esim-message', '点击“读卡”，查看卡片及号码配置。');
    $('esim-message').className = 'muted';
    esimControls();
  }
  function renderEsim(data) {
    const root = $('esim-content'); root.replaceChildren(); root.hidden = true;
    $('esim-empty').hidden = false;
    if (data.state === 'unsupported') {
      if (!Array.isArray(data.cards) || data.cards.length) throw new Error('卡片返回结果不完整，请重新读卡。');
      text('esim-message', data.message || '未发现受支持的 eUICC 卡片。');
      return;
    }
    if (data.state !== 'ready' || !Array.isArray(data.cards) || !data.cards.length || data.cards.length > 10) throw new Error('卡片返回结果不完整，请重新读卡。');
    function node(tag, className, value) {
      const el = document.createElement(tag); if (className) el.className = className;
      if (value !== undefined) el.textContent = value; return el;
    }
    function row(list, label, value) {
      const item = node('div'); item.append(node('dt', '', label), node('dd', '', value)); list.append(item);
    }
    for (const card of data.cards) {
      if (!/^\d{32}$/.test(card.eid || '') || !Array.isArray(card.profiles) || card.profiles.length > 32) throw new Error('卡片信息不完整，请重新读卡。');
      const section = node('section', 'esim-result');
      const details = node('dl', 'details'); row(details, 'EID', card.eid);
      if (card.firmware) row(details, '卡片版本', card.firmware);
      if (Number.isSafeInteger(card.freeBytes) && card.freeBytes >= 0) row(details, '剩余容量', bytes(card.freeBytes));
      section.append(details);
      const actions = node('div', 'esim-actions');
      function actionButton(parent, label, operation, profile) {
        const button = node('button', 'secondary', label); button.type = 'button';
        if (operation === 'delete') button.classList.add('danger');
        button.addEventListener('click', () => openEsimForm(operation, card, profile)); parent.append(button);
      }
      actionButton(actions, '下载号码配置', 'download'); actionButton(actions, '补发运营商回执', 'notifications'); section.append(actions);
      const title = node('h3', 'esim-list-title', `号码配置 · ${card.profiles.length}`); section.append(title);
      if (!card.profiles.length) section.append(node('p', 'muted', '此卡片尚未保存号码配置。'));
      for (const profile of card.profiles) {
        if (!/^\d{19,20}$/.test(profile.iccid || '') || typeof profile.enabled !== 'boolean' || typeof profile.name !== 'string' || typeof profile.provider !== 'string') throw new Error('号码配置不完整，请重新读卡。');
        const item = node('article', 'esim-profile'); const head = node('div', 'section-head');
        head.append(node('h3', '', profile.name || profile.provider || '未命名配置'), node('span', `esim-badge${profile.enabled ? ' enabled' : ''}`, profile.enabled ? '已启用' : '未启用'));
        const fields = node('dl', 'details'); row(fields, '运营商', profile.provider || '未提供'); row(fields, 'ICCID', profile.iccid);
        const controls = node('div', 'esim-actions');
        actionButton(controls, profile.enabled ? '停用' : '启用 / 切换', profile.enabled ? 'disable' : 'enable', profile);
        actionButton(controls, '重命名', 'rename', profile);
        if (!profile.enabled) actionButton(controls, '删除', 'delete', profile);
        item.append(head, fields, controls); section.append(item);
      }
      root.append(section);
    }
    $('esim-empty').hidden = true; root.hidden = false;
    text('esim-message', `读卡完成 · ${new Date().toLocaleTimeString()}`);
    esimNeedRead = false; esimSavedJob(''); esimControls();
  }
  $('esim-read').addEventListener('click', async () => {
    if (!state.authenticated || esimBusy || pinBusy || esimJobActive) return;
    closeEsimForm();
    if (communications.activeCall?.()) { text('esim-message', '请在电话空闲时读卡。'); return; }
    esimBusy = true; const revision = ++esimRevision;
    $('esim-read').disabled = true; $('esim-message').className = 'muted';
    $('esim-content').replaceChildren(); $('esim-content').hidden = true; $('esim-empty').hidden = true;
    text('esim-message', '正在读取卡片，请稍候…');
    try {
      if (preview) { text('esim-message', '预览模式不读取实际卡片。'); $('esim-empty').hidden = false; return; }
      const data = await api('esim-list', { method: 'POST', body: {}, timeout: 34000 });
      if (state.authenticated && revision === esimRevision) renderEsim(data);
    } catch (error) {
      if (state.authenticated && revision === esimRevision) {
        $('esim-content').replaceChildren(); $('esim-content').hidden = true; $('esim-empty').hidden = false;
        text('esim-message', error.message); $('esim-message').className = 'form-message error';
      }
    } finally { esimBusy = false; esimControls(); }
  });
  function openEsimForm(operation, card, profile) {
    if (!state.authenticated || esimBusy || pinBusy || esimJobActive || esimNeedRead) return;
    if (communications.activeCall?.()) return message('esim-message', '请先结束电话，再管理 eSIM。', true);
    closeEsimForm(); esimTarget = { operation, aid: card.aid, eid: card.eid, iccid: profile?.iccid || '' };
    const titles = { download: '下载号码配置', enable: '启用 / 切换号码配置', disable: '停用号码配置', rename: '重命名号码配置', delete: '删除号码配置', notifications: '补发运营商回执' };
    const notes = {
      download: '需要可用的网络和运营商激活码。下载完成后不会自动切换号码；操作期间请勿拔出模块。',
      enable: '切换后网络、电话和公网网页可能短暂断开；可通过 USB 本地网页重新读卡查看结果。',
      disable: '停用当前号码后可能无法联网、打电话和收短信，本地管理网页仍可使用。',
      rename: '名称会保存在卡片内，最多 64 字节（中文约 21 字）。',
      delete: '删除后不能恢复；原激活码可能无法再次下载。请确认这是要删除的号码配置。',
      notifications: '仅向运营商补发卡片已有的操作回执，不会重新下载号码配置。'
    };
    text('esim-manage-title', titles[operation]);
    text('esim-manage-target', profile ? `${profile.name || profile.provider || '号码配置'} · ${profile.iccid}` : `当前卡片 · EID 尾号 ${card.eid.slice(-6)}`);
    text('esim-manage-note', notes[operation]);
    $('esim-name-field').hidden = operation !== 'rename'; $('esim-download-fields').hidden = operation !== 'download';
    $('esim-name').value = operation === 'rename' ? profile.name : '';
    text('esim-submit', `确认${operation === 'enable' ? '切换' : operation === 'notifications' ? '补发' : titles[operation].replace('号码配置', '')}`);
    $('esim-manage-form').hidden = false; $('esim-manage-form').scrollIntoView?.({ behavior: 'smooth', block: 'center' }); esimControls();
  }
  $('esim-cancel').addEventListener('click', closeEsimForm);
  function esimRequestId() {
    const b = new Uint8Array(32); crypto.getRandomValues(b); return Array.from(b, v => v.toString(16).padStart(2, '0')).join('');
  }
  async function loadEsimJob(attempt = 0) {
    if (esimSubmitting) return;
    if (!state.authenticated || preview) return;
    clearTimeout(esimJobTimer); const revision = ++esimJobRevision;
    try {
      const result = await api('esim-job'); if (!state.authenticated || revision !== esimJobRevision) return;
      const pending = esimSavedJob();
      if (pending && result.jobId && pending !== result.jobId) {
        esimJobActive = false; esimNeedRead = true; message('esim-job', '上次操作结果未确认，请先重新读卡，勿直接重复下载。', true); esimControls(); return;
      }
      esimJobActive = result.state === 'queued' || result.state === 'running';
      if (esimJobActive) {
        if (!/^[a-f0-9]{64}$/.test(result.jobId || '')) throw new Error('操作状态不完整');
        esimSavedJob(result.jobId); message('esim-job', result.message || '正在处理 eSIM…'); closeEsimForm();
        esimJobTimer = setTimeout(() => loadEsimJob(), 2000);
      } else if (result.state === 'complete' || result.state === 'failed' || result.state === 'interrupted') {
        esimNeedRead = true; message('esim-job', `${result.message || '操作已结束'}。请点击“读卡”核对最新状态。`, result.state !== 'complete');
        if (result.state !== 'interrupted') esimSavedJob('');
      } else if (pending) {
        esimNeedRead = true; message('esim-job', '无法确认上次操作结果，请重新读卡核对；请勿直接重复下载。', true);
      }
      esimControls();
    } catch (error) {
      if (!state.authenticated || revision !== esimJobRevision) return;
      message('esim-job', `状态连接中断：${error.message}，请通过 USB 本地网页查看结果。`, true);
      if (attempt < 8 && (esimJobActive || esimSavedJob())) esimJobTimer = setTimeout(() => loadEsimJob(attempt + 1), 3000);
      else { esimJobActive = false; esimNeedRead = true; esimControls(); }
    }
  }
  $('esim-manage-form').addEventListener('submit', async event => {
    event.preventDefault();
    if (!esimTarget || esimBusy || pinBusy || esimJobActive || esimNeedRead || !state.authenticated) return;
    if (communications.activeCall?.()) return message('esim-form-message', '请先结束电话。', true);
    const payload = { ...esimTarget, requestId: esimRequestId(), name: $('esim-name').value.trim(), activationCode: $('esim-activation').value.trim(), confirmationCode: $('esim-confirmation').value.trim(), imei: $('esim-imei').value.trim() };
    if (payload.operation === 'rename' && (!payload.name || new TextEncoder().encode(payload.name).length > 64)) return message('esim-form-message', '名称不能为空，且最多 64 字节。', true);
    if (payload.operation === 'download') {
      const parts = payload.activationCode.split('$');
      if (parts.length < 3 || parts.length > 5 || parts[0] !== 'LPA:1' || !/^[A-Za-z0-9.-]+\.[A-Za-z0-9-]+$/.test(parts[1]) || /[\r\n\t]/.test(payload.activationCode)) return message('esim-form-message', '请填写完整的 LPA:1$地址$激活码。', true);
      if (parts.length === 5 && parts[4] && parts[4] !== '1') return message('esim-form-message', '激活码确认标记无效。', true);
      if (parts.length === 5 && parts[4] === '1' && !payload.confirmationCode) return message('esim-form-message', '此激活码需要运营商提供的确认码。', true);
      if (payload.imei && !/^\d{15}$/.test(payload.imei)) return message('esim-form-message', 'IMEI 应为 15 位数字，也可留空读取模块。', true);
    }
    if (preview) return message('esim-form-message', '预览模式不执行卡片操作。', true);
    const submissionRevision = ++esimJobRevision; clearTimeout(esimJobTimer); esimSubmitting = true; esimJobActive = true; esimControls(); esimSavedJob(payload.requestId); message('esim-job', '正在提交并核对卡片…'); closeEsimForm();
    try {
      const result = await api('esim-operation', { method: 'POST', body: payload, timeout: 15000 });
      if (!state.authenticated || submissionRevision !== esimJobRevision) return;
      if (!result.accepted || result.jobId !== payload.requestId) throw new Error('提交结果不完整');
      esimSubmitting = false; loadEsimJob();
    } catch (error) {
      if (submissionRevision !== esimJobRevision) return;
      esimSubmitting = false;
      if (state.authenticated) { message('esim-job', `提交结果未确认：${error.message}。正在查询任务，请勿重复提交。`, true); loadEsimJob(); }
    } finally { payload.activationCode = ''; payload.confirmationCode = ''; payload.imei = ''; }
  });
  let dataBusy = false, dataState = null, dataRevision = 0, dataTimer = null;
  async function loadNotify() {
    if (!state.authenticated) return;
    try {
      const data = preview ? { enabled: false, calls: true, sms: true, configured: false, watchReady: true } : await api('notify');
      if (!state.authenticated || notificationBusy) return;
      $('notify-enabled').checked = data.enabled; $('notify-calls').checked = data.calls; $('notify-sms').checked = data.sms;
      text('notify-configured', data.configured ? '推送地址已保存，留空可保留原地址。' : '尚未配置推送地址。');
      text('notify-last', data.lastAttempt ? `${data.lastSuccess ? '上次提醒发送成功' : '上次提醒发送失败'} · ${new Date(data.lastAttempt * 1000).toLocaleString()}` : '尚未发送提醒');
      $('notify-test').disabled = !data.configured || preview;
      if (!data.watchReady && !preview) message('notify-message', '模块事件监听暂未就绪，稍后可以重新进入应用查看。', true);
    } catch (error) { if (state.authenticated) message('notify-message', error.message, true); }
  }
  let tunnelBusy = false, tunnelState = null, tunnelRevision = 0, tunnelTimer = null;
  let heartbeatBusy = false, heartbeatRevision = 0, heartbeatTimer = null, heartbeatState = null, heartbeatEdited = false;
  function heartbeatControls() {
    const disabled = !state.authenticated || heartbeatBusy || !heartbeatState || heartbeatState.pending || tunnelBusy || !!tunnelState?.pending;
    ['heartbeat-tcp', 'heartbeat-udp', 'heartbeat-save', 'heartbeat-default'].forEach(id => { $(id).disabled = disabled; });
    $('heartbeat-refresh').disabled = !state.authenticated || heartbeatBusy;
    if (tunnelState) $('tunnel-toggle').disabled = !state.authenticated || !tunnelState.configured || typeof tunnelState.enabled !== 'boolean' || tunnelBusy || !!tunnelState.pending || heartbeatBusy || !!heartbeatState?.pending;
  }
  async function loadHeartbeat(attempt = 0) {
    if (!state.authenticated || heartbeatBusy || state.page !== 'apps' || applicationTab !== 'oray') return;
    const revision = ++heartbeatRevision;
    clearTimeout(heartbeatTimer);
    try {
      const data = preview ? { tcpSeconds: 30, forwardSeconds: 30, udpSeconds: 10, custom: false, pending: false, lastSuccess: null } : await api('oray-heartbeat');
      if (!state.authenticated || revision !== heartbeatRevision || heartbeatBusy) return;
      heartbeatState = data;
      if (!heartbeatEdited) { $('heartbeat-tcp').value = data.tcpSeconds; $('heartbeat-udp').value = data.udpSeconds; }
      text('heartbeat-current', `${data.custom ? '已保存配置' : '默认配置'}：TCP ${data.tcpSeconds} 秒 · UDP ${data.udpSeconds} 秒${data.forwardSeconds !== data.tcpSeconds ? ` · 转发 TCP ${data.forwardSeconds} 秒` : ''}`);
      if (data.pending) message('heartbeat-message', '正在保存并重连，请稍后确认。');
      else if (data.roamingBlocked) message('data-message', data.roaming === true ? (data.connected ? '数据漫游已关闭，正在停止漫游联网…' : '数据漫游已关闭，当前漫游网络不提供数据联网。') : '数据漫游已关闭，尚未确认是否漫游，暂不提供数据联网。', data.connected === true);
    else if (data.lastSuccess === false) message('heartbeat-message', '上次修改未完成，请核对当前设置后重试。', true);
      else if (data.lastSuccess === true) message('heartbeat-message', '心跳设置已保存。');
      heartbeatControls();
      if (data.pending && attempt < 4) heartbeatTimer = setTimeout(() => loadHeartbeat(attempt + 1), 5000);
    } catch (error) {
      if (state.authenticated && revision === heartbeatRevision) {
        message('heartbeat-message', `${error.message} 可点击“读取设置”核对。`, true);
        if (attempt > 0 && attempt < 4) heartbeatTimer = setTimeout(() => loadHeartbeat(attempt + 1), 5000);
      }
    }
  }
  let orayBusy = false, orayRevision = 0, orayTimer = null, orayDevice = '';
  function clearOrayQR() {
    clearInterval(orayTimer); orayTimer = null;
    $('oray-qr-panel').hidden = true; $('oray-qr-image').removeAttribute('src');
  }
  async function loadOray() {
    if (!state.authenticated || orayBusy) return;
    const revision = ++orayRevision; $('oray-check').disabled = true;
    try {
      const data = preview ? { rpcAvailable: true, online: true, bindingKnown: true, bound: false, deviceSN: '示例设备', account: '' } : await api('oray-status');
      if (!state.authenticated || revision !== orayRevision) return;
      if (orayDevice !== data.deviceSN || data.bound) clearOrayQR();
      orayDevice = data.deviceSN || '';
      text('oray-sn', data.deviceSN);
      text('oray-online', !data.rpcAvailable ? '客户端未就绪' : data.online ? '已连接' : '未连接');
      text('oray-account', !data.bindingKnown ? '暂未确认' : data.bound ? data.account : '未读取到绑定账号');
      message('oray-message', data.bound ? '已读取到绑定账号，域名和映射请在花生壳 App 中管理。' : !data.rpcAvailable ? '请先开启花生壳服务，再检查绑定。' : !data.bindingKnown ? '绑定状态暂未确认，请稍后手动检查。' : '可点击“扫码绑定”生成二维码；如果 App 已显示绑定，请核对设备编号。');
      $('oray-qr').disabled = !data.online || preview;
    } catch (error) { if (state.authenticated && revision === orayRevision) message('oray-message', error.message, true); }
    finally { if (revision === orayRevision) $('oray-check').disabled = false; }
  }
  function renderTunnel(data) {
    tunnelState = data;
    heartbeatControls();
    const known = typeof data.enabled === 'boolean';
    $('tunnel-toggle').disabled = !state.authenticated || !known || !data.configured || tunnelBusy || data.pending || heartbeatBusy || !!heartbeatState?.pending;
    $('tunnel-toggle').setAttribute('aria-checked', data.enabled === true ? 'true' : 'false');
    text('tunnel-toggle-label', data.pending || tunnelBusy ? '切换中…' : !known ? '未确认' : data.enabled ? '已开启' : '已关闭');
    const labels = { running: '服务运行中', stopped: '已停止', waiting: '等待蜂窝联网', blocked: '服务异常，已暂停', unavailable: '状态暂不可用' };
    text('tunnel-state', data.pending ? '正在切换…' : labels[data.state] || '状态暂不可用');
    text('tunnel-autostart', known ? data.enabled ? '已开启' : '已关闭' : '未读取');
    // Only backend-validated HTTPS origins are offered as links.
    const url = typeof data.publicURL === 'string' && /^https:\/\/[a-zA-Z0-9.-]+\/module\/$/.test(data.publicURL) ? data.publicURL : '';
    text('tunnel-address', url, '尚未配置');
    $('tunnel-copy').disabled = !url;
    $('tunnel-open').hidden = !url;
    if (url) $('tunnel-open').href = url; else $('tunnel-open').removeAttribute('href');
    if (!data.configured) message('tunnel-message', '花生壳尚未安装或配置，请先完成设备绑定。', true);
    else if (data.roamingBlocked) message('data-message', data.roaming === true ? (data.connected ? '数据漫游已关闭，正在停止漫游联网…' : '数据漫游已关闭，当前漫游网络不提供数据联网。') : '数据漫游已关闭，尚未确认是否漫游，暂不提供数据联网。', data.connected === true);
    else if (data.lastSuccess === false) message('tunnel-message', '上次切换未完成，请刷新状态后重试。', true);
    else if (data.state === 'blocked') message('tunnel-message', '花生壳客户端出现异常，请检查服务。', true);
  }
  async function loadTunnel() {
    if (!state.authenticated || tunnelBusy) return;
    const revision = ++tunnelRevision;
    clearTimeout(tunnelTimer);
    try {
      const data = preview ? { enabled: true, configured: true, state: 'running', pending: false, publicURL: 'https://example.com/module/' } : await api('tunnel');
      if (!state.authenticated || tunnelBusy || revision !== tunnelRevision) return;
      renderTunnel(data);
      // A switch gets a bounded follow-up, never a continuous status poll.
      if (data.pending && state.page === 'apps' && applicationTab === 'oray') tunnelTimer = setTimeout(() => loadTunnel(), 3000);
    } catch (error) { if (state.authenticated && revision === tunnelRevision) message('tunnel-message', error.message, true); }
  }
  function text(id, value, fallback = '未读取') {
    $(id).textContent = value === null || value === undefined || value === '' ? fallback : String(value);
  }
  function message(id, value, error = false) {
    $(id).textContent = value || '';
    $(id).hidden = !value;
    $(id).classList.toggle('error', error);
  }
  function toast(value) {
    clearTimeout(state.toast);
    $('toast').textContent = value;
    $('toast').hidden = false;
    state.toast = setTimeout(() => { $('toast').hidden = true; }, 4200);
  }
  function bytes(value) {
    if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) return '未读取';
    if (value < 1024) return `${Math.round(value)} B`;
    const units = ['KB', 'MB', 'GB', 'TB'];
    let count = value / 1024, unit = 0;
    while (count >= 1024 && unit < units.length - 1) { count /= 1024; unit += 1; }
    return `${count >= 100 ? Math.round(count) : Number(count.toFixed(1))} ${units[unit]}`;
  }
  function uptime(value) {
    if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) return '未读取';
    const minutes = Math.floor(value / 60), hours = Math.floor(minutes / 60), days = Math.floor(hours / 24);
    if (days) return `${days} 天 ${hours % 24} 小时`;
    if (hours) return `${hours} 小时 ${minutes % 60} 分钟`;
    return `${minutes} 分钟`;
  }
  function percentage(total, available) {
    if (typeof total !== 'number' || typeof available !== 'number' || !Number.isFinite(total) || !Number.isFinite(available) || total <= 0) return null;
    return Math.min(100, Math.max(0, Math.round((total - available) / total * 100)));
  }
  function meter(node, value) {
    node.firstElementChild.style.width = `${value === null ? 0 : value}%`;
    node.classList.toggle('unknown', value === null);
    if (value === null) { node.removeAttribute('aria-valuenow'); node.setAttribute('aria-valuetext', '尚未读取'); }
    else { node.setAttribute('aria-valuenow', String(value)); node.setAttribute('aria-valuetext', `已使用 ${value}%`); }
  }
  function setPage(page) {
    if (!pages[page]) page = 'network';
    state.page = page;
    if (page !== 'apps') { orayRevision++; clearOrayQR(); heartbeatRevision++; clearTimeout(heartbeatTimer); tunnelRevision++; clearTimeout(tunnelTimer); }
    if (page !== 'network' && !pinBusy) { pinRevision++; closePinForm(); esimControls(); }
    document.body.dataset.page = page;
    $('phone-state').hidden = page !== 'phone';
    document.querySelectorAll('.page').forEach((node) => { node.hidden = node.id !== `page-${page}`; });
    document.querySelectorAll('[data-page]').forEach((node) => {
      const active = node.dataset.page === page;
      node.classList.toggle('selected', active);
      if (active) node.setAttribute('aria-current', 'page'); else node.removeAttribute('aria-current');
    });
    text('page-title', pages[page]);
    if (location.hash !== `#${page}`) history.replaceState(null, '', `${location.pathname}${location.search}#${page}`);
    communications.page(page);
    if (state.authenticated && page === 'apps') { setApplicationTab(applicationTab, false); loadApplication(); }
    if (state.authenticated && (page === 'network' || page === 'system')) refresh();
  }
  function setAuthenticated(authenticated, session = {}) {
    state.authenticated = authenticated;
    state.username = authenticated ? session.username || 'admin' : '';
    state.csrf = authenticated ? session.csrfToken || '' : '';
    communications.auth(authenticated);
    events?.auth(authenticated);
    resetEsim();
    $('login-view').hidden = authenticated;
    $('admin-view').hidden = !authenticated;
    clearInterval(state.poll); state.poll = null;
    if (authenticated) {
      text('settings-user', state.username);
      setPage(location.hash.slice(1) || 'network');
      if (preview) text('events-status', '预览模式 · 不连接模块');
    } else {
      heartbeatRevision++; clearTimeout(heartbeatTimer); heartbeatState = null; heartbeatEdited = false; $('heartbeat-form').reset(); heartbeatControls(); message('heartbeat-message', '');
      orayRevision++; clearOrayQR(); orayDevice = ''; $('oray-qr').disabled = true;
      if (state.controller) state.controller.abort();
      state.lastStatus = null;
      state.trafficRevision++;
      ['traffic-received', 'traffic-sent', 'traffic-total'].forEach((id) => text(id, '—'));
      $('login-password').value = '';
      $('password-form').reset();
      $('notify-url').value = '';
      clearTimeout(tunnelTimer); tunnelRevision++; tunnelState = null; $('tunnel-toggle').disabled = true; $('tunnel-copy').disabled = true; $('tunnel-open').hidden = true;
      clearTimeout(dataTimer); dataRevision++; dataState = null; $('data-toggle').disabled = true; $('usb-toggle').disabled = true; $('compat-toggle').disabled = true; $('roaming-toggle').disabled = true;
      message('password-message', '');
      message('status-error', '');
    }
  }
  async function api(path, options = {}) {
    const controller = options.signal ? null : new AbortController();
    /* The module performs 600,000 password-hash rounds. Give auth requests
     * time to finish on its small CPU and across the public tunnel. */
    const timeout = options.timeout || ((path === 'login' || path === 'password') ? 35000 : 10000);
    const timer = controller ? setTimeout(() => controller.abort(), timeout) : null;
    const headers = { Accept: 'application/json' };
    if (options.body) headers['Content-Type'] = 'application/json';
    const mutating = options.method && options.method !== 'GET' && path !== 'login';
    if (mutating) headers['X-CSRF-Token'] = state.csrf;
    try {
      const response = await fetch(`${assetBase}/cgi-bin/api/${path}`, {
        method: options.method || 'GET', credentials: 'same-origin', cache: 'no-store',
        headers, body: options.body ? JSON.stringify(mutating ? { ...options.body, csrfToken: state.csrf } : options.body) : undefined,
        signal: options.signal || controller.signal
      });
      const result = await response.json().catch(() => ({}));
      if (!response.ok || result.ok === false) {
        if (response.status === 401 && path !== 'login') {
          setAuthenticated(false);
          if (path !== 'session') message('login-error', '登录已失效，请重新登录。', true);
        }
        const detail = typeof result.message === 'string' ? result.message : typeof result.error === 'string' ? result.error : '';
        const error = new Error(detail || ((response.status === 401 || response.status === 403) ? '账号或密码有误，请重新输入。' : '请求失败，请稍后再试。'));
        error.status = response.status;
        if (Number.isInteger(result.retries)) error.retries = result.retries;
        throw error;
      }
      return result;
    } catch (error) {
      if (error.name === 'AbortError') throw new Error('请求超时，请检查与模块的连接。');
      if (error instanceof TypeError) throw new Error('无法连接模块，请检查 USB 或网络连接。');
      throw error;
    } finally { if (timer) clearTimeout(timer); }
  }
  function renderStatus(status) {
    state.lastStatus = status;
    const network = status.network || {}, memory = status.memory || {};
    const notes = [];
    if (status.radioError) notes.push(status.radioError);
    if (network.registrationState >= 0 && network.registrationState !== 1) {
      const registration = ['尚未注册运营商', '', '正在搜索网络', '运营商拒绝注册', '注册状态未知'];
      notes.push(registration[network.registrationState] || '尚未注册运营商');
    }
    if (network.apnSource === 'current' && network.apnAddress && network.mobileAddress && network.apnAddress !== network.mobileAddress) {
      network.apn = null;
      notes.push('APN 查询对应另一条数据连接，暂不显示为当前 APN。');
    }
    const missing = [['运营商', 'operator'], ['网络类型', 'networkType'], ['信号', 'signalDbm'], ['SIM 状态', 'simState'], ['APN', 'apn']]
      .filter(([, key]) => network[key] === null || network[key] === undefined).map(([label]) => label);
    if (missing.length && !status.radioError) notes.push(`暂未读到${missing.join('、')}，下次刷新会重新查询。`);
    message('radio-note', notes.join(' '));
    text('net-apn-label', network.apnSource === 'configured' ? 'APN（默认配置）尚未确认是当前连接使用的值。' : 'APN');
    const signal = typeof network.signalDbm === 'number' ? `${network.signalDbm} dBm` : null;
    text('system-model', status.model, '4G 模块');
    text('system-hostname', status.hostname);
    text('net-operator', network.operator);
    text('net-type', network.networkType);
    text('net-signal', signal);
    text('net-sim', network.simState);
    text('system-uptime', uptime(status.uptimeSeconds));
    const badge = $('network-badge');
    badge.lastElementChild.textContent = network.connected === true ? '数据接口已连接' : network.connected === false ? '数据接口未连接' : '连接状态未读取';
    badge.classList.toggle('online', network.connected === true);
    badge.classList.toggle('offline', network.connected === false);
    for (const [id, key] of [['net-apn', 'apn'], ['net-usb-interface', 'usbInterface'], ['net-usb-address', 'usbAddress'], ['net-mobile-interface', 'mobileInterface'], ['net-mobile-address', 'mobileAddress']]) text(id, network[key]);
    text('memory-available', bytes(memory.availableBytes));
    text('memory-total', `总计 ${bytes(memory.totalBytes)}`);
    const used = typeof memory.totalBytes === 'number' && typeof memory.availableBytes === 'number' ? Math.max(0, memory.totalBytes - memory.availableBytes) : null;
    text('memory-used', `已用 ${bytes(used)}`); text('memory-free', `完全空闲 ${bytes(memory.freeBytes)}`);
    meter($('memory-meter'), percentage(memory.totalBytes, memory.availableBytes));
    const storage = Array.isArray(status.storage) ? status.storage : [];
    const fragment = document.createDocumentFragment();
    for (const partition of storage) {
      const item = document.createElement('div'); item.className = 'storage-item';
      const heading = document.createElement('div'); heading.className = 'storage-heading';
      const name = document.createElement('strong'); name.textContent = partition.name || partition.mount || '存储分区';
      const mount = document.createElement('small'); mount.textContent = partition.mount || '';
      heading.append(name, mount);
      const progress = document.createElement('div'); progress.className = 'progress-track';
      progress.setAttribute('role', 'progressbar'); progress.setAttribute('aria-label', `${name.textContent}已使用空间`);
      progress.setAttribute('aria-valuemin', '0'); progress.setAttribute('aria-valuemax', '100'); progress.append(document.createElement('span'));
      meter(progress, percentage(partition.totalBytes, partition.availableBytes));
      const legend = document.createElement('div'); legend.className = 'resource-legend';
      const available = document.createElement('span'); available.textContent = `可用 ${bytes(partition.availableBytes)}`;
      const total = document.createElement('span'); total.textContent = `总计 ${bytes(partition.totalBytes)}`;
      legend.append(available, total); item.append(heading, progress, legend); fragment.append(item);
    }
    if (!storage.length) { const empty = document.createElement('p'); empty.className = 'empty'; empty.textContent = '存储信息暂不可用'; fragment.append(empty); }
    $('storage-list').replaceChildren(fragment);
    text('settings-version', status.version);
    const updated = status.updatedAt ? new Date(typeof status.updatedAt === 'number' ? status.updatedAt * 1000 : status.updatedAt) : new Date();
    text('updated-at', Number.isNaN(updated.getTime()) ? '状态已更新' : `${updated.toLocaleTimeString('zh-CN', { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' })}`);
  }
  function previewStatus() {
    return {
      version: '0.3.37', model: 'QDC507', hostname: 'qdc507', uptimeSeconds: 97320,
      memory: { totalBytes: 44459622, availableBytes: 11010048, freeBytes: 2936012 },
      storage: [
        { name: '系统存储', mount: '/', totalBytes: 60397978, availableBytes: 8912896 },
        { name: '数据存储', mount: '/usrdata', totalBytes: 16043213, availableBytes: 15833497 }
      ],
      network: { usbAddress: '192.168.225.1', usbInterface: 'usb0', mobileInterface: 'rmnet0', mobileAddress: '10.26.8.12', connected: true, apn: 'cmnet', operator: '中国移动', signalDbm: -90, networkType: 'LTE', simState: '已就绪' },
      stateSource: 'preview', updatedAt: new Date().toISOString()
    };
  }
  function renderData(data) {
    dataState = data;
    const known = typeof data.enabled === 'boolean';
    $('data-toggle').disabled = !known || dataBusy || data.pending || !state.authenticated;
    $('data-toggle').setAttribute('aria-checked', data.enabled === true ? 'true' : 'false');
    const matches = data.lastSuccess !== false && typeof data.connected === 'boolean' && data.connected === (typeof data.effectiveEnabled === 'boolean' ? data.effectiveEnabled : data.enabled);
    text('data-toggle-label', dataBusy || data.pending ? '切换中…' : !known || typeof data.connected !== 'boolean' ? '状态未确认' : !matches ? (data.enabled ? '开启未完成' : '关闭未完成') : data.enabled ? '蜂窝联网 · 开' : '蜂窝联网 · 关');
    const roamingKnown = typeof data.roamingEnabled === 'boolean';
    const roamingMatches = typeof data.roamingApplied === 'boolean' && data.roamingApplied === data.roamingEnabled;
    $('roaming-toggle').disabled = !roamingKnown || dataBusy || data.pending || !state.authenticated;
    $('roaming-toggle').setAttribute('aria-checked', data.roamingEnabled === true ? 'true' : 'false');
    text('roaming-toggle-label', dataBusy || data.pending ? '数据漫游 · 切换中…' : !roamingKnown ? '数据漫游 · 未确认' : !roamingMatches ? '数据漫游 · 未完成' : data.roamingEnabled ? '数据漫游 · 开' : '数据漫游 · 关');
    text('net-roaming', data.roaming === true ? '正在漫游' : data.roaming === false ? '本地网络' : '未确认');
    const usbKnown = typeof data.usbEnabled === 'boolean';
    const usbMatches = data.usbLastSuccess !== false && typeof data.usbForwarding === 'boolean' && data.usbForwarding === data.usbEnabled;
    $('usb-toggle').disabled = !usbKnown || dataBusy || data.pending || !state.authenticated;
    $('usb-toggle').setAttribute('aria-checked', data.usbEnabled === true ? 'true' : 'false');
    text('usb-toggle-label', dataBusy || data.pending ? 'USB 上网 · 切换中…' : !usbKnown || typeof data.usbForwarding !== 'boolean' ? 'USB 上网 · 未确认' : !usbMatches ? 'USB 上网 · 未完成' : data.usbEnabled ? 'USB 上网 · 开' : 'USB 上网 · 关');
    const compatKnown = typeof data.compatEnabled === 'boolean';
    $('compat-toggle').disabled = !compatKnown || !data.compatSupported || dataBusy || data.pending || !state.authenticated;
    $('compat-toggle').setAttribute('aria-checked', data.compatEnabled === true ? 'true' : 'false');
    const compatMatches = typeof data.compatApplied === 'boolean' && data.compatApplied === data.compatEnabled;
    text('compat-toggle-label', dataBusy || data.pending ? 'USB 漫游兼容 · 切换中…' : !compatKnown ? 'USB 漫游兼容 · 未确认' : !data.compatSupported ? 'USB 漫游兼容 · 不可用' : !compatMatches ? 'USB 漫游兼容 · 未完成' : data.compatEnabled ? 'USB 漫游兼容 · 开' : 'USB 漫游兼容 · 关');
    if (typeof data.connected === 'boolean') {
      const badge = $('network-badge');
      badge.lastElementChild.textContent = data.connected ? '蜂窝已连接' : '蜂窝未连接';
      badge.classList.toggle('online', data.connected); badge.classList.toggle('offline', !data.connected);
    }
    if (data.pending) message('data-message', '正在切换联网设置…');
    else if (roamingKnown && !roamingMatches) message('data-message', '模块的漫游权限尚未确认生效，请刷新核对；设置会在后台重新确认。', true);
    else if (data.roamingBlocked) message('data-message', data.roaming === true ? (data.connected ? '数据漫游已关闭，正在停止漫游联网…' : '数据漫游已关闭，当前漫游网络不提供数据联网。') : '数据漫游已关闭，尚未确认是否漫游，暂不提供数据联网。', data.connected === true);
    else if (data.lastSuccess === false || !matches && typeof data.connected === 'boolean') message('data-message', '数据切换尚未确认成功，请刷新状态；可通过 USB 本地页面重试。', true);
    else if (typeof data.connected !== 'boolean') message('data-message', '实际数据连接状态暂未读取，请刷新确认。', true);
    else if (!usbMatches) message('data-message', 'USB 上网状态尚未确认，请刷新或重试。', true);
    else if (data.usbEnabled && !data.connected) message('data-message', 'USB 上网已开启；需先开启蜂窝联网，连接的设备才能上网。');
    else message('data-message', '');
  }
  async function loadData() {
    if (!state.authenticated || dataBusy) return;
    const revision = dataRevision;
    try {
      const data = preview ? { enabled: true, connected: true, usbEnabled: true, usbForwarding: true, usbConnected: true, compatEnabled: false, compatApplied: false, compatSupported: true, roamingEnabled: true, roamingApplied: true, roaming: false, roamingBlocked: false, effectiveEnabled: true, pending: false } : await api('data', { timeout: 15000 });
      if (!state.authenticated || dataBusy || revision !== dataRevision) return;
      renderData(data);
      clearTimeout(dataTimer);
      if (data.pending) dataTimer = setTimeout(() => loadData(), 2500);
    } catch (error) {
      if (state.authenticated && !dataBusy && revision === dataRevision) {
        $('data-toggle').disabled = true; $('usb-toggle').disabled = true; $('compat-toggle').disabled = true; $('roaming-toggle').disabled = true;
        message('data-message', `数据状态读取失败：${error.message}`, true);
      }
    }
  }
  function renderTraffic(traffic) {
    text('traffic-received', bytes(traffic.receivedBytes));
    text('traffic-sent', bytes(traffic.sentBytes));
    text('traffic-total', bytes(traffic.receivedBytes + traffic.sentBytes));
    text('traffic-since', `统计起点：${new Date(traffic.since * 1000).toLocaleString()}${traffic.interfaces ? '' : ' · 蜂窝接口暂未就绪'}`);
    message('traffic-error', '');
  }
  async function refreshTraffic(signal) {
    const revision = state.trafficRevision;
    try {
      const traffic = preview ? { receivedBytes: 12582912, sentBytes: 3145728, since: Math.floor(Date.now() / 1000), interfaces: 1 } : await api('traffic', { signal });
      if (state.authenticated && revision === state.trafficRevision && !state.resettingTraffic) renderTraffic(traffic);
    } catch (error) {
      if (state.authenticated && revision === state.trafficRevision) message('traffic-error', `流量统计读取失败：${error.message}`, true);
    }
  }
  async function refresh() {
    if (!state.authenticated || document.hidden) return;
    if (state.pending) return state.pending;
    $('refresh-status').disabled = true; $('refresh-status').classList.add('busy');
    const controller = new AbortController(); state.controller = controller;
    const timer = setTimeout(() => controller.abort(), 12000);
    state.pending = (async () => {
      try {
        const status = await (preview ? Promise.resolve(previewStatus()) : api('status', { signal: controller.signal }));
        await refreshTraffic(controller.signal);
        if (!preview && state.authenticated && !controller.signal.aborted) {
          renderStatus({ ...status, radioError: '正在读取蜂窝信息…' });
          /* Always replace radio values with this query's results; never reuse
             the previous SIM's operator or APN after a failed refresh. */
          try {
            const radio = await api('radio', { signal: controller.signal });
            status.network = { ...status.network, ...radio.network };
            status.stateSource = radio.stateSource;
          } catch (error) { status.radioError = `蜂窝信息读取失败：${error.message}`; }
        }
        if (!state.authenticated) return;
        renderStatus(status); message('status-error', ''); await loadData();
      } catch (error) {
        if (state.authenticated) message('status-error', `${error.message}${state.lastStatus ? ' 当前显示上次读取的状态。' : ''}`);
      } finally {
        clearTimeout(timer); if (state.controller === controller) state.controller = null;
        state.pending = null; $('refresh-status').disabled = false; $('refresh-status').classList.remove('busy');
      }
    })();
    return state.pending;
  }
  async function logout() {
    if (state.busy) return;
    state.busy = true;
    try {
      if (!preview) await api('logout', { method: 'POST', body: {} });
      setAuthenticated(false); message('login-error', ''); toast('已退出登录');
    } catch (error) {
      if (state.authenticated) toast(error.message);
    } finally { state.busy = false; }
  }
  $('login-form').addEventListener('submit', async (event) => {
    event.preventDefault(); if (state.busy) return;
    state.busy = true; $('login-submit').disabled = true; text('login-submit', '正在登录…'); message('login-error', '');
    try {
      const result = preview ? { ok: true, username: 'admin', csrfToken: 'preview' } : await api('login', { method: 'POST', body: { username: $('login-username').value.trim(), password: $('login-password').value, remember: $('login-remember').checked ? '1' : '0' } });
      $('login-password').value = '';
      setAuthenticated(true, { ...result, username: result.username || $('login-username').value.trim() });
      await refresh();
    } catch (error) { message('login-error', error.message, true); }
    finally { state.busy = false; $('login-submit').disabled = false; text('login-submit', '登录'); }
  });
  $('password-form').addEventListener('submit', async (event) => {
    event.preventDefault(); if (state.busy) return;
    const currentPassword = $('current-password').value, newPassword = $('new-password').value, confirmPassword = $('confirm-password').value;
    if (newPassword !== confirmPassword) return message('password-message', '两次输入的新密码不一致。', true);
    const passwordBytes = new TextEncoder().encode(newPassword).length;
    if ([...newPassword].length < 8 || passwordBytes > 128) return message('password-message', '新密码至少 8 个字符，最长 128 字节。', true);
    if (newPassword === currentPassword) return message('password-message', '请使用与当前密码不同的新密码。', true);
    state.busy = true; $('password-submit').disabled = true; text('password-submit', '正在保存…'); message('password-message', '');
    try {
      const result = preview ? { ok: true } : await api('password', { method: 'POST', body: { currentPassword, newPassword, confirmPassword } });
      $('password-form').reset();
      if (result.csrfToken) state.csrf = result.csrfToken;
      if (result.reauthenticate || result.relogin) { setAuthenticated(false); toast('密码已修改，请使用新密码登录'); }
      else message('password-message', preview ? '预览演示：密码未实际修改。' : '密码已修改，下次登录请使用新密码。');
    } catch (error) { message('password-message', error.message, true); }
    finally { state.busy = false; $('password-submit').disabled = false; text('password-submit', '保存新密码'); }
  });
  document.querySelectorAll('[data-page]').forEach((node) => node.addEventListener('click', () => setPage(node.dataset.page)));
  document.querySelectorAll('[data-open]').forEach((node) => node.addEventListener('click', () => { setPage(node.dataset.open); window.scrollTo(0, 0); }));
  $('refresh-status').addEventListener('click', () => { refresh(); communications.refresh(); if (state.page === 'apps') loadApplication(); });
  async function switchData(scope) {
    const usb = scope === 'usb', compat = scope === 'compat', roam = scope === 'roaming', key = roam ? 'roamingEnabled' : compat ? 'compatEnabled' : usb ? 'usbEnabled' : 'enabled';
    if (dataBusy || !state.authenticated || !dataState || dataState.pending || typeof dataState[key] !== 'boolean') return;
    const enabled = !dataState[key];
    if (!usb && !compat && !enabled && communications.activeCall?.()) { message('data-message', roam ? '请先结束电话，再关闭数据漫游。' : '请先结束电话，再关闭蜂窝联网。', true); return; }
    if (roam && !enabled && !window.confirm('关闭数据漫游后，如果模块正在漫游，USB 上网、公网网页和 Bark 提醒会断开；重新开启可通过 USB 本地页面操作。确定关闭吗？')) return;
    if (!usb && !compat && !roam && !enabled && !window.confirm('关闭蜂窝联网后，USB 上网、公网网页和 Bark 提醒都会断开。重新开启需要通过 USB 本地页面操作。确定关闭吗？')) return;
    dataBusy = true; dataRevision++; clearTimeout(dataTimer); $('data-toggle').disabled = true; $('usb-toggle').disabled = true; $('compat-toggle').disabled = true; $('roaming-toggle').disabled = true;
    text(roam ? 'roaming-toggle-label' : compat ? 'compat-toggle-label' : usb ? 'usb-toggle-label' : 'data-toggle-label', '切换中…');
    if (state.controller) state.controller.abort();
    try {
      const result = preview ? { ...dataState, [key]: enabled, pending: false, ...(roam ? { roamingApplied: enabled, connected: dataState.enabled && (enabled || dataState.roaming === false), effectiveEnabled: dataState.enabled && (enabled || dataState.roaming === false), roamingBlocked: dataState.enabled && !enabled && dataState.roaming !== false } : compat ? { compatApplied: enabled } : usb ? { usbForwarding: enabled, usbConnected: enabled && dataState.connected } : { connected: enabled, usbConnected: enabled && dataState.usbForwarding }) } : await api('data-set', { method: 'POST', body: { scope, enabled: enabled ? '1' : '0' }, timeout: 15000 });
      if (!state.authenticated) return;
      dataBusy = false; renderData(result);
      message('data-message', preview ? '预览演示，未改变模块网络。' : roam ? (enabled ? '已请求开启数据漫游。' : '已请求关闭数据漫游；当前若处于漫游网络，将停止数据联网。') : compat ? (enabled ? '已开启 USB 漫游兼容。' : '已关闭 USB 漫游兼容。') : usb ? (enabled ? '已请求开启 USB 上网。' : '已请求关闭 USB 上网，本地管理网页和模块自身联网继续保留。') : enabled ? '已请求开启蜂窝联网，正在确认连接…' : '已请求关闭蜂窝联网；公网页面即将断开，重新开启请使用 USB 本地页面。');
      if (!preview) dataTimer = setTimeout(() => loadData(), 3000);
    } catch (error) {
      if (state.authenticated) message('data-message', `切换结果未确认：${error.message} 请刷新核对；若已断网，请使用 USB 本地页面。`, true);
    } finally {
      dataBusy = false; dataRevision++;
      if (state.authenticated && dataState) {
        $('data-toggle').disabled = dataState.pending || typeof dataState.enabled !== 'boolean';
        $('usb-toggle').disabled = dataState.pending || typeof dataState.usbEnabled !== 'boolean';
        $('roaming-toggle').disabled = dataState.pending || typeof dataState.roamingEnabled !== 'boolean';
        $('compat-toggle').disabled = dataState.pending || !dataState.compatSupported || typeof dataState.compatEnabled !== 'boolean';
      }
    }
  }
  $('data-toggle').addEventListener('click', () => switchData('cellular'));
  $('usb-toggle').addEventListener('click', () => switchData('usb'));
  $('roaming-toggle').addEventListener('click', () => switchData('roaming'));
  $('compat-toggle').addEventListener('click', () => switchData('compat'));
  $('traffic-reset').addEventListener('click', async () => {
    if (state.resettingTraffic || !state.authenticated) return;
    state.resettingTraffic = true; state.trafficRevision++;
    $('traffic-reset').disabled = true; text('traffic-reset', '清零中…');
    try {
      const result = preview ? { receivedBytes: 0, sentBytes: 0, since: Math.floor(Date.now() / 1000), interfaces: 1 } : await api('traffic-reset', { method: 'POST', body: {} });
      if (state.authenticated) { renderTraffic(result); toast('累计接收、累计发送和总流量已清零'); }
    } catch (error) { if (state.authenticated) message('traffic-error', `清零未完成：${error.message}`, true); }
    finally { state.resettingTraffic = false; state.trafficRevision++; $('traffic-reset').disabled = false; text('traffic-reset', '清零'); }
  });
  $('tunnel-toggle').addEventListener('click', async () => {
    if (tunnelBusy || heartbeatBusy || heartbeatState?.pending || !state.authenticated || !tunnelState || tunnelState.pending || typeof tunnelState.enabled !== 'boolean') return;
    const enabled = !tunnelState.enabled;
    if (!enabled && communications.activeCall?.()) return message('tunnel-message', '请先结束电话，再关闭花生壳。', true);
    if (!enabled && !window.confirm('关闭花生壳后，公网网页将断开，开机后也不会自动启动。重新开启需要通过 USB 本地页面操作。确定关闭吗？')) return;
    tunnelBusy = true; tunnelRevision++; clearTimeout(tunnelTimer); $('tunnel-toggle').disabled = true; text('tunnel-toggle-label', '切换中…'); message('tunnel-message', '');
    try {
      const result = preview ? { ...tunnelState, enabled, state: enabled ? 'running' : 'stopped', pending: false } : await api('tunnel-set', { method: 'POST', body: { enabled: enabled ? '1' : '0' }, timeout: 15000 });
      if (!state.authenticated) return;
      tunnelBusy = false; renderTunnel(result);
      message('tunnel-message', preview ? '预览演示，未修改花生壳。' : enabled ? '已请求开启花生壳，请稍后刷新确认。' : '已请求关闭花生壳；公网页面即将断开。');
      if (!preview && enabled) tunnelTimer = setTimeout(() => loadTunnel(), 3000);
    } catch (error) { if (state.authenticated) message('tunnel-message', `切换结果未确认：${error.message} 请刷新核对；若公网已断开，请使用 USB 本地页面。`, true); }
    finally { tunnelBusy = false; tunnelRevision++; if (state.authenticated && tunnelState) $('tunnel-toggle').disabled = tunnelState.pending || !tunnelState.configured || typeof tunnelState.enabled !== 'boolean'; }
  });
  $('tunnel-copy').addEventListener('click', async () => {
    const url = $('tunnel-open').getAttribute('href');
    if (!url || !state.authenticated) return;
    try { await navigator.clipboard.writeText(url); toast('公网地址已复制'); }
    catch { message('tunnel-message', '请长按或选中公网地址手动复制。'); }
  });
  $('oray-check').addEventListener('click', loadOray);
  ['heartbeat-tcp', 'heartbeat-udp'].forEach(id => $(id).addEventListener('input', () => { heartbeatEdited = true; }));
  $('heartbeat-refresh').addEventListener('click', () => { heartbeatEdited = false; loadHeartbeat(); });
  $('heartbeat-default').addEventListener('click', () => {
    $('heartbeat-tcp').value = '30'; $('heartbeat-udp').value = '10'; heartbeatEdited = true;
    message('heartbeat-message', '已填入默认值，点击“保存并重连”生效。');
  });
  $('heartbeat-form').addEventListener('submit', async event => {
    event.preventDefault();
    if (!state.authenticated || heartbeatBusy || !heartbeatState || heartbeatState.pending || tunnelBusy || tunnelState?.pending) return;
    const tcp = $('heartbeat-tcp').value, udp = $('heartbeat-udp').value;
    if (![tcp, udp].every(value => /^\d+$/.test(value) && Number(value) >= 10 && Number(value) <= 1800)) return message('heartbeat-message', '请输入 10–1800 秒的整数。', true);
    if (communications.activeCall?.()) return message('heartbeat-message', '请先结束电话，再修改心跳。', true);
    heartbeatBusy = true; const revision = ++heartbeatRevision; clearTimeout(heartbeatTimer); heartbeatControls();
    message('heartbeat-message', '正在提交…');
    try {
      const data = preview ? { unchanged: true } : await api('oray-heartbeat-set', { method: 'POST', body: { tcpSeconds: tcp, udpSeconds: udp }, timeout: 15000 });
      if (!state.authenticated || revision !== heartbeatRevision) return;
      heartbeatEdited = false;
      if (data.unchanged) message('heartbeat-message', preview ? '预览演示，未修改模块。' : '与当前配置相同，无需重连。');
      else {
        heartbeatState = { ...heartbeatState, pending: true };
        message('heartbeat-message', '已提交重连任务；公网连接可能短暂断开，请稍后读取设置。');
        heartbeatTimer = setTimeout(() => loadHeartbeat(1), 8000);
      }
    } catch (error) { if (state.authenticated && revision === heartbeatRevision) message('heartbeat-message', `${error.message} 请先读取设置确认结果，再决定是否重新保存。`, true); }
    finally { heartbeatBusy = false; heartbeatControls(); }
  });
  $('oray-qr').addEventListener('click', async () => {
    if (!state.authenticated || orayBusy || preview) return;
    orayBusy = true; const revision = ++orayRevision;
    clearOrayQR(); $('oray-qr').disabled = true; $('oray-check').disabled = true;
    message('oray-message', '正在生成绑定二维码…');
    try {
      const data = await api('oray-qr', { method: 'POST', body: {}, timeout: 35000 });
      if (!state.authenticated || state.page !== 'apps' || revision !== orayRevision) return;
      if (typeof data.image !== 'string' || !/^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(data.image) || !Number.isInteger(data.expiresIn) || data.expiresIn < 1 || data.expiresIn > 600) throw new Error('二维码内容无效，请重新生成。');
      orayDevice = data.deviceSN; text('oray-sn', data.deviceSN);
      $('oray-qr-image').src = data.image; $('oray-qr-panel').hidden = false;
      const expires = Date.now() + data.expiresIn * 1000;
      const tick = () => {
        const remaining = Math.max(0, Math.ceil((expires - Date.now()) / 1000));
        text('oray-qr-time', `二维码有效期：${Math.floor(remaining / 60)} 分 ${remaining % 60} 秒`);
        if (!remaining) { clearOrayQR(); message('oray-message', '二维码已过期，请点击“扫码绑定”重新生成。'); }
      };
      tick(); orayTimer = setInterval(tick, 1000);
      message('oray-message', '扫码完成后，请点击“检查绑定”。');
    } catch (error) { if (state.authenticated && revision === orayRevision) message('oray-message', error.message, true); }
    finally { orayBusy = false; if (state.authenticated) { $('oray-check').disabled = false; $('oray-qr').disabled = false; } }
  });
  $('notify-form').addEventListener('submit', async event => {
    event.preventDefault(); if (notificationBusy || !state.authenticated) return;
    notificationBusy = true; $('notify-save').disabled = true; message('notify-message', '');
    try {
      if (!preview) await api('notify-settings', { method: 'POST', body: {
        barkURL: $('notify-url').value.trim(), enabled: $('notify-enabled').checked ? '1' : '0',
        calls: $('notify-calls').checked ? '1' : '0', sms: $('notify-sms').checked ? '1' : '0', removeKey: '0'
      } });
      $('notify-url').value = ''; message('notify-message', preview ? '预览不保存实际配置。' : '提醒设置已保存。');
    } catch (error) { if (state.authenticated) message('notify-message', error.message, true); }
    finally { notificationBusy = false; $('notify-save').disabled = false; loadNotify(); }
  });
  $('notify-remove').addEventListener('click', async () => {
    if (notificationBusy || !state.authenticated) return;
    notificationBusy = true; $('notify-remove').disabled = true;
    try {
      if (!preview) await api('notify-settings', { method: 'POST', body: { barkURL: '', enabled: '0', calls: '1', sms: '1', removeKey: '1' } });
      $('notify-url').value = ''; message('notify-message', '推送地址已移除，提醒已关闭。');
    } catch (error) { if (state.authenticated) message('notify-message', error.message, true); }
    finally { notificationBusy = false; $('notify-remove').disabled = false; loadNotify(); }
  });
  $('notify-test').addEventListener('click', async () => {
    if (notificationBusy || !state.authenticated || preview) return;
    notificationBusy = true; $('notify-test').disabled = true;
    try { await api('notify-test', { method: 'POST', body: {}, timeout: 15000 }); message('notify-message', '测试提醒已发送，请查看手机通知。'); }
    catch (error) { if (state.authenticated) message('notify-message', error.message, true); }
    finally { notificationBusy = false; loadNotify(); }
  });
  $('logout-settings').addEventListener('click', logout);
  window.addEventListener('hashchange', () => { if (state.authenticated) setPage(location.hash.slice(1)); });
  document.addEventListener('visibilitychange', () => { if (!document.hidden && state.authenticated) refresh(); });
  window.addEventListener('online', () => { if (state.authenticated) refresh(); });
  async function start() {
    if (preview) {
      $('preview-banner').hidden = false;
      setAuthenticated(true, { username: 'admin', csrfToken: 'preview' }); await refresh(); return;
    }
    $('login-submit').disabled = true;
    try {
      const session = await api('session');
      if (session.authenticated === true) { setAuthenticated(true, session); await refresh(); }
      else setAuthenticated(false);
    } catch (error) {
      if (error.status !== 401 && error.status !== 403) message('login-error', error.message, true);
    } finally { $('login-submit').disabled = false; }
  }
  start();
})();
