/* Real page handlers: fake network only, no actual data disconnects. */
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
class E{constructor(){this.classList={toggle(){},add(){},remove(){}};this.style={};this.handlers={};this.attrs={};this.textContent='';}get firstElementChild(){return this.child||(this.child=new E());}get lastElementChild(){return this.firstElementChild;}addEventListener(k,f){this.handlers[k]=f;}setAttribute(k,v){this.attrs[k]=v;}removeAttribute(){}append(){}replaceChildren(){}reset(){}querySelectorAll(){return[];}}
const nodes=new Map(),doc={body:{dataset:{}},hidden:false,getElementById(id){if(!nodes.has(id))nodes.set(id,new E());return nodes.get(id);},querySelectorAll(){return[];},addEventListener(){},createElement(){return new E();},createDocumentFragment(){return new E();}};
let confirm=true,active=false,data={enabled:true,connected:true,usbEnabled:true,usbForwarding:true,usbConnected:true,pending:false},posts=[],releasePost=null,holdPost=false,holdGet=false,releaseGet=null,fail=false;
const context={document:doc,location:{hostname:'192.168.225.1',search:'',hash:'#network',pathname:'/'},history:{replaceState(){}},URLSearchParams,AbortController,TextEncoder,Date,console,setInterval(){return 1;},clearInterval(){},setTimeout,clearTimeout,addEventListener(){},confirm(){return confirm;},createQDCCommunications(){return{auth(){},page(){},refresh(){},activeCall(){return active;}};},
 async fetch(path,options){let result;
  if(path.endsWith('/session'))result={authenticated:true,csrfToken:'test-csrf'};
  else if(path.endsWith('/status'))result={storage:[],network:{connected:true}};
  else if(path.endsWith('/radio'))result={network:{}};
  else if(path.endsWith('/traffic'))result={receivedBytes:0,sentBytes:0,since:1,interfaces:1};
  else if(path.endsWith('/data')){result={...data};if(holdGet)await new Promise(r=>releaseGet=r);}
  else if(path.endsWith('/data-set')){posts.push(options);if(holdPost)await new Promise(r=>releasePost=r);if(fail)return {ok:false,status:409,async json(){return{ok:false,error:'failed switch'};}};const body=JSON.parse(options.body);result=body.scope==='roaming'?{...data,roamingEnabled:body.enabled==='1',pending:true}:body.scope==='compat'?{...data,compatEnabled:body.enabled==='1',compatApplied:body.enabled==='1',pending:false}:{...data,enabled:body.scope==='usb'?data.enabled:body.enabled==='1',usbEnabled:body.scope==='usb'?body.enabled==='1':data.usbEnabled,pending:true,accepted:true};}
  else throw Error('Unexpected request '+path);
  return{ok:true,async json(){return result;}};
 }};context.window=context;vm.createContext(context);
vm.runInContext(fs.readFileSync(__dirname+'/static/app.js','utf8').replace('  start();','  globalThis.ready=start();globalThis.loadDataPage=loadData;'),context);
(async()=>{
 await context.ready;const button=nodes.get('data-toggle');assert.equal(button.attrs['aria-checked'],'true');assert.equal(button.disabled,false);
 confirm=false;await button.handlers.click();assert.equal(posts.length,0);
 confirm=true;active=true;await button.handlers.click();assert.equal(posts.length,0);assert.match(nodes.get('data-message').textContent,/先结束电话/);active=false;
 holdGet=true;const oldRead=context.loadDataPage();while(!releaseGet)await new Promise(r=>setTimeout(r,1));
 holdPost=true;const closing=button.handlers.click();while(!releasePost)await new Promise(r=>setTimeout(r,1));await button.handlers.click();assert.equal(posts.length,1);assert.equal(button.disabled,true);
 releasePost();await closing;assert.equal(JSON.parse(posts[0].body).csrfToken,'test-csrf');assert.equal(JSON.parse(posts[0].body).enabled,'0');assert.equal(posts[0].method,'POST');assert.equal(button.attrs['aria-checked'],'false');assert.match(nodes.get('data-message').textContent,/USB/);
 releaseGet();await oldRead;assert.equal(button.attrs['aria-checked'],'false','stale GET cannot undo the pending switch');
 holdGet=false;holdPost=false;data={enabled:false,connected:false,usbEnabled:true,usbForwarding:true,usbConnected:false,pending:false,lastSuccess:true};await context.loadDataPage();assert.equal(button.disabled,false);assert.match(nodes.get('network-badge').lastElementChild.textContent,/未连接/);
 confirm=false;await button.handlers.click();assert.equal(posts.length,2,'turn-on does not ask for disconnect confirmation');assert.equal(JSON.parse(posts[1].body).enabled,'1');
 data={enabled:true,connected:true,usbEnabled:true,usbForwarding:true,usbConnected:true,pending:false,lastSuccess:true};await context.loadDataPage();fail=true;confirm=true;await button.handlers.click();assert.equal(button.attrs['aria-checked'],'true');assert.match(nodes.get('data-message').textContent,/未确认/);assert.equal(button.disabled,false);
 data={enabled:false,connected:true,usbEnabled:true,usbForwarding:true,usbConnected:true,pending:false,lastSuccess:false};await context.loadDataPage();assert.equal(button.attrs['aria-checked'],'false');assert.match(nodes.get('data-message').textContent,/尚未确认/);assert.match(nodes.get('network-badge').lastElementChild.textContent,/已连接/);
 data={enabled:false,connected:true,usbEnabled:true,usbForwarding:true,usbConnected:true,pending:false,lastSuccess:true};await context.loadDataPage();assert.match(nodes.get('data-toggle-label').textContent,/关闭未完成/);assert.match(nodes.get('data-message').textContent,/尚未确认/);
 // USB-only action is independently scoped; no cellular disconnect dialog.
 fail=false;active=true;confirm=false;
 data={enabled:true,connected:true,usbEnabled:true,usbForwarding:true,usbConnected:true,pending:false,lastSuccess:true,usbLastSuccess:true};await context.loadDataPage();
 const usb=nodes.get('usb-toggle');assert.equal(usb.attrs['aria-checked'],'true');assert.equal(usb.disabled,false);
 const before=posts.length;await usb.handlers.click();assert.equal(posts.length,before+1);assert.equal(JSON.parse(posts.at(-1).body).scope,'usb');assert.equal(JSON.parse(posts.at(-1).body).enabled,'0');assert.equal(button.attrs['aria-checked'],'true');assert.equal(usb.attrs['aria-checked'],'false');assert.match(nodes.get('data-message').textContent,/本地管理/);
 data={enabled:true,connected:true,usbEnabled:false,usbForwarding:false,usbConnected:false,pending:false,lastSuccess:true,usbLastSuccess:true};await context.loadDataPage();assert.equal(usb.disabled,false);assert.equal(button.disabled,false);assert.match(nodes.get('network-badge').lastElementChild.textContent,/已连接/);
 data={enabled:false,connected:false,usbEnabled:true,usbForwarding:true,usbConnected:false,pending:false,lastSuccess:true,usbLastSuccess:true};await context.loadDataPage();assert.match(nodes.get('data-message').textContent,/需先开启蜂窝/);
 data.usbForwarding=false;await context.loadDataPage();assert.match(nodes.get('data-message').textContent,/尚未确认/);
 // Opt-in roaming compatibility is independently scoped and reflects runtime mismatch.
 data={enabled:true,connected:true,usbEnabled:true,usbForwarding:true,pending:false,compatEnabled:false,compatApplied:false,compatSupported:true};await context.loadDataPage();
 const compat=nodes.get('compat-toggle');assert.equal(compat.disabled,false);assert.equal(compat.attrs['aria-checked'],'false');
 await compat.handlers.click();assert.equal(JSON.parse(posts.at(-1).body).scope,'compat');assert.equal(compat.attrs['aria-checked'],'true');assert.equal(button.attrs['aria-checked'],'true');
 data.compatEnabled=true;data.compatApplied=false;await context.loadDataPage();assert.match(nodes.get('compat-toggle-label').textContent,/未完成/);
 data.compatSupported=false;await context.loadDataPage();assert.equal(compat.disabled,true);assert.match(nodes.get('compat-toggle-label').textContent,/不可用/);
 // Genuine data-roaming permission is distinct from USB compatibility.
 active=false;confirm=true;data={enabled:true,connected:true,usbEnabled:true,usbForwarding:true,pending:false,roamingEnabled:true,roamingApplied:true,roaming:true,effectiveEnabled:true,roamingBlocked:false};await context.loadDataPage();
 const roam=nodes.get('roaming-toggle');assert.equal(roam.disabled,false);assert.equal(roam.attrs['aria-checked'],'true');assert.equal(nodes.get('net-roaming').textContent,'正在漫游');
 confirm=false;const n=posts.length;await roam.handlers.click();assert.equal(posts.length,n,'Roaming OFF requires disconnect confirmation');
 confirm=true;active=true;await roam.handlers.click();assert.equal(posts.length,n,'Roaming OFF preserves calls');
 active=false;await roam.handlers.click();assert.equal(JSON.parse(posts.at(-1).body).scope,'roaming');assert.equal(JSON.parse(posts.at(-1).body).enabled,'0');
 data={...data,roamingEnabled:false,roamingApplied:false,connected:false,effectiveEnabled:false,roamingBlocked:true};await context.loadDataPage();assert.equal(button.attrs['aria-checked'],'true');assert.equal(roam.attrs['aria-checked'],'false');assert.match(nodes.get('data-message').textContent,/当前漫游网络/);assert.equal(nodes.get('data-toggle-label').textContent,'蜂窝联网 · 开');
 confirm=false;await roam.handlers.click();assert.equal(JSON.parse(posts.at(-1).body).enabled,'1','Allowing roaming never requires disconnect confirmation');
 data={...data,roaming:false,connected:true,effectiveEnabled:true,roamingBlocked:false};await context.loadDataPage();assert.equal(nodes.get('net-roaming').textContent,'本地网络');
 data={...data,roaming:null,connected:false,effectiveEnabled:false,roamingBlocked:true};await context.loadDataPage();assert.match(nodes.get('data-message').textContent,/尚未确认是否漫游/);
 data.roamingEnabled=null;await context.loadDataPage();assert.equal(roam.disabled,true);
 data.roamingEnabled=false;data.roamingApplied=true;await context.loadDataPage();assert.match(nodes.get('roaming-toggle-label').textContent,/未完成/);assert.match(nodes.get('data-message').textContent,/漫游权限尚未确认/);
 console.log('Data UI passed: actual/desired state, disconnect confirmation, active-call protection, duplicate click guard, CSRF, stale-read protection, turn-on without confirmation, failure stays visible.');process.exit(0);
})().catch(e=>{console.error(e);process.exit(1);});
