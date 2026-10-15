/* Exercise actual management form/status code, no hardware or network. */
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),crypto=require('node:crypto').webcrypto;
class E {
 constructor(){this.handlers={};this.children=[];this.style={};this.value='';this.textContent='';this.classList={add(){},remove(){},toggle(){}};}
 addEventListener(n,f){this.handlers[n]=f;}setAttribute(k,v){(this.attrs ||= {})[k]=v;}removeAttribute(k){delete (this.attrs ||= {})[k];}focus(){this.focused=true;}append(...v){this.children.push(...v);}replaceChildren(...v){this.children=[...v];}
 querySelectorAll(){return this.children.flatMap(c=>[c,...c.querySelectorAll()]);}reset(){for(const id of ['esim-name','esim-activation','esim-confirmation','esim-imei'])document.getElementById(id).value='';}
 get firstElementChild(){return this.children[0]||(this.child ||=new E());}get lastElementChild(){return this.firstElementChild;}
}
const nodes=new Map(),stored=new Map(),timers=new Map();let timer=0;
const document={body:{dataset:{}},getElementById(id){if(!nodes.has(id))nodes.set(id,new E());return nodes.get(id);},querySelectorAll(){return[];},addEventListener(){},createElement(){return new E();},createDocumentFragment(){return new E();}};
const card={aid:'a06573746b6d65ffff4953442d522030',eid:'89000000000000000000000000000001',profiles:[{iccid:'89860000000000000001',name:'Active',provider:'Test',enabled:true},{iccid:'89860000000000000002',name:'Other',provider:'Test',enabled:false}]};
let pinRequired=true,pinUnavailable=false,pinFailure=false,pinDelay=null,pinPosts=[];
let job={ok:true,state:'idle'},active=false,postDelay=null,postFail=false,posts=[];const requests=[];
const c={document,location:{hostname:'192.168.225.1',pathname:'/',hash:'#apps',search:''},history:{replaceState(){}},URLSearchParams,TextEncoder,AbortController,Date,crypto,console,
 localStorage:{getItem(k){return stored.get(k);},setItem(k,v){stored.set(k,v);},removeItem(k){stored.delete(k);}},
 setTimeout(f,ms){timers.set(++timer,{f,ms});return timer;},clearTimeout(t){timers.delete(t);},clearInterval(){},addEventListener(){},
 createQDCCommunications(){return {auth(){},page(){},refresh(){},activeCall(){return active;}};},
 async fetch(path,opt={}){requests.push(path);
 if(path.endsWith('/sim-pin-state'))return {ok:true,json:async()=>({ok:true,state:pinUnavailable?'SIM 读取错误':pinRequired?'需要 PIN 码':'PIN 已验证',required:pinRequired&&!pinUnavailable,retries:pinUnavailable?null:3,stateToken:'00'.repeat(29)})};
 if(path.endsWith('/sim-pin-verify')){pinPosts.push(JSON.parse(opt.body));if(pinDelay)await pinDelay;if(pinFailure)throw TypeError('offline');return {ok:true,json:async()=>({ok:true,message:'PIN 已验证'})};}
 if(path.endsWith('/esim-operation')){const p=JSON.parse(opt.body);posts.push(p);if(postDelay)await postDelay;if(postFail)throw TypeError('offline');job={ok:true,jobId:p.requestId,state:'running'};return {ok:true,json:async()=>({ok:true,accepted:true,jobId:p.requestId})};}
 if(path.endsWith('/esim-job'))return {ok:true,json:async()=>structuredClone(job)};return {ok:true,json:async()=>({ok:true,pending:false,configured:false,enabled:false})};}};
c.window=c;vm.createContext(c);vm.runInContext(fs.readFileSync(__dirname+'/static/app.js','utf8').replace('  start();','  globalThis.test={state,resetEsim,renderEsim,openEsimForm,loadEsimJob,loadPinStatus,setApplicationTab};'),c);
const t=c.test,submit=nodes.get('esim-manage-form').handlers.submit;const tick=async()=>{for(let i=0;i<12;i++)await Promise.resolve();};
(async()=>{
 t.state.authenticated=true;t.state.page='apps';t.resetEsim();
 for(const name of ['esim','oray','bark']) {
   requests.length=0;t.setApplicationTab(name);await tick();
   for(const other of ['esim','oray','bark']) {
     assert.equal(nodes.get('apps-panel-'+other).hidden,other!==name);
     assert.equal(nodes.get('apps-tab-'+other).attrs['aria-selected'],String(other===name));
     assert.equal(nodes.get('apps-tab-'+other).attrs.tabindex,other===name?'0':'-1');
   }
   if(name==='esim') assert.deepEqual(requests,['/cgi-bin/api/esim-job']);
   if(name==='oray') {assert(requests.every(p=>/\/(tunnel|oray-status|oray-heartbeat)$/.test(p)));assert.equal(requests.length,3);}
   if(name==='bark') assert.deepEqual(requests,['/cgi-bin/api/notify']);
 }
 nodes.get('apps-tab-bark').handlers.keydown({key:'Home',preventDefault(){}});await tick();
 assert(!nodes.get('apps-panel-esim').hidden);assert(nodes.get('apps-tab-esim').focused);
 nodes.get('apps-tab-esim').handlers.keydown({key:'ArrowLeft',preventDefault(){}});await tick();
 assert(!nodes.get('apps-panel-bark').hidden);
 assert.equal(posts.length,0);assert.equal(pinPosts.length,0);
 const html=fs.readFileSync(__dirname+'/static/index.html','utf8');
 assert(html.indexOf('id="sim-pin-form"')<html.indexOf('id="page-apps"'));
 assert.equal((html.match(/id="sim-pin-form"/g)||[]).length,1);
 console.log('Applications UI passed: exclusive panels, current application requests only, keyboard navigation, no card writes, independent network PIN entry.');
})().catch(e=>{console.error(e);process.exitCode=1;});
