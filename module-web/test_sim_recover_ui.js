/* Exercise actual management form/status code, no hardware or network. */
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),crypto=require('node:crypto').webcrypto;
class E {
 constructor(){this.handlers={};this.children=[];this.style={};this.value='';this.textContent='';this.classList={add(){},remove(){},toggle(){}};}
 addEventListener(n,f){this.handlers[n]=f;}setAttribute(){}removeAttribute(){}append(...v){this.children.push(...v);}replaceChildren(...v){this.children=[...v];}
 querySelectorAll(){return this.children.flatMap(c=>[c,...c.querySelectorAll()]);}reset(){for(const id of ['esim-name','esim-activation','esim-confirmation','esim-imei'])document.getElementById(id).value='';}
 get firstElementChild(){return this.children[0]||(this.child ||=new E());}get lastElementChild(){return this.firstElementChild;}
}
const nodes=new Map(),stored=new Map(),timers=new Map();let timer=0;
const document={body:{dataset:{}},getElementById(id){if(!nodes.has(id))nodes.set(id,new E());return nodes.get(id);},querySelectorAll(){return[];},addEventListener(){},createElement(){return new E();},createDocumentFragment(){return new E();}};
const card={aid:'a06573746b6d65ffff4953442d522030',eid:'89000000000000000000000000000001',profiles:[{iccid:'89860000000000000001',name:'Active',provider:'Test',enabled:true},{iccid:'89860000000000000002',name:'Other',provider:'Test',enabled:false}]};
let recoverable=false,recoverPosts=[],recoverDelay=null;let pinRequired=true,pinUnavailable=false,pinFailure=false,pinDelay=null,pinPosts=[];
let job={ok:true,state:'idle'},active=false,postDelay=null,postFail=false,posts=[];const requests=[];
const c={document,location:{hostname:'192.168.225.1',pathname:'/',hash:'#apps',search:''},history:{replaceState(){}},URLSearchParams,TextEncoder,AbortController,Date,crypto,console,
 localStorage:{getItem(k){return stored.get(k);},setItem(k,v){stored.set(k,v);},removeItem(k){stored.delete(k);}},
 setTimeout(f,ms){timers.set(++timer,{f,ms});return timer;},clearTimeout(t){timers.delete(t);},clearInterval(){},addEventListener(){},
 createQDCCommunications(){return {auth(){},page(){},refresh(){},activeCall(){return active;}};},
 async fetch(path,opt={}){requests.push(path);
 if(path.endsWith('/sim-pin-state'))return {ok:true,json:async()=>({ok:true,state:pinUnavailable?'SIM 读取错误':pinRequired?'需要 PIN 码':'PIN 已验证',required:pinRequired&&!pinUnavailable,retries:pinUnavailable?null:3,recoverable,stateToken:'00'.repeat(29)})};
 if(path.endsWith('/sim-recover')){recoverPosts.push(JSON.parse(opt.body));if(recoverDelay)await recoverDelay;throw TypeError('offline');}
 if(path.endsWith('/sim-pin-verify')){pinPosts.push(JSON.parse(opt.body));if(pinDelay)await pinDelay;if(pinFailure)throw TypeError('offline');return {ok:true,json:async()=>({ok:true,message:'PIN 已验证'})};}
 if(path.endsWith('/esim-operation')){const p=JSON.parse(opt.body);posts.push(p);if(postDelay)await postDelay;if(postFail)throw TypeError('offline');job={ok:true,jobId:p.requestId,state:'running'};return {ok:true,json:async()=>({ok:true,accepted:true,jobId:p.requestId})};}
 if(path.endsWith('/esim-job'))return {ok:true,json:async()=>structuredClone(job)};throw Error(path);}};
c.window=c;vm.createContext(c);vm.runInContext(fs.readFileSync(__dirname+'/static/app.js','utf8').replace('  start();','  globalThis.test={state,resetEsim,renderEsim,openEsimForm,loadEsimJob,loadPinStatus};'),c);
const t=c.test,submit=nodes.get('esim-manage-form').handlers.submit;const tick=async()=>{for(let i=0;i<12;i++)await Promise.resolve();};
(async()=>{
 t.state.authenticated=true;t.state.csrf='fixture';t.resetEsim();pinRequired=false;
 await t.loadPinStatus();assert(nodes.get('sim-recover-notice').hidden);assert(nodes.get('sim-recover-submit').disabled);
 recoverable=true;pinUnavailable=true;await t.loadPinStatus();assert(!nodes.get('sim-recover-notice').hidden);assert(!nodes.get('sim-recover-submit').disabled);assert(nodes.get('sim-pin-submit').hidden);
 const submit=nodes.get('sim-recover-submit').handlers.click;active=true;await submit();assert.equal(recoverPosts.length,0);active=false;
 let done;recoverDelay=new Promise(r=>done=r);const pending=submit();await tick();await submit();assert.equal(recoverPosts.length,1);assert(nodes.get('sim-pin-open').disabled);assert(nodes.get('esim-read').disabled);
 done();await pending;recoverDelay=null;await submit();assert.equal(recoverPosts.length,1);assert.match(nodes.get('sim-pin-message').textContent,/刷新状态/);assert(nodes.get('sim-recover-submit').disabled);
 recoverable=false;pinUnavailable=false;pinRequired=true;await t.loadPinStatus();assert(nodes.get('sim-recover-notice').hidden);assert(!nodes.get('sim-pin-submit').hidden);
 assert(![...stored.values()].some(v=>/stateToken/.test(v)));assert.equal(timers.size,0);
 console.log('SIM recovery UI passed: error-only confirmation, active-call exclusion, busy/duplicate gates, uncertain result requires manual refresh and PIN separation.');
})().catch(e=>{console.error(e);process.exitCode=1;});
