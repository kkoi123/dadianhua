#!/usr/bin/env python3
"""Offline ES10c/QMI tests. Synthetic card data; no hardware access."""
from pathlib import Path
import json, os, subprocess, tempfile, shutil
ROOT=Path(__file__).resolve().parent
def tlv(tag, data):
    tag=tag.to_bytes((tag.bit_length()+7)//8,'big')
    n=len(data); length=bytes([n]) if n<128 else b'\x81'+bytes([n]) if n<256 else b'\x82'+n.to_bytes(2,'big')
    return tag+length+data
def profile(iccid, enabled, name, provider):
    raw=bytes(int(iccid[i+1]+iccid[i],16) for i in range(0,20,2))
    return tlv(0xe3,tlv(0x5a,raw)+tlv(0x9f70,bytes([enabled]))+tlv(0x90,name.encode())+tlv(0x91,provider.encode()))
with tempfile.TemporaryDirectory() as tmp:
    base=Path(tmp);run=base/'run';run.mkdir(mode=0o700);(base/'private').mkdir(mode=0o700)
    lib=base/'qmi.dylib'
    subprocess.run(['clang','-dynamiclib','-std=c11',str(ROOT/'test_esim_mock.c'),'-o',str(lib)],check=True)
    log=base/'operations'
    env={**os.environ,'QDC_WEB_ROOT':str(base),'QDC_WEB_RUN':str(run),'QDC_TEST_LIBRARY':str(lib),'QDC_ESIM_FIXTURES':str(base),'QDC_ESIM_LOG':str(log)}
    eid=tlv(0xbf3e,tlv(0x5a,bytes.fromhex('89000000000000000000000000000001')))
    profiles=tlv(0xbf2d,tlv(0xa0,profile('89860000000000000001',1,'测试卡片 <script>','测试运营商')+profile('89860000000000000002',0,'备用卡','另一运营商')))
    info=tlv(0xbf22,tlv(0x83,b'\x01\x02\x03')+tlv(0x84,tlv(0x82,b'\x01\x00\x00')))
    originals={'eid':eid,'profiles':profiles,'info':info}
    for name, data in originals.items():(base/name).write_bytes(data)
    def helper(scenario='normal'):
        log.write_text('')
        result=json.loads(subprocess.check_output([str(ROOT/'build/comm-native'),'esim-list'],env={**env,'QDC_TEST_SCENARIO':scenario},timeout=40))
        return result,log.read_text().splitlines()
    result,ops=helper();assert result['state']=='ready'
    card=result['cards'][0];assert card['eid']=='89000000000000000000000000000001'
    assert card['firmware']=='1.2.3' and card['freeBytes']==65536
    assert [p['enabled'] for p in card['profiles']]==[True,False]
    assert card['profiles'][0]['iccid']=='89860000000000000001'
    assert card['profiles'][0]['name']=='测试卡片 <script>'
    assert ops[-1]=='close' and 'get-response' in ops
    result,ops=helper('fallback');assert result['ok'] and ops.count('open')==2 and ops[-1]=='close'
    result,ops=helper('unsupported');assert result['state']=='unsupported' and result['cards']==[] and ops==['open']*10
    for scenario in ['incoming','conversation','call-check-failure','radio-no-sim','radio-pin']:
        result,ops=helper(scenario);assert not result['ok'] and not ops,scenario
    for scenario in ['apdu-transport','close-error']:
        result,ops=helper(scenario);assert not result['ok'] and ops[-1]=='close',scenario
    result,ops=helper('open-transport');assert not result['ok'] and ops==['open']
    result,ops=helper('info-unsupported');assert result['ok'] and result['cards'][0]['freeBytes'] is None and ops[-1]=='close'
    for data in [b'\xbf\x2d\x80',profiles[:-1],profiles+b'\x00',tlv(0xbf2d,tlv(0x81,b'\x01')),tlv(0xbf2d,tlv(0xa0,profile('89860000000000000001',2,'坏状态','测试')))]:
        (base/'profiles').write_bytes(data);result,ops=helper();assert not result['ok'] and ops[-1]=='close'
    (base/'profiles').write_bytes(tlv(0xbf2d,tlv(0xa0,b'')));result,ops=helper();assert result['ok'] and result['cards'][0]['profiles']==[]
    (base/'eid').write_bytes(tlv(0xbf3e,tlv(0x5a,b'\xff'*16)));result,ops=helper();assert not result['ok'] and ops[-1]=='close'
    for name,data in originals.items():(base/name).write_bytes(data)
    # The real CGI gates access before opening a card channel.
    (base/'www/cgi-bin').mkdir(parents=True);shutil.copy(ROOT/'build/comm-native',base/'www/cgi-bin/qdc-comm')
    subprocess.run([str(ROOT/'build/api-native'),'--init'],input=b'local-test-password-123\n',stdout=subprocess.DEVNULL,env=env,check=True)
    def api(action,body=None,cookie='',method='POST'):
        raw=json.dumps(body or {}).encode();settings={**env,'REQUEST_METHOD':method,'PATH_INFO':'/'+action,'CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(raw)),'HTTP_COOKIE':cookie}
        result=subprocess.check_output([str(ROOT/'build/api-native')],input=raw,env=settings).decode()
        head,data=result.replace('\r\n','\n').split('\n\n',1);return head,json.loads(data)
    log.write_text('');assert '401' in api('esim-list')[0] and not log.read_text()
    head,login=api('login',{'username':'admin','password':'local-test-password-123'})
    cookie=next(l for l in head.splitlines() if l.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0]
    assert '403' in api('esim-list',cookie=cookie)[0] and not log.read_text()
    assert '405' in api('esim-list',cookie=cookie,method='GET')[0]
    head,result=api('esim-list',{'csrfToken':login['csrfToken']},cookie);assert result['state']=='ready' and '200' in head
print('eSIM checks passed: segmented responses, channel fallback/cleanup, busy/PIN guards, malformed data, authentication, CSRF and method gates.')
