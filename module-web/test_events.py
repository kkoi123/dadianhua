"""No real modem/network: QMI indications, SSE auth, Bark secret handling/dedup."""
import json,os,pathlib,subprocess,tempfile,time,select
ROOT=pathlib.Path(__file__).resolve().parent
subprocess.run(['clang','-std=c11','-shared','-fPIC','-DWATCH_MOCK','-Wall','-Wextra','-Werror',str(ROOT/'test_mock_qmi.c'),'-o',str(ROOT/'build/mock-watch.dylib')],check=True)
with tempfile.TemporaryDirectory(prefix='qdc-events-') as tmp:
    root=pathlib.Path(tmp);(root/'private').mkdir(mode=0o700);(root/'run').mkdir(mode=0o700)
    (root/'private/audio-origin').write_text('https://example.test\n');(root/'private/audio-origin').chmod(0o600)
    mode=root/'mode';mode.write_text('normal\n')
    fake=root/'curl';fake.write_text('''#!/usr/bin/env python3
import json,os,sys
payload=json.load(sys.stdin)
assert all(payload['device_key'] not in x for x in sys.argv)
assert sys.argv[-1]=='https://api.day.app/push'
assert '--cacert' in sys.argv and '--insecure' not in sys.argv
assert 'pdu' not in str(payload) and '号码' not in payload['body']
with open(os.environ['QDC_NOTIFY_TEST_LOG'],'a') as f:f.write(json.dumps({'page':payload['url'].split('#')[-1], 'call':payload.get('call')})+'\\n')
print('{"code":200}')
''');fake.chmod(0o700)
    env={**os.environ,'QDC_WEB_ROOT':tmp,'QDC_WEB_RUN':str(root/'run'),'QDC_TEST_LIBRARY':str(ROOT/'build/mock-watch.dylib'),'QDC_WATCH_TEST_STATE':str(mode),'QDC_TEST_LOG':str(root/'qmi-log'),'QDC_NOTIFY_TEST_CURL':str(fake),'QDC_NOTIFY_TEST_LOG':str(root/'notify-log'),'QDC_EVENTS_TEST_ONCE':'1'}
    def api(action,body=None,cookie='',method='GET'):
        raw=json.dumps(body or {}).encode();e={**env,'REQUEST_METHOD':method,'PATH_INFO':'/'+action,'CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(raw)),'HTTP_COOKIE':cookie}
        r=subprocess.check_output([str(ROOT/'build/api-native')],input=raw,env=e).decode().replace('\r\n','\n');return r.split('\n\n',1)
    subprocess.run([str(ROOT/'build/api-native'),'--init'],input=b'events-test-password\n',env=env,stdout=subprocess.DEVNULL,check=True)
    for endpoint in ['notify','events']:assert '401' in api(endpoint)[0]
    h,d=api('login',{'username':'admin','password':'events-test-password'},method='POST');login=json.loads(d)
    cookie=next(l for l in h.splitlines() if l.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0];csrf=login['csrfToken']
    config={'barkURL':'https://api.day.app/TESTKEY_123','enabled':'1','calls':'1','sms':'1','removeKey':'0'}
    assert '403' in api('notify-settings',config,cookie,'POST')[0]
    assert '405' in api('notify-settings',cookie=cookie)[0]
    config['csrfToken']=csrf
    for bad in ['http://api.day.app/TESTKEY_123','https://127.0.0.1/key','https://api.day.app/key?test=1']:
        assert '400' in api('notify-settings',{**config,'barkURL':bad},cookie,'POST')[0]
    h,d=api('notify-settings',config,cookie,'POST');assert json.loads(d)['configured'] and 'TESTKEY_123' not in d
    assert (root/'private/notify').stat().st_mode&0o777==0o600
    process=subprocess.Popen([str(ROOT/'build/comm-native'),'watch'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    def state():return json.loads((root/'run/events-state').read_text())
    def await_state(condition):
        for _ in range(100):
            if process.poll() is not None:raise AssertionError('watch exited '+str(process.stderr.read()))
            try:
                data=state()
                if condition(data):return data
            except FileNotFoundError:pass
            time.sleep(.05)
        raise AssertionError('event deadline')
    def pushes():return (root/'notify-log').read_text().splitlines() if (root/'notify-log').exists() else []
    def change(value):new=root/'mode.new';new.write_text(value+'\n');new.replace(mode)
    def await_pushes(count):
        for _ in range(80):
            if len(pushes())==count:return
            time.sleep(.05)
        raise AssertionError(('notification count',pushes(),count))
    try:
        initial=await_state(lambda d:d['callsReady'] and d['smsReady']);assert initial['callRegistered'] and initial['smsRegistered'];assert pushes()==[],'baseline must not push history'
        h,d=api('events',cookie=cookie);assert 'text/event-stream' in h and 'event: state' in d and ': keepalive' not in d and 'TESTKEY_123' not in d
        snapshot=json.loads(d.split('event: state\n',1)[1].split('data: ',1)[1].split('\n',1)[0]);assert snapshot['callState']==initial['callState']
        # The real streaming loop stays silent past the former 25-second
        # heartbeat, then delivers a changed state and releases a closed pipe.
        stream_env={**env,'REQUEST_METHOD':'GET','PATH_INFO':'/events','HTTP_COOKIE':cookie}
        stream_env.pop('QDC_EVENTS_TEST_ONCE')
        stream=subprocess.Popen([str(ROOT/'build/api-native')],env=stream_env,
                                stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            start=time.monotonic();initial_output=b''
            while b'event: state\n' not in initial_output:
                assert select.select([stream.stdout],[],[],3)[0], 'Initial SSE missing'
                initial_output+=os.read(stream.stdout.fileno(),65536)
            idle_output=b''
            while time.monotonic()-start<27:
                if select.select([stream.stdout],[],[],.25)[0]:
                    chunk=os.read(stream.stdout.fileno(),65536)
                    assert chunk, 'Silent stream exited early'
                    idle_output+=chunk
            assert not idle_output and b': keepalive' not in initial_output
            change('incoming');changed_output=b''
            deadline=time.monotonic()+5
            while b'event: state\n' not in changed_output:
                assert time.monotonic()<deadline
                if select.select([stream.stdout],[],[],.25)[0]:
                    chunk=os.read(stream.stdout.fileno(),65536);assert chunk
                    changed_output+=chunk
            assert json.loads(changed_output.split(b'data: ',1)[1].split(b'\n',1)[0])['callState']['calls'][0]['state']==2
            stream.stdout.close();stream.wait(timeout=3)
            assert stream.returncode==0
        finally:
            if stream.poll() is None:stream.terminate();stream.wait(timeout=3)
        change('incoming');incoming=await_state(lambda d:d['callState']['calls'] and d['callState']['calls'][0]['state']==2);await_pushes(1)
        time.sleep(.5);assert len(pushes())==1
        change('conversation');await_state(lambda d:d['callState']['calls'] and d['callState']['calls'][0]['state']==3)
        change('call-check-failure');await_state(lambda d:not d['callsReady'])
        active=json.loads(api('call-history-list',{'csrfToken':csrf},cookie,'POST')[1])['records']
        assert len(active)==1 and active[0]['status']==0, 'Failed QMI query must not end a live history record'
        change('normal');await_state(lambda d:d['callsReady'] and not d['callState']['calls'])
        # No browser calls were needed to record this incoming call and answer.
        h,d=api('call-history-list',{'csrfToken':csrf},cookie,'POST');records=json.loads(d)['records']
        assert len(records)==1 and records[0]['direction']==2 and records[0]['answered'] and records[0]['status']==1
        assert (root/'private/call-history').stat().st_mode & 0o777 == 0o600
        for endpoint in ['call-history-list','call-history-clear','call-history-import']:
            assert '401' in api(endpoint,method='POST')[0]
            assert '405' in api(endpoint,cookie=cookie)[0]
            assert '403' in api(endpoint,{},cookie,'POST')[0]
        legacy={'csrfToken':csrf,'records':'1 1700000000000 1700000010000 10086'}
        assert json.loads(api('call-history-import',legacy,cookie,'POST')[1])['ok']
        assert json.loads(api('call-history-import',legacy,cookie,'POST')[1])['ok']
        assert json.loads(api('call-history-list',{'csrfToken':csrf},cookie,'POST')[1])['total']==2
        assert '400' in api('call-history-import',{'csrfToken':csrf,'records':'broken'},cookie,'POST')[0]
        assert '400' in api('call-history-list',{'csrfToken':csrf,'cursor':'501'},cookie,'POST')[0]
        assert json.loads(api('call-history-clear',{'csrfToken':csrf},cookie,'POST')[1])['ok']
        assert json.loads(api('call-history-import',legacy,cookie,'POST')[1])['ok']
        assert json.loads(api('call-history-list',{'csrfToken':csrf},cookie,'POST')[1])['records']==[]
        change('reused');await_state(lambda d:d['smsRevision']>initial['smsRevision']);await_pushes(2);time.sleep(.3);assert len(pushes())==2
        assert json.loads(pushes()[0])['page']=='phone' and json.loads(pushes()[1])['page']=='sms'
        # One bounded retry for a transport failure; a successful request is
        # not repeated. Fake curl fails before accepting the first payload.
        retry=root/'retry';retry.write_text('''#!/usr/bin/env python3
import json,os,pathlib,sys
payload=json.load(sys.stdin);counter=pathlib.Path(os.environ['QDC_RETRY_COUNT'])
n=int(counter.read_text()) if counter.exists() else 0;counter.write_text(str(n+1))
if n==0:sys.exit(7)
print('{"code":200}\\nQDC_HTTP=200')
''');retry.chmod(0o700)
        env_retry={**env,'QDC_NOTIFY_TEST_CURL':str(retry),'QDC_RETRY_COUNT':str(root/'retry-count')}
        subprocess.run([str(ROOT/'build/comm-native'),'notify-send','call'],env=env_retry,check=True)
        assert (root/'retry-count').read_text()=='2'
        assert 'call 0 200 200' in (root/'run/notify-diagnostic').read_text()
        # Disabling prevents future deliveries; no automatic testing on save.
        api('notify-settings',{**config,'barkURL':'','enabled':'0'},cookie,'POST');time.sleep(1.2)
        change('incoming');await_state(lambda d:d['callState']['calls'] and d['callState']['calls'][0]['state']==2);time.sleep(.3);assert len(pushes())==2
        # Explicit test is allowed while automatic reminders are disabled.
        h,d=api('notify-test',{'csrfToken':csrf},cookie,'POST');assert json.loads(d)['ok'];assert len(pushes())==3
        assert '503' in api('notify-test',{'csrfToken':csrf},cookie,'POST')[0]
        api('logout',{'csrfToken':csrf},cookie,'POST');assert '401' in api('events',cookie=cookie)[0]
        operations=(root/'qmi-log').read_text().splitlines();assert all(x not in operations for x in ['voice:20','sms:20','sms:24','voice:21','voice:22'])
    finally:process.terminate();process.wait(timeout=5)
print('Events passed: local QMI subscriptions, initial baseline, incoming/SMS dedup, disabled reminders, fixed HTTPS/secret stdin, explicit test/rate limit, authenticated SSE and logout.')
