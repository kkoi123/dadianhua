"""Exercise actual authenticated CGI against a fake vendor, without real binding."""
import base64,json,os,pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as tmp:
    b=pathlib.Path(tmp)
    for name in ['private','run']: (b/name).mkdir(mode=0o700)
    fake=b/'curl';fake.write_text('''#!/usr/bin/env python3
import json,os,sys,pathlib,base64
b=pathlib.Path(os.environ['QDC_WEB_ROOT']); mode=(b/'mode').read_text();url=sys.argv[-1]
with (b/'requests').open('a') as f:f.write(url+'\\n')
if url.endswith('/getsn'): print(json.dumps({'result_code':0,'data':{'device_sn':'phd-test','device_sn_pwd':'SECRET-DO-NOT-LEAK','status':0 if mode=='offline' else 1}}))
elif url.endswith('/getmgrurl'):
 if mode=='status-fail': sys.exit(22)
 print(json.dumps({'result_code':0,'data':{'status':0,'account':'my-account' if mode=='bound' else '', 'url':'https://console.oray.com/?private=SECRET'}}))
elif url=='https://hsk-api.oray.com/devices/qrcode':
 data=json.load(sys.stdin); assert data=={'sn':'phd-test','password':'SECRET-DO-NOT-LEAK'}
 if mode=='bad-password':print('{"code":401002}')
 else: print(json.dumps({'qrcodeimg':'https://attacker.test/qr' if mode=='ssrf' else 'https://cphapi.oray.net/qr?id=private-token','ttl':300}))
elif url.startswith('https://cphapi.oray.net/'):
 sys.stdout.buffer.write(b'bad image' if mode=='bad-image' else base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jVt8AAAAASUVORK5CYII='))
else:raise AssertionError(url)
''');fake.chmod(0o700);(b/'mode').write_text('unbound')
    env={**os.environ,'QDC_WEB_ROOT':tmp,'QDC_WEB_RUN':str(b/'run'),'QDC_ORAY_TEST_CURL':str(fake)}
    subprocess.run([str(ROOT/'build/api-native'),'--init'],input=b'fake-test-password\n',env=env,stdout=subprocess.DEVNULL,check=True)
    def api(name,method='GET',body=None,cookie=''):
        raw=json.dumps(body or {}).encode();e={**env,'REQUEST_METHOD':method,'PATH_INFO':'/'+name,'CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(raw)),'HTTP_COOKIE':cookie}
        out=subprocess.check_output([str(ROOT/'build/api-native')],input=raw,env=e).decode().replace('\r\n','\n')
        assert 'SECRET' not in out and 'private-token' not in out,'Credentials leaked'
        h,p=out.split('\n\n',1);return int(h.splitlines()[0].split()[1]),h,json.loads(p)
    assert api('oray-status')[0]==401
    assert api('oray-qr','POST')[0]==401
    _,h,s=api('login','POST',{'username':'admin','password':'fake-test-password'})
    cookie=next(x for x in h.splitlines() if x.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0];csrf=s['csrfToken']
    def qr():return api('oray-qr','POST',{'csrfToken':csrf},cookie)
    assert api('oray-qr',cookie=cookie)[0]==405
    assert api('oray-status','POST',cookie=cookie)[0]==405
    assert api('oray-qr','POST',{},cookie)[0]==403
    assert not (b/'requests').exists()
    code,_,s=api('oray-status',cookie=cookie);assert code==200 and s['online'] and s['bindingKnown'] and not s['bound']
    (b/'mode').write_text('bound');assert api('oray-status',cookie=cookie)[2]['account']=='my-account'
    (b/'mode').write_text('status-fail');s=api('oray-status',cookie=cookie)[2];assert not s['bindingKnown'] and s['rpcAvailable']
    (b/'mode').write_text('offline');assert qr()[0]==503
    (b/'mode').write_text('unbound');code,_,s=qr();assert code==200 and s['expiresIn']==300 and base64.b64decode(s['image'].split(',')[1]).startswith(b'\x89PNG')
    assert qr()[0]==429
    for mode,code in [('ssrf',503),('bad-image',503),('bad-password',409)]:
        (b/'run/oray-binding.lock').write_text('0\n');(b/'mode').write_text(mode);assert qr()[0]==code
    assert 'https://attacker.test/qr' not in (b/'requests').read_text()
    assert (b/'run/oray-binding.lock').stat().st_mode&0o777==0o600
print('Oray binding CGI: authentication, CSRF, official host restriction, secret redaction, PNG, expiry and throttling passed.')
