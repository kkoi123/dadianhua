const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
function setup({nativeSink = false, mediaSink = false, failPlay = false, devices = []} = {}) {
  class Media {}
  if (mediaSink) Media.prototype.setSinkId = async function(id) { this.sinkId = id; };
  class Element extends Media {
    async play() { if (failPlay) throw new Error('play denied'); this.playing = true; }
    pause() { this.playing = false; }
  }
  const env = {navigator: {mediaDevices: {enumerateDevices: async () => devices}}, console};
  env.window = {HTMLMediaElement: Media, Audio: Element}; vm.createContext(env);
  vm.runInContext(fs.readFileSync(__dirname + '/static/call-audio.js', 'utf8'), env);
  const audio = new env.window.CallAudio(() => true, () => {});
  const context = {destination: {default: true}, state: 'running', close: async () => {}, createMediaStreamDestination: () => ({stream: {}})};
  if (nativeSink) context.setSinkId = async id => { context.sinkId = id; };
  const connections = new Set([context.destination]);
  audio.context = context; audio.node = {connect: d => connections.add(d), disconnect: d => d ? connections.delete(d) : connections.clear()};
  return {audio, context, connections};
}
(async () => {
  const unsupported = setup(); await assert.rejects(unsupported.audio.outputOptions(), /不支持/);
  assert(unsupported.connections.has(unsupported.context.destination));
  const native = setup({nativeSink: true, devices: [{kind:'audiooutput', deviceId:'receiver', label:'听筒'}, {kind:'audioinput', deviceId:'mic'}]});
  const options = await native.audio.outputOptions(); assert.equal(options.length, 2); assert.equal(options[1].label, '听筒');
  await native.audio.selectOutput('receiver', '听筒'); assert.equal(native.context.sinkId, 'receiver');
  native.context.setSinkId = async () => { throw new Error('device rejected'); };
  await assert.rejects(native.audio.selectOutput('speaker', '扬声器')); assert.equal(native.audio.outputLabel, '听筒');
  const media = setup({mediaSink: true}); await media.audio.selectOutput('speaker', '扬声器');
  assert(!media.connections.has(media.context.destination)); assert(media.connections.has(media.audio.outputDestination));
  const element = media.audio.outputElement; assert(element.playing); await media.audio.selectOutput('receiver', '听筒'); assert.equal(element.sinkId, 'receiver');
  media.audio.stop(); assert(!element.playing); assert.equal(element.srcObject, null); assert.equal(media.connections.size, 0);
  const rollback = setup({mediaSink: true, failPlay: true}); await assert.rejects(rollback.audio.selectOutput('speaker','扬声器'));
  assert.equal(rollback.connections.size, 1); assert(rollback.connections.has(rollback.context.destination));
  const race = setup({nativeSink: true}); let resolve; race.context.setSinkId = () => new Promise(r => resolve = r);
  const pending = race.audio.selectOutput('speaker', '扬声器'); race.audio.stop(); resolve(); await pending; assert.equal(race.audio.outputDevice, '');
  console.log('Audio output checks passed: unsupported, native routing, Safari media routing, rejected selection, playback rollback, stop and pending-selection race.');
})().catch(e => { console.error(e); process.exitCode = 1; });
