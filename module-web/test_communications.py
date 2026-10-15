"""Local protocol and authentication checks; never send SMS or call a modem."""
import json, os, pathlib, subprocess, tempfile, shutil, fcntl, time
ROOT = pathlib.Path(__file__).resolve().parent
subprocess.run(['clang', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', str(ROOT/'test_mock_qmi.c'), '-o', str(ROOT/'build/mock-qmi.dylib')], check=True)
PDU='0001000d91683108108300f00008044f60597d'
SIM='89860000000000000000'
with tempfile.TemporaryDirectory(prefix='qdc-comm-') as folder:
    base=pathlib.Path(folder); (base/'private').mkdir(mode=0o700);(base/'run').mkdir(mode=0o700)
    env={**os.environ,'QDC_WEB_ROOT':folder,'QDC_WEB_RUN':str(base/'run'),'QDC_TEST_LIBRARY':str(ROOT/'build/mock-qmi.dylib'),'QDC_TEST_LOG':str(base/'operations')}
    def helper(*args, scenario='normal'):
        return json.loads(subprocess.check_output([str(ROOT/'build/comm-native'),*args],env={**env,'QDC_TEST_SCENARIO':scenario}))
    def operations(): return (base/'operations').read_text().splitlines() if (base/'operations').exists() else []
    radio=helper('radio');net=radio['network']
    assert radio['complete'] and net['operator']=='中国移动' and net['networkType']=='LTE'
    assert net['signalDbm']==-94 and net['simState']=='已就绪'
    assert net['apn']=='cmnet' and net['apnSource']=='current' and net['apnAddress']=='10.203.20.206'
    assert net['roaming'] is False
    assert helper('radio',scenario='radio-roaming')['network']['roaming'] is True
    assert helper('radio',scenario='radio-roaming-missing')['network']['roaming'] is None
    assert helper('radio',scenario='radio-roaming-invalid')['network']['roaming'] is None
    assert helper('roaming-state',scenario='radio-roaming')['roaming'] is True
    assert helper('roaming-state')['roaming'] is False
    assert helper('roaming-state',scenario='radio-searching')['roaming'] is None
    assert helper('roaming-state','1')['permission'] is True
    assert helper('roaming-state','1',scenario='roaming-unsupported')['permission'] is None
    assert helper('roaming-state','1',scenario='roaming-flag-missing')['permission'] is None
    assert not helper('roaming-state','17')['ok']
    configured=helper('radio',scenario='radio-default')['network']
    assert configured['apn']=='cmhk' and configured['apnSource']=='configured' and configured['apnAddress'] is None
    assert helper('radio',scenario='radio-failure')['network']['apn'] is None
    assert helper('radio',scenario='radio-primary-unset')['network']['simState']=='已就绪'
    assert helper('radio',scenario='radio-pin')['network']['simState']=='需要 PIN 码'
    assert helper('radio',scenario='radio-no-sim')['network']['simState']=='未插卡'
    malformed=helper('radio',scenario='radio-truncated')['network']
    assert malformed['simState'] is None and malformed['operator'] is None and malformed['signalDbm'] is None
    searching=helper('radio',scenario='radio-searching')['network']
    assert searching['registrationState']==2 and searching['operator'] is None and searching['networkType'] is None
    assert searching['roaming'] is None, 'Unregistered state must not reuse a stale roaming flag'
    assert operations()==[], 'radio status must never mutate the modem'
    assert helper('roaming-config','1','1')['permission'] is True and operations()==[], 'Unchanged setting must not write NV'
    assert not helper('roaming-config','1','2')['ok'] and operations()==[]
    assert not helper('roaming-config','1','0',scenario='roaming-unsupported')['ok'] and operations()==[]
    assert helper('roaming-config','1','0')['permission'] is False and operations()==['wds:28']
    assert not helper('roaming-config','1','0',scenario='roaming-write-ignored')['ok']
    assert not helper('roaming-config','1','0',scenario='roaming-write-error')['ok']
    assert not helper('roaming-config','1','0',scenario='roaming-write-timeout')['ok']
    (base/'operations').unlink()
    listed=helper('sms-list','0');assert listed['sim']==SIM and listed['canMutate']
    assert len(listed['messages'])==2 and listed['messages'][0]['pdu']==PDU
    fallback=helper('sms-list','0',scenario='uim');assert fallback['canMutate'] and fallback['sim'].startswith('ef:')
    assert helper('sms-list','0',scenario='uim')['sim']==fallback['sim'], 'raw EF identity must be stable'
    assert not helper('sms-list','0',scenario='uim-invalid')['canMutate']
    assert not helper('sms-delete','0','7',PDU,fallback['sim'],scenario='uim-swap')['ok']
    assert not helper('sms-delete','0','7',PDU,SIM,scenario='swap')['ok']
    assert not helper('sms-delete','0','7',PDU,SIM,scenario='reused')['ok']
    assert operations()==[], 'stale SIM or reused slot must not delete'
    removed=helper('sms-delete','0','7',PDU,SIM);assert removed['ok'] and removed['deleted'] and removed['verified'];assert operations()==['sms:24']
    already=helper('sms-delete','0','7',PDU,SIM,scenario='delete-already-gone');assert already['verified'] and operations()==['sms:24']
    still=helper('sms-delete','0','7',PDU,SIM,scenario='delete-still-present');assert not still['ok'] and not still['deleted'] and not still['uncertain']
    failed=helper('sms-delete','0','7',PDU,SIM,scenario='delete-verify-error');assert not failed['ok'] and failed['uncertain']
    reused=helper('sms-delete','0','7',PDU,SIM,scenario='delete-reused');assert reused['verified']
    assert operations().count('sms:24')==4, 'confirmation reads must not repeat deletion'
    before_send=len(operations())
    assert not helper('sms-send','a'*32,PDU[:-2],SIM)['ok'];assert len(operations())==before_send
    assert helper('sms-send','b'*32,PDU,SIM)['ok'];assert helper('sms-send','b'*32,PDU,SIM)['ok'];assert operations().count('sms:20')==1
    timed=helper('sms-send','c'*32,PDU,SIM,scenario='timeout');assert timed['uncertain']
    assert helper('sms-send','c'*32,PDU,SIM)['uncertain'];assert operations().count('sms:20')==2, 'uncertain send must not retry'
    # Read-only audio checks require the persistent watcher, tested separately.
    assert not helper('call-audio-check')['ok']
    assert helper('call-list')['calls']==[]
    assert not helper('call-dial','13800138000','d'*32,scenario='incoming')['ok']
    assert helper('call-dial','13800138000','e'*32)['ok'];assert helper('call-dial','13800138000','e'*32)['ok'];assert operations().count('voice:20')==1
    assert helper('call-answer','4','f'*32,scenario='incoming')['ok']
    # Hangup has no fallback to starting a new QMI client without the watcher.
    before=operations().count('voice:21')
    assert not helper('call-end','4','f'*32,scenario='conversation')['ok']
    assert not helper('call-end','invalid','f'*32)['ok']
    assert operations().count('voice:21')==before
    # Exercise actual CGI authentication, method gates, CSRF and fixed exec path.
    (base/'www/cgi-bin').mkdir(parents=True);shutil.copy(ROOT/'build/comm-native',base/'www/cgi-bin/qdc-comm')
    subprocess.run([str(ROOT/'build/api-native'),'--init'],input=b'local-test-password-123\n',stdout=subprocess.DEVNULL,env=env,check=True)
    def api(action,body=None,cookie='',method='POST'):
        raw=json.dumps(body or {},ensure_ascii=False).encode()
        settings={**env,'REQUEST_METHOD':method,'PATH_INFO':'/'+action,'CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(raw)),'HTTP_COOKIE':cookie}
        result=subprocess.check_output([str(ROOT/'build/api-native')],input=raw,env=settings).decode()
        head,data=result.split('\r\n\r\n') if '\r\n\r\n' in result else result.split('\n\n')
        return head,json.loads(data)
    before=len(operations());assert '401' in api('sms-delete',{'storage':'0','index':'7','pdu':PDU,'sim':SIM})[0]
    assert '401' in api('radio',method='GET')[0]
    head,login=api('login',{'username':'admin','password':'local-test-password-123'});cookie=next(l for l in head.splitlines() if l.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0]
    assert '403' in api('sms-delete',{'storage':'0','index':'7','pdu':PDU,'sim':SIM},cookie)[0]
    assert '405' in api('call-dial',{},cookie,method='GET')[0]
    assert '405' in api('radio',{},cookie)[0]
    head,result=api('radio',cookie=cookie,method='GET');assert result['complete'] and result['network']['operator']=='中国移动'
    assert len(operations())==before
    head,result=api('sms-list',{'csrfToken':login['csrfToken'],'cursor':'0'},cookie);assert result['ok'] and result['sim']==SIM
print('Communications checks passed: stale SIM/slot rejection, PDU validation, retry deduplication, call state, auth/CSRF/method gates.')
