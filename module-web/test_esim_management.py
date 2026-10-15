#!/usr/bin/env python3
"""Real native libeuicc + synthetic QMI/card, then real authenticated async CGI."""
from pathlib import Path
import json,os,subprocess,tempfile,shutil,time,fcntl
R=Path(__file__).resolve().parent
AID='a06573746b6d65ffff4953442d522030';EID='89000000000000000000000000000001';ACTIVE='89860000000000000001';OTHER='89860000000000000002'
def tlv(tag,b):
 t=tag.to_bytes((tag.bit_length()+7)//8,'big');n=len(b)
 return t+(bytes([n]) if n<128 else b'\x81'+bytes([n]))+b
def profile(id,enabled):
 id=id+'f' if len(id)==19 else id
 return tlv(0xe3,tlv(0x5a,bytes(int(id[i+1]+id[i],16) for i in range(0,20,2)))+tlv(0x9f70,bytes([enabled]))+tlv(0x90,b'Test')+tlv(0x91,b'Test'))
with tempfile.TemporaryDirectory() as tmp:
 b=Path(tmp);run=b/'run';run.mkdir(mode=0o700);(b/'private').mkdir(mode=0o700);(b/'www/bin').mkdir(parents=True)
 lib=b/'qmi.dylib';subprocess.run(['clang','-dynamiclib','-std=c11',str(R/'test_esim_management_mock.c'),'-o',str(lib)],check=True)
 (b/'eid').write_bytes(tlv(0xbf3e,tlv(0x5a,bytes.fromhex(EID))));(b/'profiles').write_bytes(tlv(0xbf2d,tlv(0xa0,profile(ACTIVE,1)+profile(OTHER,0)+profile('8944476500003106114',0))))
 log=b/'log';env={**os.environ,'QDC_WEB_ROOT':str(b),'QDC_WEB_RUN':str(run),'QDC_TEST_LIBRARY':str(lib),'QDC_ESIM_FIXTURES':str(b),'QDC_ESIM_LOG':str(log)}
 worker=R/'build/esim-native/qdc-esim'
 def operation(op,id=OTHER,scenario='normal',eid=EID,name='New name',activation='',aid=AID,imei='490154203237518'):
  log.write_text('');result=subprocess.run([str(worker),'a'*64],input='\n'.join([op,aid,eid,id,name,activation,'',imei])+'\n',text=True,env={**env,'QDC_TEST_SCENARIO':scenario},capture_output=True,timeout=35)
  assert (run/'esim-result').exists(),result.stderr
  return json.loads((run/'esim-result').read_text()),log.read_text().splitlines()
 check=json.loads(subprocess.check_output([str(worker),'--check',AID],env=env));assert check=={'ok':True,'profileCount':3,'enabledCount':1}
 for op in ['enable','disable','rename','delete','notifications']:
  result,ops=operation(op,ACTIVE if op=='disable' else OTHER);assert result['state']=='complete',(op,result,ops)
  assert op in ops and ops[-1]=='close',ops
 result,ops=operation('rename',id='8944476500003106114');assert result['state']=='complete' and 'rename' in ops
 for op,id,scenario,eid in [('delete',ACTIVE,'normal',EID),('rename','89860000000000000003','normal',EID),('enable',OTHER,'normal','89000000000000000000000000000002'),('enable',OTHER,'incoming',EID),('enable',OTHER,'radio-pin',EID)]:
  result,ops=operation(op,id,scenario,eid);assert result['state']=='failed' and not result['uncertain'];assert not any(x in ops for x in ['enable','disable','rename','delete'])
 for scenario in ['mutation-transport','mutation-rejected','close-error']:
  result,ops=operation('rename',scenario=scenario);assert result['state']=='failed' and result['uncertain'] and ops[-1]=='close',result
 for activation in ['not-a-code','LPA:1$localhost$secret','LPA:1$example.com$secret$$bad']:
  result,ops=operation('download',activation=activation);assert result['state']=='failed' and not result['uncertain']
 result,ops=operation('download',activation='LPA:1$example.com$secret',imei='');assert result['state']=='failed' and 'imei' in ops and '格式无效' not in result['message'],(result,ops)
 result,ops=operation('rename',name='中'*22);assert result['state']=='failed' and not any(x=='rename' for x in ops)
 # Mock only process duration for API locking/replay tests; card tests above run real libeuicc.
 shim=b/'www/bin/qdc-esim';shim.write_text('#!/usr/bin/env python3\nimport os,sys,time,json\nfrom pathlib import Path\nr=Path(os.environ["QDC_WEB_RUN"])\nwire=sys.stdin.read()\nwith (r/"executions").open("a") as f:f.write(sys.argv[1]+"\\n")\ntime.sleep(0.8)\n(r/"esim-result").write_text(json.dumps({"ok":True,"jobId":sys.argv[1],"state":"complete","message":"done"},separators=(",",":")))\n');shim.chmod(0o700)
 subprocess.run([str(R/'build/api-native'),'--init'],input=b'local-test-password-123\n',stdout=subprocess.DEVNULL,env=env,check=True)
 def api(action,body=None,cookie='',method='POST'):
  raw=json.dumps(body or {}).encode();e={**env,'REQUEST_METHOD':method,'PATH_INFO':'/'+action,'CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(raw)),'HTTP_COOKIE':cookie}
  raw=subprocess.check_output([str(R/'build/api-native')],input=raw,env=e).decode().replace('\r\n','\n');h,d=raw.split('\n\n',1);return h,json.loads(d)
 assert '401' in api('esim-operation')[0];assert '401' in api('esim-job',method='GET')[0]
 head,login=api('login',{'username':'admin','password':'local-test-password-123'});cookie=next(l for l in head.splitlines() if l.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0]
 payload={'operation':'rename','aid':AID,'eid':EID,'iccid':OTHER,'name':'New','requestId':'b'*64,'csrfToken':login['csrfToken']}
 assert '403' in api('esim-operation',{**payload,'csrfToken':'bad'},cookie)[0]
 assert '400' in api('esim-operation',{**payload,'shell':'anything'},cookie)[0]
 h,result=api('esim-operation',payload,cookie);assert '202' in h and result['accepted']
 h,result=api('esim-operation',payload,cookie);assert '202' in h and result['duplicate']
 assert '409' in api('esim-operation',{**payload,'requestId':'c'*64},cookie)[0]
 assert '409' in api('data-set',{'enabled':'0','csrfToken':login['csrfToken']},cookie)[0]
 
 for _ in range(50):
  j=api('esim-job',cookie=cookie,method='GET')[1]
  if j['state']=='complete':break
  time.sleep(0.1)
 assert j['state']=='complete',(j, (run/'executions').read_text() if (run/'executions').exists() else 'no execution')
 assert api('esim-operation',payload,cookie)[1]['duplicate'];assert '409' in api('esim-operation',{**payload,'name':'Changed'},cookie)[0]
 assert (run/'executions').read_text().splitlines()==['b'*64]
 assert (run/('esim-request-'+'b'*64)).stat().st_mode&0o777==0o600
 assert not any(EID.encode() in p.read_bytes() for p in run.iterdir() if p.is_file()),'no card IDs in job records'
 (run/'esim-result').write_text('{"ok":true,"state":"running"}')
 assert api('esim-job',cookie=cookie,method='GET')[1]['state']=='interrupted'
print('eSIM management passed: real libeuicc QMI mutations/refresh, card identity, call/PIN guards, active-delete refusal, uncertain failures, async locking, CSRF, replay protection and secret-free records.')
