"""Card-slot recovery tests: mocked QMI only, no hardware writes."""
import json,os,pathlib,subprocess,tempfile,shutil
ROOT=pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as tmp:
 base=pathlib.Path(tmp);run=base/'run';run.mkdir(mode=0o700);(base/'private').mkdir(mode=0o700);log=base/'log';lib=base/'qmi.dylib'
 subprocess.run(['clang','-dynamiclib','-std=c11',str(ROOT/'test_sim_pin_mock.c'),'-o',str(lib)],check=True)
 env={**os.environ,'QDC_WEB_ROOT':str(base),'QDC_WEB_RUN':str(run),'QDC_TEST_LIBRARY':str(lib),'QDC_TEST_LOG':str(log)}
 def call(action,scenario,wire=None):
  result=subprocess.check_output([str(ROOT/'build/comm-native'),action],input=wire,text=True,env={**env,'QDC_TEST_SCENARIO':scenario});return json.loads(result)
 def recover(scenario,id,token=None):
  if token is None:token=call('sim-pin-state',scenario)['stateToken']
  return call('sim-recover',scenario,f'{token}\n{id*64}\n')
 for scenario in ['ready','absent','initializing','pin','blocked']:
  assert not call('sim-pin-state',scenario)['recoverable'];assert not recover(scenario,'a')['ok']
 assert not recover('recover-call','a')['ok'];assert not log.exists()
 assert not recover('recover-ok','a','00')['ok'];assert not log.exists()
 assert call('sim-pin-state','recover-ok')['recoverable']
 r=recover('recover-ok','b');assert r['ok'] and r['recovered'];assert log.read_text().splitlines()==['power-off','power-on']
 assert not recover('recover-ok','b')['ok'];assert log.read_text().splitlines()==['power-off','power-on']
 log.unlink();r=recover('recover-off-timeout','c');assert r['ok'];assert log.read_text().splitlines()==['power-off','power-on']
 log.unlink();r=recover('recover-off-rejected','d');assert not r['ok'];assert log.read_text().splitlines()==['power-off']
 log.unlink();r=recover('recover-on-timeout','e');assert not r['ok'] and r['uncertain'];assert log.read_text().splitlines()==['power-off','power-on']
 assert not recover('recover-on-timeout','e')['ok'];assert log.read_text().splitlines()==['power-off','power-on']
 log.unlink();r=recover('recover-still-error','6');assert r['ok'] and not r['recovered'] and r['powerRestored'];assert log.read_text().splitlines()==['power-off','power-on']
 (base/'www/cgi-bin').mkdir(parents=True);shutil.copy(ROOT/'build/comm-native',base/'www/cgi-bin/qdc-comm')
 subprocess.run([str(ROOT/'build/api-native'),'--init'],input=b'test-admin-password\n',env=env,stdout=subprocess.DEVNULL,check=True)
 def api(action,body=None,cookie='',method='POST'):
  raw=json.dumps(body or {}).encode();settings={**env,'QDC_TEST_SCENARIO':'recover-ok','REQUEST_METHOD':method,'PATH_INFO':'/'+action,'CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(raw)),'HTTP_COOKIE':cookie}
  out=subprocess.check_output([str(ROOT/'build/api-native')],input=raw,env=settings).decode().replace('\r\n','\n');head,data=out.split('\n\n',1);return head,json.loads(data)
 assert '401' in api('sim-recover')[0]
 head,login=api('login',{'username':'admin','password':'test-admin-password'})
 cookie=next(l for l in head.splitlines() if l.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0]
 assert '405' in api('sim-recover',cookie=cookie,method='GET')[0]
 assert '403' in api('sim-recover',{},cookie)[0]
 state=api('sim-pin-state',cookie=cookie,method='GET')[1];payload={'csrfToken':login['csrfToken'],'stateToken':state['stateToken'],'requestId':'f'*64}
 assert '400' in api('sim-recover',{**payload,'slot':'2'},cookie)[0]
 assert '400' in api('sim-recover',{**payload,'stateToken':'nothex'},cookie)[0]
 log.unlink();head,r=api('sim-recover',payload,cookie);assert '200' in head and r['ok'];assert log.read_text().splitlines()==['power-off','power-on']
 assert not api('sim-recover',payload,cookie)[1]['ok'];assert log.read_text().splitlines()==['power-off','power-on']
print('SIM recovery passed: state/token/call gates, exact slot, one power cycle, restore after lost off reply, no retry after uncertain on, duplicate and authenticated CSRF API guards.')
