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
let job={ok:true,state:'idle'},active=false,postDelay=null,postFail=false,posts=[];const requests=[];
const c={document,location:{hostname:'192.168.225.1',pathname:'/',hash:'#apps',search:''},history:{replaceState(){}},URLSearchParams,TextEncoder,AbortController,Date,crypto,console,
 localStorage:{getItem(k){return stored.get(k);},setItem(k,v){stored.set(k,v);},removeItem(k){stored.delete(k);}},
 setTimeout(f,ms){timers.set(++timer,{f,ms});return timer;},clearTimeout(t){timers.delete(t);},clearInterval(){},addEventListener(){},
 createQDCCommunications(){return {auth(){},page(){},refresh(){},activeCall(){return active;}};},
 async fetch(path,opt={}){requests.push(path);if(path.endsWith('/esim-operation')){const p=JSON.parse(opt.body);posts.push(p);if(postDelay)await postDelay;if(postFail)throw TypeError('offline');job={ok:true,jobId:p.requestId,state:'running'};return {ok:true,json:async()=>({ok:true,accepted:true,jobId:p.requestId})};}
 if(path.endsWith('/esim-job'))return {ok:true,json:async()=>structuredClone(job)};throw Error(path);}};
c.window=c;vm.createContext(c);vm.runInContext(fs.readFileSync(__dirname+'/static/app.js','utf8').replace('  start();','  globalThis.test={state,resetEsim,renderEsim,openEsimForm,loadEsimJob};'),c);
const t=c.test,submit=nodes.get('esim-manage-form').handlers.submit;const tick=async()=>{for(let i=0;i<12;i++)await Promise.resolve();};
(async()=>{
 assert.equal(requests.length,0);t.state.authenticated=true;t.state.csrf='fixture';t.resetEsim();t.renderEsim({state:'ready',cards:[card]});
 const buttons=nodes.get('esim-content').querySelectorAll().filter(e=>e.handlers.click);assert.equal(buttons.filter(b=>b.textContent==='删除').length,1,'active profile has no delete button');assert(buttons.some(b=>b.textContent==='下载号码配置'));
 active=true;t.openEsimForm('rename',card,card.profiles[1]);assert(nodes.get('esim-manage-form').hidden);active=false;
 t.openEsimForm('rename',card,card.profiles[1]);nodes.get('esim-name').value='中'.repeat(22);await submit({preventDefault(){}});assert.equal(posts.length,0);assert.match(nodes.get('esim-form-message').textContent,/64/);
 t.openEsimForm('download',card);nodes.get('esim-activation').value='LPA:1$example.com$secret$$1';await submit({preventDefault(){}});assert.equal(posts.length,0);assert.match(nodes.get('esim-form-message').textContent,/确认码/);
 nodes.get('esim-confirmation').value='private-confirmation';let resolve;postDelay=new Promise(r=>resolve=r);const sent=submit({preventDefault(){}});await tick();await submit({preventDefault(){}});assert.equal(posts.length,1);
 const before=requests.length;await t.loadEsimJob();assert.equal(requests.length,before,'no status race while submitting');assert.equal(nodes.get('esim-activation').value,'');assert(![...stored.values()].some(v=>v.includes('secret')||v.includes('confirmation')));
 resolve();await sent;postDelay=null;await tick();assert(nodes.get('esim-read').disabled);assert.equal(requests.filter(x=>x.endsWith('/esim-operation')).length,1);
 job={...job,state:'complete',message:'Download complete'};await t.loadEsimJob();assert(!nodes.get('esim-read').disabled);assert(buttons.every(b=>b.disabled),'must read after operation');assert.equal(timers.size,0,'no idle polling');
 t.renderEsim({state:'ready',cards:[card]});assert(buttons.every(b=>b.disabled),'old detached buttons remain stale');
 t.openEsimForm('enable',card,card.profiles[1]);postFail=true;await submit({preventDefault(){}});await tick();const n=posts.length;await t.loadEsimJob();assert.equal(posts.length,n,'lost POST response never resubmits');
 assert.match(nodes.get('esim-job').textContent,/未确认/);t.state.authenticated=false;t.resetEsim();assert(nodes.get('esim-manage-form').hidden&&nodes.get('esim-read').disabled);
 console.log('eSIM management UI passed: explicit actions, active-profile protection, byte limits, secret clearing, submission/status race, no duplicate download, reconnect and no idle polling.');
})().catch(e=>{console.error(e);process.exitCode=1;});
