"""Local password boundaries and actual change/login flow; no device access."""
import json, os, pathlib, subprocess, tempfile
R=pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='qdc-password-') as folder:
    root=pathlib.Path(folder)
    for i,(password,valid) in enumerate([('1234567',False),('12345678',True),('一二三四五六七',False),('一二三四五六七八',True),('😀'*7,False),('😀'*8,True),('a'*129,False),('abcd\t1234',False)]):
        case=root/str(i);case.mkdir(mode=0o700)
        env={**os.environ,'QDC_WEB_ROOT':str(case),'QDC_WEB_RUN':str(case/'run')}
        result=subprocess.run([str(R/'build/api-native'),'--init'],input=(password+'\n').encode(),env=env,capture_output=True)
        assert (result.returncode==0)==valid,(i,result.stderr)
    case=root/'change';case.mkdir(mode=0o700);env={**os.environ,'QDC_WEB_ROOT':str(case),'QDC_WEB_RUN':str(case/'run')}
    subprocess.run([str(R/'build/api-native'),'--init'],input=b'old-pass-123\n',env=env,stdout=subprocess.DEVNULL,check=True)
    def api(action,body,cookie=''):
        raw=json.dumps(body).encode();settings={**env,'REQUEST_METHOD':'POST','PATH_INFO':'/'+action,'CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(raw)),'HTTP_COOKIE':cookie}
        out=subprocess.check_output([str(R/'build/api-native')],input=raw,env=settings).decode().replace('\r\n','\n');head,body=out.split('\n\n',1);return head,json.loads(body)
    head,login=api('login',{'username':'admin','password':'old-pass-123'})
    cookie=next(line for line in head.splitlines() if line.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0]
    body={'csrfToken':login['csrfToken'],'currentPassword':'old-pass-123','newPassword':'1234567','confirmPassword':'1234567'}
    assert '400' in api('password',body,cookie)[0]
    body.update(newPassword='12345678',confirmPassword='12345678')
    head,result=api('password',body,cookie);assert result['ok'] and result['reauthenticate']
    assert '401' in api('login',{'username':'admin','password':'old-pass-123'})[0]
    assert api('login',{'username':'admin','password':'12345678'})[1]['ok']
print('Password checks passed: 7/8 character boundaries, UTF-8, controls/max bytes, actual change and new login.')
