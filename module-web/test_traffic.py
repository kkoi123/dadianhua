"""Accounting and authenticated API tests using synthetic counters, never a modem."""
import concurrent.futures, json, os, pathlib, shutil, subprocess, tempfile, time
ROOT=pathlib.Path(__file__).resolve().parent
(ROOT/'build/traffic-test.c').write_text('''#include "traffic.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
int main(int argc,char **argv){QdcTraffic t;if(!qdc_traffic_update(&t,argc>1&&!strcmp(argv[1],"reset"),argc>1&&!strcmp(argv[1],"flush")))return 1;printf("%" PRIu64 " %" PRIu64 "\\n",t.received,t.sent);return 0;}
''')
subprocess.run(['clang','-std=c11','-O2','-Wall','-Wextra','-Werror','-DTRAFFIC_TEST','-I'+str(ROOT),str(ROOT/'build/traffic-test.c'),str(ROOT/'traffic.c'),'-o',str(ROOT/'build/traffic-test')],check=True)
with tempfile.TemporaryDirectory(prefix='qdc-traffic-') as tmp:
    base=pathlib.Path(tmp); (base/'private').mkdir(mode=0o700); (base/'run').mkdir(mode=0o700)
    (base/'sys').mkdir(); (base/'proc/sys/kernel/random').mkdir(parents=True)
    boot=base/'proc/sys/kernel/random/boot_id';boot.write_text('00000000-0000-0000-0000-000000000001\n')
    env={**os.environ,'QDC_WEB_ROOT':tmp,'QDC_WEB_RUN':str(base/'run'),'QDC_WEB_PROC_ROOT':str(base/'proc'),'QDC_TRAFFIC_SYS':str(base/'sys'),'QDC_TRAFFIC_TIME':'100'}
    def link(rx,tx,index=4,name='rmnet_data0'):
        p=base/'sys'/name;(p/'statistics').mkdir(parents=True,exist_ok=True)
        for f,n in [('ifindex',index),('statistics/rx_bytes',rx),('statistics/tx_bytes',tx)]: (p/f).write_text(str(n)+'\n')
    def sample(action='',expected=None):
        r=subprocess.run([str(ROOT/'build/traffic-test')]+([action] if action else []),env=env,capture_output=True)
        assert r.returncode==0,r.stderr
        result=tuple(map(int,r.stdout.split()))
        if expected is not None:assert result==expected,(result,expected)
        return result
    link(1000,500);link(10**12,10**12,name='rmnet_ipa0');link(10**12,10**12,name='rmnet0');link(10**12,10**12,name='usb0')
    sample(expected=(0,0));link(1300,650);sample(expected=(300,150));sample(expected=(300,150))
    link(1500,700);(base/'run/traffic').unlink();sample(expected=(500,200)) # recover raw delta on same boot
    link(40,20);sample(expected=(540,220)) # driver counter reset
    link(25,10,index=7);sample(expected=(565,230)) # new ifindex
    shutil.rmtree(base/'sys/rmnet_data0');sample(expected=(565,230))
    link(45,15,index=7);sample(expected=(585,235)) # disappearing link retains its baseline
    link(2**33+45,2**32+15,index=7);sample('flush',expected=(2**33+585,2**32+235))
    (base/'run/traffic').unlink();boot.write_text('00000000-0000-0000-0000-000000000002\n');env['QDC_TRAFFIC_TIME']='2'
    link(20,10,index=4);sample(expected=(2**33+605,2**32+245)) # power-cycle persisted totals + new boot
    old=(base/'run/traffic').read_bytes();sample('reset',expected=(0,0))
    (base/'run/traffic').write_bytes(old);sample(expected=(0,0)) # killed reset between durable and RAM commit
    link(120,80,index=4);sample(expected=(100,70))
    # Concurrent samplers either win the lock or retry; totals are counted once.
    link(220,180,index=4)
    def parallel(_):
        for _ in range(20):
            r=subprocess.run([str(ROOT/'build/traffic-test')],env=env,capture_output=True)
            if r.returncode==0:return tuple(map(int,r.stdout.split()))
            time.sleep(.005)
        raise AssertionError('lock failed to recover')
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        assert all(x==(200,170) for x in pool.map(parallel,range(24)))
    env['QDC_TRAFFIC_TIME']='40';sample(expected=(200,170));(base/'run/traffic').unlink();sample(expected=(200,170))
    # Damaged state is an error, never implicit zeroing.
    saved=(base/'private/traffic').read_bytes();(base/'private/traffic').write_bytes(b'traffic-v1 truncated')
    assert subprocess.run([str(ROOT/'build/traffic-test')],env=env,capture_output=True).returncode==1
    (base/'private/traffic').write_bytes(saved)
    # Reuse the real CGI credential/session/CSRF code.
    subprocess.run([str(ROOT/'build/api-native'),'--init'],input=b'traffic-test-password\n',env=env,stdout=subprocess.DEVNULL,check=True)
    def api(action,body=None,cookie='',method='GET'):
        raw=json.dumps(body or {}).encode();e={**env,'REQUEST_METHOD':method,'PATH_INFO':'/'+action,'CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(raw)),'HTTP_COOKIE':cookie}
        r=subprocess.check_output([str(ROOT/'build/api-native')],input=raw,env=e).decode()
        h,d=r.split('\r\n\r\n') if '\r\n\r\n' in r else r.split('\n\n');return h,json.loads(d)
    assert '401' in api('traffic')[0]
    h,d=api('login',{'username':'admin','password':'traffic-test-password'},method='POST')
    cookie=next(x for x in h.splitlines() if x.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0]
    assert '405' in api('traffic-reset',cookie=cookie)[0]
    assert '403' in api('traffic-reset',{},cookie,method='POST')[0]
    h,d=api('traffic',cookie=cookie);assert d['receivedBytes']==200 and d['sentBytes']==170
    h,d=api('traffic-reset',{'csrfToken':d.get('csrfToken','invalid')},cookie,method='POST');assert '403' in h
    csrf=api('session',cookie=cookie)[1]['csrfToken']
    h,d=api('traffic-reset',{'csrfToken':csrf},cookie,method='POST');assert d['receivedBytes']==d['sentBytes']==0
    # Existing entry master samples without any web request and flushes on stop.
    import socket
    with socket.socket() as probe:probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    p=subprocess.Popen([str(ROOT/'build/entry-native'),'https://example.test'],env={**env,'ENTRY_PORT':str(port)},stderr=subprocess.DEVNULL)
    try:
        time.sleep(.3);link(320,280,index=4);time.sleep(5.5)
        state=(base/'run/traffic').read_text().split();assert tuple(map(int,state[5:7]))==(100,100),state
    finally:p.terminate();p.wait(timeout=3)
    (base/'run/traffic').unlink();sample(expected=(100,100))
print('Traffic passed: first baseline, deltas/64-bit, link reset/recreation, cold boot, durable reset recovery, concurrent samplers, corruption refusal, auth/CSRF and collection with page closed.')
