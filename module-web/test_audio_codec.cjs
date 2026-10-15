const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync(__dirname + '/static/pcm-worklet.js', 'utf8');
for (const rate of [44100, 48000]) {
  let Processor; const sent = [];
  class AudioWorkletProcessor { constructor() { this.port = {postMessage: value => sent.push(value)}; } }
  vm.runInNewContext(source, {AudioWorkletProcessor, sampleRate: rate, Float32Array, Int16Array, DataView, ArrayBuffer, Math, registerProcessor: (name, ctor) => { Processor = ctor; }});
  const p = new Processor();
  p.port.onmessage({data: {type: 'state', capture: true, muted: false}});
  const samples = 128 * Math.ceil(rate / 128);
  for (let i = 0; i < samples / 128; i++) p.process([[new Float32Array(128).fill(.25)]], [[new Float32Array(128)]]);
  assert.ok(Math.abs(sent.length - Math.floor(samples * 8000 / rate / 160)) <= 1, '8 kHz uplink cadence');
  for (const frame of sent) { assert.equal(frame.buffer.byteLength, 320); assert.equal(new DataView(frame.buffer).getInt16(0, true), 8192); }
  const down = new ArrayBuffer(3200), view = new DataView(down);
  for (let i = 0; i < 1600; i++) view.setInt16(i * 2, 16384, true);
  p.port.onmessage({data: {type: 'pcm', buffer: down}});
  const output = new Float32Array(128); p.process([[new Float32Array(128)]], [[output]]);
  assert.ok(output[0] > 0 && output[0] < .01, 'playback starts with a ramp');
  for (let i = 0; i < 3; i++) p.process([[new Float32Array(128)]], [[output]]);
  assert.ok(output.every(value => Math.abs(value - .5) < 1e-6), 'downlink playback resampling after ramp');
  p.port.onmessage({data: {type: 'pcm', buffer: down}});
  assert.ok(p.available <= 1600, 'downlink latency bounded');
  sent.length = 0; p.port.onmessage({data: {type: 'state', capture: true, muted: true}});
  for (let i = 0; i < 20; i++) p.process([[new Float32Array(128).fill(.5)]], [[new Float32Array(128)]]);
  assert.ok(sent.length > 0 && new Int16Array(sent.at(-1).buffer).every(value => value === 0), 'mute produces silent PCM');
}
// Simulate actual packet arrivals at normal cadence and at the previously
// observed slow cadence. A constant signal makes discontinuities measurable.
for (const rate of [44100, 48000]) for (const fps of [50, 46.45]) {
  let Processor; class AudioWorkletProcessor { constructor() { this.port = {postMessage() {}}; } }
  vm.runInNewContext(source, {AudioWorkletProcessor, sampleRate: rate, Float32Array, Int16Array, DataView, ArrayBuffer, Math, registerProcessor: (name, ctor) => { Processor = ctor; }});
  const p = new Processor(), frame = new ArrayBuffer(320), view = new DataView(frame);
  for (let i=0;i<160;i++) view.setInt16(i*2,16384,true);
  let next=0, previous=0, maxJump=0, zeros=0, samples=0;
  for (let block=0;block<rate*10/128;block++) {
    const t=block*128/rate;
    while(next<=t) { p.port.onmessage({data:{type:'pcm',buffer:frame}}); next+=1/fps; }
    const output = new Float32Array(128); p.process([[new Float32Array(128)]],[[output]]);
    for (const x of output) {
      assert.ok(Number.isFinite(x) && Math.abs(x)<=.50001);
      if(t>.1) { maxJump=Math.max(maxJump,Math.abs(x-previous)); zeros+=x===0; samples++; }
      previous=x;
    }
  }
  assert.ok(maxJump < .003, `underrun/restart must fade, rate=${rate} fps=${fps} jump=${maxJump}`);
  if (fps===50) assert.equal(zeros,0,'steady real-time packets must not underrun');
  else assert.ok(zeros>0 && zeros<samples*.1,'slow packets should still rebuffer safely');
}
console.log('音频转换检查通过：44.1/48 kHz ↔ 8 kHz、静音、缓冲上限、正常节奏无断音、欠载/恢复平滑');
