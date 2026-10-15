const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const nodes=new Map();const makeNode=()=>({textContent:'',hidden:false,children:[],addEventListener(){},replaceChildren(){this.children=[];},appendChild(x){this.children.push(x);}});const doc={createElement:makeNode,getElementById(id){if(!nodes.has(id))nodes.set(id,makeNode());return nodes.get(id);}};
const sockets=[];
class WS{static OPEN=1;constructor(url){this.url=url;this.readyState=1;this.bufferedAmount=0;this.sent=[];sockets.push(this);}send(x){this.sent.push(x);}close(){this.closed=true;}}
class Audio{constructor(send,report){this.send=send;this.report=report;this.node=null;}async prepare(){this.node={};}start(){this.active=true;}receive(x){this.last=x;}async outputOptions(){return [{deviceId:'',label:'系统默认'},{deviceId:'speaker',label:'扬声器'}];}toggleMute(){}stop(){this.node=null;this.active=false;}}
const context={document:doc,location:{href:'https://example.test/module/'},URL,WebSocket:WS,CallAudio:Audio,ArrayBuffer,Uint8Array,JSON,Error,setInterval(){return 1;},clearInterval(){},setTimeout(){return 2;},clearTimeout(){},addEventListener(){},isSecureContext:true};context.window=context;vm.createContext(context);vm.runInContext(fs.readFileSync(__dirname+'/static/audio.js','utf8'),context);
(async()=>{
 let resolveTicket,called=0;
 const audio=new context.QDCModuleAudio(async(name,options)=>{called++;assert.equal(name,'audio-ticket');assert.equal(options.method,'POST');return {ticket:'a'.repeat(64)};},'/module');
 await audio.prepare();await audio.start();const socket=sockets.at(-1);assert.equal(socket.url.href,'wss://example.test/module/audio');socket.onopen();assert.equal(socket.sent[0],'AUTH '+'a'.repeat(64));socket.onmessage({data:'{"type":"ready"}'});assert(audio.audio.active);await new Promise(r=>setImmediate(r));assert.equal(nodes.get('audio-output-panel').hidden,false);assert.equal(nodes.get('audio-output-select').children.length,2,'connected audio must show outputs without another click');
 socket.onmessage({data:'{"type":"stats","upFrames":2,"downFrames":3,"upNonzeroSamples":4,"downNonzeroSamples":5}'});assert.match(nodes.get('audio-module-counts').textContent,/下行 3 帧/);
 audio.calls([{state:3}]);audio.calls([]);assert(socket.closed&&!audio.audio.node);assert.match(nodes.get('audio-message').textContent,/结束/);
 await audio.prepare();await audio.start();const failedSocket=sockets.at(-1);audio.calls([{state:3}]);
 failedSocket.onclose({code:4000,reason:'模块未返回音频数据'});audio.calls([]);
 assert.match(nodes.get('audio-message').textContent,/上次音频提示：模块未返回音频数据/,'hangup must preserve capture failure');
 const before=sockets.length;const race=new context.QDCModuleAudio(()=>new Promise(r=>resolveTicket=r),'/module');const pending=race.start();await new Promise(r=>setImmediate(r));race.stop();resolveTicket({ticket:'b'.repeat(64)});await pending;assert.equal(sockets.length,before,'logout during ticket request must not open socket');
 context.isSecureContext=false;await audio.prepare();assert.match(nodes.get('audio-message').textContent,/HTTP/);assert.equal(called,2);
 console.log('Audio UI integration passed: secure module prefix, ticket in message, readiness, counters, call-end cleanup, logout race, HTTP guidance.');
})().catch(e=>{console.error(e);process.exitCode=1;});
