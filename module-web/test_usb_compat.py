"""Exercise authenticated opt-in, failed loading and simulated power loss.
No kernel module or real network configuration is touched by this test.
"""
import json,os,pathlib,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='qdc-compat-') as temp:
    base=pathlib.Path(temp);(base/'run').mkdir(mode=0o700)
    params=base/'params';params.mkdir()
    (base/'sysctl').mkdir();(base/'sysctl/ipv4').write_text('1\n');(base/'sysctl/ipv6').write_text('2\n');(base/'link').write_text('1\n')
    loader=base/'insmod';unloader=base/'rmmod'
    loader.write_text('''#!/usr/bin/env python3
import os,pathlib,sys
b=pathlib.Path(os.environ['QDC_WEB_ROOT']);p=pathlib.Path(os.environ['QDC_COMPAT_TEST_PARAMS'])
assert sys.argv[1:]==[str(b/'usb-compat/qdc_usb_compat.ko'),'enabled=0']
if (b/'fail-load').exists():sys.exit(1)
(p/'enabled').write_text('N\\n')
''')
    unloader.write_text('''#!/usr/bin/env python3
import os,pathlib,sys
assert sys.argv[1:]==['qdc_usb_compat']
(pathlib.Path(os.environ['QDC_COMPAT_TEST_PARAMS'])/'enabled').unlink()
''')
    loader.chmod(0o700);unloader.chmod(0o700)
    env={**os.environ,'QDC_WEB_ROOT':temp,'QDC_WEB_RUN':str(base/'run'),
         'QDC_DATA_TEST_IP':'/usr/bin/true','QDC_DATA_TEST_SYSCTL':str(base/'sysctl'),
         'QDC_DATA_TEST_LINK':str(base/'link'),'QDC_COMPAT_TEST_PARAMS':str(params),
         'QDC_COMPAT_TEST_INSMOD':str(loader),'QDC_COMPAT_TEST_RMMOD':str(unloader)}
    binary=str(root/'build/api-native')
    subprocess.run([binary,'--init'],input=b'compat-test-password\n',env=env,stdout=subprocess.DEVNULL,check=True)
    def request(name,body=None,cookie='',method='GET'):
        raw=json.dumps(body or {}).encode();e={**env,'REQUEST_METHOD':method,'PATH_INFO':'/'+name,
            'CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(raw)),'HTTP_COOKIE':cookie}
        out=subprocess.check_output([binary],input=raw,env=e).decode().replace('\r\n','\n')
        head,payload=out.split('\n\n',1);return head,json.loads(payload)
    assert '401' in request('data-set',{'scope':'compat','enabled':'1'},method='POST')[0]
    head,login=request('login',{'username':'admin','password':'compat-test-password'},method='POST')
    cookie=next(x for x in head.splitlines() if x.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0]
    def state():return request('data',cookie=cookie)[1]
    def switch(value):return request('data-set',{'scope':'compat','enabled':value,'csrfToken':login['csrfToken']},cookie,'POST')
    assert '403' in request('data-set',{'scope':'compat','enabled':'1'},cookie,'POST')[0]
    assert not state()['compatEnabled'] and not state()['compatApplied'] and not state()['compatSupported']
    assert '503' in switch('1')[0] and not (params/'enabled').exists()
    (base/'usb-compat').mkdir();(base/'usb-compat/qdc_usb_compat.ko').write_bytes(b'test-only')
    assert state()['compatSupported'] and not (params/'enabled').exists(),'GET must not load the component'
    assert '400' in switch('1;reboot')[0]
    _,s=switch('1');assert s['compatEnabled'] and s['compatApplied']
    policy=base/'private/usb-compat-policy'
    assert policy.read_text()=='1\n' and policy.stat().st_mode&0o777==0o600
    # Kernel state is gone after power loss; policy remains in persistent storage.
    (params/'enabled').unlink();assert state()['compatEnabled'] and not state()['compatApplied']
    subprocess.run([binary,'--usb-compat-reconcile'],env=env,check=True)
    assert state()['compatApplied']
    _,s=switch('0');assert not s['compatEnabled'] and not s['compatApplied'] and not (params/'enabled').exists()
    (base/'fail-load').touch();assert '503' in switch('1')[0] and policy.read_text()=='0\n'
    (base/'fail-load').unlink()
    # A failure to save must restore the old runtime state.
    (base/'private').chmod(0o500)
    try:assert '503' in switch('1')[0] and not (params/'enabled').exists() and policy.read_text()=='0\n'
    finally:(base/'private').chmod(0o700)
    policy.write_text('bad\n');assert state()['compatEnabled'] is None
    assert subprocess.run([binary,'--usb-compat-reconcile'],env=env).returncode==1
    assert not (params/'enabled').exists()
    assert (base/'sysctl/ipv4').read_text()=='1\n' and (base/'sysctl/ipv6').read_text()=='2\n'
print('USB compatibility: auth/CSRF, default OFF, read-only status, invalid input, load failure, save rollback, power-loss restoration, corrupt policy and independent forwarding gate passed.')
