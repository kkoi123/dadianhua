'use strict';
class CallAudio {
  constructor(send, report) {
    this.send = send; this.report = report; this.context = null; this.stream = null;
    this.node = null; this.active = false; this.muted = false; this.generation = 0;
    this.sequence = 0; this.down = 0; this.up = 0; this.pending = null;
    this.outputDevice = ''; this.outputLabel = '系统默认'; this.outputElement = null; this.outputDestination = null;
  }
  async prepare() {
    if (!window.isSecureContext) throw new Error('通话需要使用 HTTPS 网页地址');
    if (!navigator.mediaDevices?.getUserMedia || !window.AudioWorkletNode) throw new Error('此浏览器不支持通话音频，请用新版 Safari 打开');
    if (this.context && this.node) { await this.context.resume(); return; }
    if (this.pending) return this.pending;
    const generation = ++this.generation;
    const task = (async () => {
      let context, stream;
      try {
        context = new (window.AudioContext || window.webkitAudioContext)({latencyHint: 'interactive'});
        this.context = context;
        // Resume during the button gesture, before the permission request finishes.
        await context.resume();
        stream = await navigator.mediaDevices.getUserMedia({audio: {channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true}, video: false});
        if (generation !== this.generation) throw new Error('通话已结束');
        this.stream = stream;
        await context.audioWorklet.addModule((document.body.dataset.assetBase || '') + '/pcm-worklet.js?v=0.3.29');
        if (generation !== this.generation) throw new Error('通话已结束');
        const node = new AudioWorkletNode(context, 'module-pcm', {numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1]});
        this.node = node;
        this.source = context.createMediaStreamSource(stream);
        this.source.connect(node); node.connect(context.destination);
        this.down = 0; this.up = 0; this.sequence = 0; this.started = performance.now();
        node.port.onmessage = event => {
          if (!this.active || event.data.type !== 'uplink') return;
          const pcm = event.data.buffer;
          const frame = new ArrayBuffer(pcm.byteLength + 7), view = new DataView(frame);
          view.setUint8(0, 2); view.setUint16(1, this.sequence++ & 65535);
          view.setUint32(3, Math.floor(performance.now() - this.started) >>> 0);
          new Uint8Array(frame, 7).set(new Uint8Array(pcm));
          if (this.send(frame)) this.up++;
        };
        context.onstatechange = () => this.reportState();
        this.reportState();
      } catch (err) {
        stream?.getTracks().forEach(track => track.stop());
        if (context && context.state !== 'closed') await context.close().catch(() => {});
        if (generation === this.generation) { this.context = null; this.stream = null; this.node = null; }
        if (err.name === 'NotAllowedError') throw new Error('请允许 Safari 使用麦克风，再重试');
        throw err;
      }
    })();
    this.pending = task;
    try { await task; } finally { if (this.pending === task) this.pending = null; }
  }
  start() { if (!this.node) return false; this.active = true; this.node.port.postMessage({type: 'state', capture: true, muted: this.muted}); this.reportState(); return true; }
  receive(frame) {
    if (!this.node || !this.active || frame.byteLength <= 7 || (frame.byteLength - 7) % 2) return;
    if (new DataView(frame).getUint8(0) !== 1) return;
    const pcm = frame.slice(7); this.node.port.postMessage({type: 'pcm', buffer: pcm}, [pcm]);
    this.down++; if (this.down % 50 === 0) this.reportState();
  }
  outputSupported() {
    return Boolean(this.context && (typeof this.context.setSinkId === 'function' ||
      typeof window.HTMLMediaElement?.prototype?.setSinkId === 'function'));
  }
  async outputOptions() {
    if (!this.node || !this.context) throw new Error('请先启动音频，再选择声音输出。');
    if (!this.outputSupported() || !navigator.mediaDevices?.enumerateDevices) {
      throw new Error('当前浏览器不支持网页切换声音输出，请使用手机系统的声音输出控制。');
    }
    const devices = await navigator.mediaDevices.enumerateDevices();
    return [{deviceId: '', label: '系统默认'}, ...devices.filter(d => d.kind === 'audiooutput' && d.deviceId && d.deviceId !== 'default')
      .map((d, i) => ({deviceId: d.deviceId, label: d.label || `声音设备 ${i + 1}`}))];
  }
  async selectOutput(deviceId, label) {
    if (!this.node || !this.context) throw new Error('音频已停止，请重新启动后选择。');
    const generation = this.generation, context = this.context, node = this.node;
    if (typeof context.setSinkId === 'function') {
      await context.setSinkId(deviceId);
    } else if (typeof window.HTMLMediaElement?.prototype?.setSinkId === 'function') {
      // Safari may expose media-element device selection without WebAudio sinks.
      // Keep microphone/worklet alive; only change the remote playback destination.
      let element = this.outputElement;
      const fresh = !element;
      if (fresh) {
        element = new window.Audio(); element.autoplay = true; element.playsInline = true;
      }
      await element.setSinkId(deviceId);
      if (generation !== this.generation) { if (fresh) element.pause(); return; }
      if (fresh) {
        const destination = context.createMediaStreamDestination();
        element.srcObject = destination.stream;
        node.connect(destination);
        node.disconnect(context.destination);
        try { await element.play(); }
        catch (error) {
          node.disconnect(destination); node.connect(context.destination);
          element.srcObject = null; throw error;
        }
        if (generation !== this.generation) {
          element.pause(); element.srcObject = null; node.disconnect(destination); return;
        }
        this.outputElement = element; this.outputDestination = destination;
      }
    } else {
      throw new Error('当前浏览器不支持网页切换声音输出。');
    }
    if (generation !== this.generation) return;
    this.outputDevice = deviceId; this.outputLabel = label; this.reportState();
  }
  toggleMute() { this.muted = !this.muted; this.node?.port.postMessage({type: 'state', capture: this.active, muted: this.muted}); this.reportState(); return this.muted; }
  reportState() { this.report({running: this.active && this.context?.state === 'running', ready: Boolean(this.node), down: this.down, up: this.up, muted: this.muted}); }
  stop() {
    this.generation++; this.active = false; this.muted = false;
    this.stream?.getTracks().forEach(track => track.stop());
    this.outputElement?.pause();
    if (this.outputElement) this.outputElement.srcObject = null;
    this.outputElement = null; this.outputDestination = null;
    this.outputDevice = ''; this.outputLabel = '系统默认';
    this.source?.disconnect(); this.node?.disconnect();
    const context = this.context; if (context && context.state !== 'closed') context.close().catch(() => {});
    this.context = null; this.stream = null; this.source = null; this.node = null; this.pending = null;
    this.reportState();
  }
}
window.CallAudio = CallAudio;
