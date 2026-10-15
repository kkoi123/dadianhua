/* Actual UI behavior: unit rendering, authenticated reset, and an old refresh
 * response racing a reset must not restore the previous totals. */
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
class E{constructor(){this.classList={toggle(){},add(){},remove(){}};this.style={};this.handlers={};this.textContent='';}get firstElementChild(){return this.child||(this.child=new E());}get lastElementChild(){return this.firstElementChild;}addEventListener(k,f){this.handlers[k]=f;}setAttribute(){}removeAttribute(){}append(){}replaceChildren(){}reset(){}}
const nodes=new Map(),doc={body:{dataset:{}},hidden:false,getElementById(id){if(!nodes.has(id))nodes.set(id,new E());return nodes.get(id);},querySelectorAll(){return[];},addEventListener(){},createElement(){return new E();},createDocumentFragment(){return new E();}};
let pendingTraffic=null,hold=false,resetRequest;
const context={document:doc,location:{hostname:'192.168.225.1',search:'',hash:'#network',pathname:'/'},history:{replaceState(){}},URLSearchParams,AbortController,TextEncoder,Date,console,setInterval(){return 1;},clearInterval(){},setTimeout,clearTimeout,addEventListener(){},createQDCCommunications(){return{auth(){},page(){},refresh(){}};},
 async fetch(path,options){
  let result;
  if(path.endsWith('/session'))result={authenticated:true,csrfToken:'test-csrf'};
  else if(path.endsWith('/status'))result={version:'0.3.5',storage:[],network:{}};
  else if(path.endsWith('/radio'))result={network:{}};
  else if(path.endsWith('/traffic-reset')){resetRequest=options;result={receivedBytes:0,sentBytes:0,since:100,interfaces:1};}
  else if(path.endsWith('/traffic')){result={receivedBytes:2048,sentBytes:1024,since:100,interfaces:1};if(hold)await new Promise(resolve=>pendingTraffic=resolve);}
  else throw Error('Unexpected request '+path);
  return{ok:true,async json(){return result;}};
 }};context.window=context;vm.createContext(context);
const source=fs.readFileSync(__dirname+'/static/app.js','utf8').replace('  start();','  globalThis.ready=start();globalThis.refreshPage=refresh;');
vm.runInContext(source,context);
(async()=>{await context.ready;assert.equal(nodes.get('traffic-received').textContent,'2 KB');assert.equal(nodes.get('traffic-sent').textContent,'1 KB');assert.equal(nodes.get('traffic-total').textContent,'3 KB');
 hold=true;const refreshing=context.refreshPage();while(!pendingTraffic)await new Promise(r=>setTimeout(r,1));
 await nodes.get('traffic-reset').handlers.click();assert.equal(resetRequest.method,'POST');assert.equal(JSON.parse(resetRequest.body).csrfToken,'test-csrf');assert.equal(nodes.get('traffic-total').textContent,'0 B');
 pendingTraffic();await refreshing;assert.equal(nodes.get('traffic-total').textContent,'0 B');assert.equal(nodes.get('traffic-reset').disabled,false);
 clearTimeout(context.state?.toast);console.log('Traffic UI passed: totals/units, POST with CSRF, and stale refresh cannot undo reset.');process.exit(0);
})().catch(e=>{console.error(e);process.exit(1);});
