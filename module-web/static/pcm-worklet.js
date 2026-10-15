'use strict';
// The module speaks 8 kHz PCM; the iPhone audio device normally runs at 48 kHz.
class ModulePCM extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ring = new Float32Array(8000);
    this.read = 0; this.write = 0; this.available = 0;
    this.primed = false; this.playPosition = 0;
    // Ramp across packet underruns/restarts rather than stepping to silence.
    this.fadeSamples = Math.max(1, Math.round(sampleRate * 0.005));
    this.playGain = 0; this.lastOutput = 0; this.fadeOut = 0; this.fadeRemaining = 0;
    this.capture = false; this.muted = false;
    this.capturePhase = 0; this.sum = 0; this.count = 0;
    this.frame = new Int16Array(160); this.framePosition = 0;
    this.port.onmessage = event => {
      const data = event.data;
      if (data.type === 'state') { this.capture = data.capture; this.muted = data.muted; return; }
      if (data.type !== 'pcm') return;
      const view = new DataView(data.buffer);
      for (let i = 0; i + 1 < view.byteLength; i += 2) {
        if (this.available >= 1600) { this.read = (this.read + 1) % this.ring.length; this.available--; }
        this.ring[this.write] = view.getInt16(i, true) / 32768;
        this.write = (this.write + 1) % this.ring.length; this.available++;
      }
    };
  }
  process(inputs, outputs) {
    const output = outputs[0][0];
    const input = inputs[0]?.[0];
    const ratio = sampleRate / 8000;
    for (let i = 0; i < output.length; i++) {
      if (this.capture && input) {
        this.sum += this.muted ? 0 : input[i]; this.count++; this.capturePhase++;
        if (this.capturePhase >= ratio) {
          this.capturePhase -= ratio;
          const sample = Math.max(-1, Math.min(1, this.sum / this.count));
          this.frame[this.framePosition++] = Math.round(sample * (sample < 0 ? 32768 : 32767));
          this.sum = 0; this.count = 0;
          if (this.framePosition === 160) {
            // DataView explicitly writes little endian, independent of CPU endianness.
            const pcm = new ArrayBuffer(320), view = new DataView(pcm);
            for (let k = 0; k < 160; k++) view.setInt16(k * 2, this.frame[k], true);
            this.port.postMessage({type: 'uplink', buffer: pcm}, [pcm]);
            this.framePosition = 0;
          }
        }
      }
      if (!this.primed && this.available >= 320) { this.primed = true; this.playPosition = 0; this.playGain = 0; }
      if (this.primed && this.available >= 2) {
        const a = this.ring[this.read], b = this.ring[(this.read + 1) % this.ring.length];
        this.playGain = Math.min(1, this.playGain + 1 / this.fadeSamples);
        output[i] = (a + (b - a) * this.playPosition) * this.playGain;
        if (this.fadeRemaining > 0) output[i] += this.fadeOut * (--this.fadeRemaining / this.fadeSamples);
        this.playPosition += 8000 / sampleRate;
        while (this.playPosition >= 1 && this.available > 0) {
          this.playPosition--; this.read = (this.read + 1) % this.ring.length; this.available--;
        }
      } else {
        if (this.primed) { this.primed = false; this.fadeOut = this.lastOutput; this.fadeRemaining = this.fadeSamples; }
        output[i] = this.fadeRemaining > 0 ? this.fadeOut * (--this.fadeRemaining / this.fadeSamples) : 0;
      }
      this.lastOutput = output[i];
    }
    return true;
  }
}
registerProcessor('module-pcm', ModulePCM);
