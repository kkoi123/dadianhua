const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const sources=[],timeouts=[];let changes=[],statuses=[],expired=0;
class ES{constructor(url){this.url=url;this.handlers={};this.closed=false;sources.push(this);}addEventListener(n,f){this.handlers[n]=f;}close(){this.closed=true;}emit(n,data){this.handlers[n]?.({data:JSON.stringify(data)});}}
let active=false;const listeners={};const document={hidden:false,addEventListener(n,f){listeners[n]=f;}};
const c={document,EventSource:ES,setTimeout(f,t){const v={f,t};timeouts.push(v);return v;},clearTimeout(v){if(v)v.cancelled=true;},addEventListener(){},console};c.window=c;vm.createContext(c);vm.runInContext(fs.readFileSync(__dirname+'/static/events.js','utf8'),c);
const events=new c.QDCModuleEvents('/module',{state(d,newSMS){changes.push(newSMS);},status(s){statuses.push(s);},activeCall(){return active;},reconnected(){},expired(){expired++;}});
events.auth(true);const first=sources[0];assert.equal(first.url,'/module/cgi-bin/api/events');
const state={ok:true,epoch:'boot1',callRevision:1,smsRevision:1};first.emit('state',state);assert.deepEqual(changes,[false]);assert.equal(timeouts.length,0,'idle must not schedule HTTP polls');
first.emit('state',{...state,smsRevision:2});assert.deepEqual(changes,[false,true]);
first.emit('state',{...state,epoch:'boot2'});assert.equal(changes.at(-1),false,'restart must not alert old messages');
document.hidden=true;listeners.visibilitychange();assert(first.closed);assert.equal(events.source,null);
document.hidden=false;listeners.visibilitychange();const second=sources[1];second.onerror();assert.equal(timeouts.at(-1).t,10000);timeouts.at(-1).f();const third=sources[2];third.onerror();assert.equal(timeouts.at(-1).t,20000);
events.auth(false);assert(timeouts.at(-1).cancelled);assert.equal(events.source,null);
events.auth(true);const last=sources.at(-1);last.emit('expired',{});assert.equal(expired,1);assert(last.closed);
const app=fs.readFileSync(__dirname+'/static/app.js','utf8');assert.deepEqual(app.match(/setInterval\([^;]+/g),['setInterval(tick, 1000)']); // local QR expiry countdown, no network polls
assert(!fs.readFileSync(__dirname+'/static/communications.js','utf8').includes('setInterval('));
console.log('Event UI passed: no idle polling, SMS changes/restart baseline, background close, reconnect backoff and session expiry.');
