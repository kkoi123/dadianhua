"""Exercise actual UIM PIN1/UPIN wire requests without any hardware writes."""
import json,os,pathlib,subprocess,tempfile,shutil
ROOT=pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as tmp:
    base=pathlib.Path(tmp);run=base/'run';run.mkdir(mode=0o700);(base/'private').mkdir(mode=0o700);log=base/'log';lib=base/'qmi.dylib'
    subprocess.run(['clang','-dynamiclib','-std=c11',str(ROOT/'test_sim_pin_mock.c'),'-o',str(lib)],check=True)
    env={**os.environ,'QDC_WEB_ROOT':str(base),'QDC_WEB_RUN':str(run),'QDC_TEST_LIBRARY':str(lib),'QDC_TEST_LOG':str(log)}
    def call(op,scenario='pin',wire=None):
        p=subprocess.run([str(ROOT/'build/comm-native'),op],input=wire,text=True,capture_output=True,env={**env,'QDC_TEST_SCENARIO':scenario},check=True)
        assert '1234' not in p.stdout+p.stderr
        return json.loads(p.stdout)
    def verify(scenario='pin',id='a'*64,token=None,pin='1234'):
        token=token or call('sim-pin-state',scenario)['stateToken']
        return call('sim-pin-verify',scenario,f'{pin}\n{token}\n{id}\n')
    for scenario in ['pin','upin','nonprov','ordinary']:
        state=call('sim-pin-state',scenario);assert state['required'] and state['retries']==3
        id={'pin':'a','upin':'b','nonprov':'c','ordinary':'4'}[scenario]*64
        assert verify(scenario,id)['ok'];n=log.read_text().count('verify-once')
        assert not verify(scenario,id)['ok'];assert log.read_text().count('verify-once')==n
    for scenario in ['ready','blocked','puk-required']:
        assert not call('sim-pin-state',scenario)['required'];assert not verify(scenario,'d'*64)['ok']
    for scenario in ['absent','card-error','initializing']:
        state=call('sim-pin-state',scenario);assert state['ok'] and not state['required'] and state['retries'] is None and state['stateToken']
        assert not verify(scenario,'5'*64,token='00')['ok']
    assert not call('sim-pin-state','truncated')['ok']
    assert not verify(id='e'*64,token='00')['ok'];assert not verify(id='e'*64,pin='12a4')['ok']
    wrong=verify('wrong','f'*64);assert not wrong['ok'] and wrong['retries']==2 and not wrong['uncertain']
    timeout=verify('timeout','1'*64);assert not timeout['ok'] and timeout['uncertain']
    assert not verify('timeout','1'*64)['ok'];
    malformed=verify('malformed-reply','3'*64);assert not malformed['ok'] and malformed['uncertain'];assert '1234' not in ''.join(p.read_text() for p in run.glob('sim-pin-request-*'))
    (base/'www/cgi-bin').mkdir(parents=True);shutil.copy(ROOT/'build/comm-native',base/'www/cgi-bin/qdc-comm')
    subprocess.run([str(ROOT/'build/api-native'),'--init'],input=b'test-admin-password\n',env=env,stdout=subprocess.DEVNULL,check=True)
    def api(action,body=None,cookie='',method='POST'):
        raw=json.dumps(body or {}).encode();settings={**env,'QDC_TEST_SCENARIO':'pin','REQUEST_METHOD':method,'PATH_INFO':'/'+action,'CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(raw)),'HTTP_COOKIE':cookie}
        result=subprocess.check_output([str(ROOT/'build/api-native')],input=raw,env=settings).decode().replace('\r\n','\n');head,data=result.split('\n\n',1);return head,json.loads(data)
    assert '401' in api('sim-pin-state',method='GET')[0]
    head,login=api('login',{'username':'admin','password':'test-admin-password'})
    cookie=next(l for l in head.splitlines() if l.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0]
    head,state=api('sim-pin-state',cookie=cookie,method='GET');assert '200' in head and state['required']
    payload={'pin':'1234','stateToken':state['stateToken'],'requestId':'2'*64,'csrfToken':login['csrfToken']}
    assert '405' in api('sim-pin-verify',cookie=cookie,method='GET')[0]
    assert '403' in api('sim-pin-verify',{},cookie)[0]
    assert '400' in api('sim-pin-verify',{**payload,'pin':'abc'},cookie)[0]
    head,result=api('sim-pin-verify',payload,cookie);assert '200' in head and result['ok'],result
    n=log.read_text().count('verify-once');assert not api('sim-pin-verify',payload,cookie)[1]['ok'];assert log.read_text().count('verify-once')==n
print('SIM PIN passed: ordinary empty-AID/PIN1/UPIN/nonprovisioned AID, absent/card error/ready/PUK/malformed guards, retries, uncertainty, duplicate suppression, secret transport and authenticated CSRF API.')
