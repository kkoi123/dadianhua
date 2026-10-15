/* Real UI handlers, fake APIs and PDU fixtures; no network or modem. */
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),codec=require('./static/sms-codec.js');
class Element {
  constructor(){this.children=[];this.listeners={};this.dataset={};this.textContent='';this.value='';this.classList={toggle(){}};}
  append(...nodes){this.children.push(...nodes);} replaceChildren(...nodes){this.children=nodes.flatMap(n=>n.fragment?n.children:[n]);}
  addEventListener(type,cb){this.listeners[type]=cb;}setAttribute(){}reset(){}focus(){}get childNodes(){return this.children;}
}
function setup(mode='normal'){
  const nodes=new Map(),requests=[];let ui;
  const long='你好，'.repeat(35);
  let records=[{id:'a'.repeat(32),pdus:codec.encode('13800138000',long,1).join(','),timestamp:1700000000,timeExact:true,status:'submitted',sentParts:2,totalParts:2},
               {id:'b'.repeat(32),pdus:codec.encode('10086','查询流量',2).join(','),timestamp:1600000000,timeExact:false,status:'uncertain',sentParts:0,totalParts:1,error:'结果尚未确认'},
               {id:'c'.repeat(32),pdus:codec.encode('13800138000','长短信'.repeat(35),3).join(','),timestamp:1500000000,timeExact:true,status:'partial',sentParts:1,totalParts:2,error:'第二段失败'}];
  const document={getElementById(id){if(!nodes.has(id))nodes.set(id,new Element());return nodes.get(id);},querySelectorAll(){return [];},createElement(){return new Element();},createDocumentFragment(){let n=new Element();n.fragment=true;return n;}};
  const context={document,QDCSmsCodec:codec,crypto:require('node:crypto').webcrypto,confirm(){return true;},localStorage:{getItem(){return null;},setItem(){}}};context.window=context;
  vm.createContext(context);vm.runInContext(fs.readFileSync('static/communications.js','utf8'),context);
  ui=context.createQDCCommunications({preview:false,toast(){},async api(name,{body}){
    requests.push({name,body});
    if(name==='sms-list')return {sim:'test-sim',canMutate:true,next:null,messages:[]};
    if(name==='sms-outbox-list')return {messages:records.map(x=>({...x})),next:null};
    if(name==='sms-outbox-delete'){
      if(mode==='delete-error')throw new Error('删除失败');
      if(mode==='no-proof')return {ok:true};
      records=records.filter(x=>x.id!==body.id);if(mode==='logout')ui.auth(false);return {deleted:true};
    }
    if(name==='sms-send'){
      const row={id:body.requestId,pdus:body.pdus,timestamp:1800000000,timeExact:true,status:mode==='send-error'?'uncertain':'submitted',sentParts:mode==='send-error'?0:1,totalParts:1};records.unshift(row);
      if(mode==='send-error')throw new Error('通讯操作未返回完整结果，请勿直接重发');return {sentParts:1};
    }
    throw new Error('Unexpected API '+name);
  }});
  const get=id=>document.getElementById(id),all=n=>[n,...n.children.flatMap(all)],click=id=>get(id).listeners.click();
  async function idle(){for(let i=0;i<100;i++){await new Promise(r=>setImmediate(r));if(!get('sms-refresh').disabled)return;}throw new Error('operation stuck');}
  async function open(){ui.auth(true);ui.page('sms');await idle();click('sms-tab-outbox');await idle();}
  function action(label,where='sms-outbox-list'){const n=all(get(where)).find(n=>n.textContent===label);assert(n,label);n.listeners.click();}
  return {ui,get,all,click,idle,open,action,requests,long};
}
(async()=>{
  let t=setup();await t.open();assert.equal(t.get('sms-panel-outbox').hidden,false);assert.equal(t.get('sms-panel-list').hidden,true);
  let text=t.all(t.get('sms-outbox-list')).map(x=>x.textContent).join('|');assert.match(text,/已提交 · 2\/2 段/);assert.match(text,/结果未确认/);assert.match(text,/部分发送 · 1\/2 段/);assert.match(text,/旧记录时间/);
  t.action('查看');assert.equal(t.get('sms-panel-detail').hidden,false);assert.equal(t.get('sms-back').textContent,'‹ 已发送');assert(t.all(t.get('sms-detail-content')).some(n=>n.textContent===t.long));
  t.ui.events({callsReady:false},true);await t.idle();assert(t.all(t.get('sms-detail-content')).some(n=>n.textContent===t.long),'Incoming SMS must not replace a sent detail');
  t.action('再次编辑','sms-detail-content');assert.equal(t.get('sms-number').value,'13800138000');assert.equal(t.get('sms-body').value,t.long);assert.equal(t.get('sms-panel-compose').hidden,false);assert.equal(t.requests.filter(x=>x.name==='sms-send').length,0);
  t.click('sms-tab-outbox');t.action('查看');t.click('sms-back');assert.equal(t.get('sms-panel-outbox').hidden,false);
  t.action('查看');t.action('删除','sms-detail-content');await t.idle();assert.equal(t.get('sms-panel-detail').hidden,true);assert.equal(t.get('sms-outbox-list').children.length,2);t.click('sms-outbox-refresh');await t.idle();assert.equal(t.get('sms-outbox-list').children.length,2);
  for(const mode of ['delete-error','no-proof']){t=setup(mode);await t.open();t.action('删除');await t.idle();assert.equal(t.get('sms-outbox-list').children.length,3);assert.equal(t.requests.filter(x=>x.name==='sms-outbox-delete').length,1);}
  t=setup('logout');await t.open();t.action('删除');await t.idle();assert.equal(t.get('sms-outbox-list').children.length,0);assert.equal(t.get('sms-detail-content').children.length,0);
  for(const mode of ['normal','send-error']){
    t=setup(mode);await t.open();t.click('sms-tab-compose');t.get('sms-number').value='10086';t.get('sms-body').value='测试发送';t.get('sms-compose').listeners.submit({preventDefault(){}});await t.idle();
    assert.equal(t.requests.filter(x=>x.name==='sms-send').length,1,'Never automatically resend');
    if(mode==='normal'){assert.equal(t.get('sms-panel-outbox').hidden,false);assert.equal(t.get('sms-body').value,'');assert.match(t.get('sms-message').textContent,/不代表对方已收到/);}
    else {assert.equal(t.get('sms-body').value,'测试发送');assert.match(t.get('sms-message').textContent,/请勿直接重发/);assert(t.all(t.get('sms-outbox-list')).some(n=>n.textContent.includes('结果未确认')));}
  }
  console.log('Sent SMS UI passed: tabs, multipart content, statuses, inline details, incoming event isolation, edit without sending, persistent and rejected deletes, logout race, success and uncertain send without retry.');
})().catch(e=>{console.error(e);process.exitCode=1;});
