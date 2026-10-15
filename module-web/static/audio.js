(() => {
  'use strict';
  // Tickets are sent inside the socket, never in a URL or localStorage.
  class ModuleAudio {
    constructor(api, base) {
      this.api = api; this.base = base; this.socket = null; this.epoch = 0;
      this.wanted = false; this.hadCall = false; this.pending = false;
      this.lastFailure = '';
      this.audio = new window.CallAudio(frame => {
        if (!this.socket || this.socket.readyState !== WebSocket.OPEN || this.socket.bufferedAmount > 1635) return false;
        this.socket.send(frame); return true;
      }, state => {
        const counters = document.getElementById('audio-browser-counts');
        if (counters) counters.textContent = `手机接收 ${state.down} 帧 · 手机发送 ${state.up} 帧${state.muted ? ' · 已静音' : ''}`;
        const mute = document.getElementById('audio-mute');
        if (mute) { mute.textContent = state.muted ? '取消静音' : '静音'; mute.disabled = !state.ready; }
      });
      document.getElementById('audio-output')?.addEventListener('click', () => this.showOutputs());
      document.getElementById('audio-output-select')?.addEventListener('change', event => this.changeOutput(event.target));
      document.getElementById('audio-start')?.addEventListener('click', () => this.start().catch(e => this.failure(e.message)));
      document.getElementById('audio-stop')?.addEventListener('click', () => this.stop('音频已停止；电话仍可保持接通。'));
      document.getElementById('audio-mute')?.addEventListener('click', () => this.audio.toggleMute());
      window.addEventListener('pagehide', () => this.stop());
    }
    async showOutputs() {
      const panel = document.getElementById('audio-output-panel');
      if (!panel) return;
      panel.hidden = false;
      const select = document.getElementById('audio-output-select');
      const hint = document.getElementById('audio-output-hint');
      const epoch = this.epoch;
      select.hidden = true; hint.textContent = '正在读取声音设备…';
      try {
        const options = await this.audio.outputOptions();
        if (epoch !== this.epoch || panel.hidden) return;
        select.replaceChildren();
        for (const item of options) {
          const option = document.createElement('option');
          option.value = item.deviceId; option.textContent = item.label; select.appendChild(option);
        }
        select.value = this.audio.outputDevice; select.hidden = false;
        hint.textContent = options.length > 1 ? '选择声音设备后生效；请以实际发声位置为准。' :
          'Safari 目前只提供系统默认输出，没有提供可单独选择的听筒或扬声器。';
      } catch (error) { if (epoch === this.epoch) hint.textContent = error.message; }
    }
    async changeOutput(select) {
      const epoch = this.epoch, previous = this.audio.outputDevice;
      const label = select.selectedOptions[0]?.textContent || '系统默认';
      select.disabled = true;
      const hint = document.getElementById('audio-output-hint');
      try {
        await this.audio.selectOutput(select.value, label);
        if (epoch !== this.epoch) return;
        const button = document.getElementById('audio-output');
        button.textContent = `声音输出：${label}`;
        hint.textContent = '已提交输出选择，请确认实际发声位置。';
      } catch (error) {
        if (epoch === this.epoch) { select.value = previous; hint.textContent = `切换失败：${error.message}`; }
      } finally { select.disabled = false; }
    }
    message(value) { const node = document.getElementById('audio-message'); if (node) node.textContent = value; }
    async prepare() {
      if (!window.isSecureContext) { this.message('当前 HTTP 页面仅支持电话控制；声音请使用 HTTPS 通讯地址的 /module/ 页面。'); return; }
      await this.audio.prepare(); this.wanted = true;
      this.message('麦克风已准备，接通后启动声音。');
    }
    calls(calls) {
      const live = calls.filter(c => c.state !== 9);
      if (live.length) this.hadCall = true;
      if (!live.length && this.hadCall) { this.hadCall = false; this.stop(this.lastFailure ? `电话已结束。上次音频提示：${this.lastFailure}` : '电话已结束。'); return; }
      if (calls.some(c => c.state === 3) && this.wanted && this.audio.node && !this.socket && !this.pending) {
        this.start().catch(e => this.failure(e.message));
      }
    }
    async start() {
      if (this.socket || this.pending) return;
      this.lastFailure = '';
      this.pending = true; const epoch = ++this.epoch;
      try {
        await this.audio.prepare(); if (epoch !== this.epoch) return;
        this.wanted = true; this.message('正在连接模块音频…');
        const result = await this.api('audio-ticket', { method: 'POST', body: {} });
        if (epoch !== this.epoch) return;
        if (!/^[0-9a-f]{64}$/.test(result.ticket || '')) throw new Error('音频授权返回无效');
        const url = new URL(`${this.base}/audio`, location.href); url.protocol = 'wss:';
        const ws = new WebSocket(url); ws.binaryType = 'arraybuffer'; this.socket = ws;
        this.timeout = setTimeout(() => { if (epoch === this.epoch) this.failure('模块音频启动超时，请检查语音组件是否已准备。'); }, 18000);
        ws.onopen = () => { if (epoch !== this.epoch) { ws.close(); return; } ws.send(`AUTH ${result.ticket}`); };
        ws.onmessage = event => {
          if (epoch !== this.epoch) return;
          if (event.data instanceof ArrayBuffer) { if (event.data.byteLength === 327) this.audio.receive(event.data); return; }
          let data; try { data = JSON.parse(event.data); } catch (_) { this.failure('模块返回了无效音频消息'); return; }
          if (data.type === 'ready') {
            clearTimeout(this.timeout); this.audio.start(); this.message('音频已启动。');
            this.showOutputs();
            this.heartbeat = setInterval(() => { if (ws.readyState === WebSocket.OPEN) ws.send('PING'); }, 3000);
          }
          if (data.type === 'stats') {
            const node = document.getElementById('audio-module-counts');
            if (node) node.textContent = `模块下行 ${data.downFrames} 帧 · 上行 ${data.upFrames} 帧 · 非零采样：下行 ${data.downNonzeroSamples} / 上行 ${data.upNonzeroSamples}`;
          }
        };
        ws.onclose = event => {
          if (epoch !== this.epoch) return;
          const detail = event.reason || '音频连接断开，请点“启动音频”重试。';
          if (event.code !== 1000) this.lastFailure = detail;
          this.stop(detail);
        };
        ws.onerror = () => { if (epoch === this.epoch) this.failure('无法连接模块音频，请检查 HTTPS 入口和音频服务。'); };
      } catch (e) { if (epoch === this.epoch) throw e; }
      finally { if (epoch === this.epoch) this.pending = false; }
    }
    failure(message) { this.lastFailure = message; this.stop(message); }
    stop(message) {
      this.epoch++; this.pending = false; this.wanted = false;
      clearTimeout(this.timeout); clearInterval(this.heartbeat);
      const ws = this.socket; this.socket = null;
      if (ws) { ws.onclose = null; ws.onerror = null; ws.onmessage = null; ws.close(); }
      this.audio.stop();
      const panel = document.getElementById('audio-output-panel'); if (panel) panel.hidden = false;
      const select = document.getElementById('audio-output-select'); if (select) select.hidden = true;
      const hint = document.getElementById('audio-output-hint'); if (hint) hint.textContent = '音频启动后显示可选设备。';
      const output = document.getElementById('audio-output'); if (output) output.textContent = '声音输出';
      if (message) this.message(message);
    }
  }
  window.QDCModuleAudio = ModuleAudio;
})();
