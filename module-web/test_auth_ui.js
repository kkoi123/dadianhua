/* Reproduce the measured 13-second login and two-hash password change without
 * sleeping or reducing password strength. Other requests remain bounded. */
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
class E{constructor(){this.classList={toggle(){},add(){},remove(){}};this.style={};this.handlers={};this.textContent='';}get firstElementChild(){return this.child||(this.child=new E());}get lastElementChild(){return this.firstElementChild;}addEventListener(k,f){this.handlers[k]=f;}setAttribute(){}removeAttribute(){}append(){}replaceChildren(){}reset(){}}
const nodes=new Map(),doc={body:{dataset:{}},hidden:false,getElementById(id){if(!nodes.has(id))nodes.set(id,new E());return nodes.get(id);},querySelectorAll(){return[];},addEventListener(){},createElement(){return new E();},createDocumentFragment(){return new E();}};
let now=0,timers=[],delay=13000,status=200,requests=[];
function schedule(f,t){const token={f,at:now+t,cancelled:false,delay:t};timers.push(token);return token;}
function clear(t){if(t)t.cancelled=true;}
async function advance(ms){const end=now+ms;while(true){const next=timers.filter(t=>!t.cancelled&&t.at<=end).sort((a,b)=>a.at-b.at)[0];if(!next)break;now=next.at;next.cancelled=true;next.f();for(let i=0;i<12;i++)await Promise.resolve();}now=end;for(let i=0;i<12;i++)await Promise.resolve();}
const context={document:doc,location:{hostname:'module.example.test',search:'',hash:'#home',pathname:'/module/'},history:{replaceState(){}},URLSearchParams,AbortController,TextEncoder,Date,console,clearInterval(){},setTimeout:schedule,clearTimeout:clear,addEventListener(){},createQDCCommunications(){return{auth(){},page(){},refresh(){}};},
 fetch(path,options){requests.push({path,options});return new Promise((resolve,reject)=>{const token=schedule(()=>{options.signal.removeEventListener('abort',abort);resolve({ok:status===200,status,json:async()=>status===200?{ok:true,csrfToken:'fixture'}:{ok:false,error:'用户名或密码错误'}});},delay);function abort(){clear(token);const e=new Error('aborted');e.name='AbortError';reject(e);}options.signal.addEventListener('abort',abort,{once:true});});}};
context.window=context;vm.createContext(context);vm.runInContext(fs.readFileSync(__dirname+'/static/app.js','utf8').replace('  start();','  globalThis.testApi=api; globalThis.testState=state;'),context);
let finished=false;process.on('beforeExit',()=>{if(!finished){console.error('Auth UI did not finish');process.exitCode=1;}});
(async()=>{
 let p=context.testApi('login',{method:'POST',body:{username:'admin',password:'fixture'}});
 assert(timers.some(t=>t.delay===35000));await advance(13000);assert.equal((await p).ok,true,'13-second login must finish');assert(requests[0].options.credentials==='same-origin');
 delay=27000;p=context.testApi('password',{method:'POST',body:{currentPassword:'fixture',newPassword:'fixture2'}});await advance(27000);assert.equal((await p).ok,true,'two password hashes must have sufficient time');
 delay=13000;p=context.testApi('traffic');const failed=assert.rejects(p,/请求超时/);await advance(10000);await failed;
 p=context.testApi('notify-test',{method:'POST',body:{},timeout:15000});await advance(13000);assert.equal((await p).ok,true,'explicit action timeout is retained');
 delay=1000;status=401;p=context.testApi('login',{method:'POST',body:{username:'admin',password:'wrong'}});const wrong=assert.rejects(p,/用户名或密码错误/);await advance(1000);await wrong;
 context.testState.authenticated=true;status=403;p=context.testApi('notify-settings',{method:'POST',body:{}});let rejected=assert.rejects(p);await advance(1000);await rejected;assert(context.testState.authenticated,'403 must retain login');
 status=401;p=context.testApi('session');rejected=assert.rejects(p);await advance(1000);await rejected;assert(!context.testState.authenticated,'401 must return to login');
 status=200;doc.getElementById('login-username').value='admin';doc.getElementById('login-password').value='fixture';doc.getElementById('login-remember').checked=true;context.location.hash='#settings';
 p=doc.getElementById('login-form').handlers.submit({preventDefault(){}});await advance(15000);await p;assert.equal(JSON.parse(requests.findLast(r=>r.path.endsWith('/login')).options.body).remember,'1');
 assert(!timers.some(t=>!t.cancelled),'completed requests must clear their timers');
 finished=true;console.log('Auth UI passed: delayed login/password success, ordinary timeout, explicit deadline, wrong-password error and timer cleanup.');
})().catch(e=>{console.error(e);process.exit(1);});
