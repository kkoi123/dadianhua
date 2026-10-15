"""Application tunnel controls against a fake client; never change a real tunnel."""
import json, os, pathlib, shutil, subprocess, tempfile, time
ROOT = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='qdc-tunnel-') as tmp:
    base = pathlib.Path(tmp)
    for name in ['private', 'run', 'standalone', 'www/cgi-bin']:
        (base/name).mkdir(parents=True, mode=0o700)
    shutil.copy(ROOT/'build/api-native', base/'www/cgi-bin/api')
    for name, value in [('module-oray.json', '{"secret":"must-not-leak"}\n'), ('audio-origin', 'https://example.com\n')]:
        (base/'private'/name).write_text(value); (base/'private'/name).chmod(0o600)
    (base/'calls').write_text('[]')
    helper = base/'www/cgi-bin/qdc-comm'
    helper.write_text('''#!/usr/bin/env python3
import os,pathlib
r=pathlib.Path(os.environ['QDC_WEB_ROOT'])
print('{"ok":true,"calls":'+(r/'calls').read_text()+'}')
'''); helper.chmod(0o700)
    service = base/'standalone/service.sh'
    service.write_text('''#!/bin/sh
set -eu
root=$QDC_WEB_ROOT
case "$1" in
 tunnel-status) if [ -e "$root/running" ]; then echo running; else echo stopped; fi;;
 tunnel-apply)
  if [ -e "$root/fail" ]; then exit 1; fi
  policy=$(cat "$root/private/tunnel-policy")
  printf '%s\\n' "$policy" >> "$root/commands"
  if [ "$policy" = 1 ]; then touch "$root/running"; else rm -f "$root/running"; fi;;
 *) exit 2;;
esac
'''); service.chmod(0o700)
    env = {**os.environ, 'QDC_WEB_ROOT': tmp, 'QDC_WEB_RUN': str(base/'run')}
    subprocess.run([str(ROOT/'build/api-native'), '--init'], input=b'tunnel-test-password\n', env=env, stdout=subprocess.DEVNULL, check=True)
    def api(name, body=None, cookie='', method='GET'):
        raw=json.dumps(body or {}).encode()
        e={**env, 'REQUEST_METHOD':method, 'PATH_INFO':'/'+name, 'CONTENT_TYPE':'application/json', 'CONTENT_LENGTH':str(len(raw)), 'HTTP_COOKIE':cookie}
        out=subprocess.check_output([str(ROOT/'build/api-native')],input=raw,env=e).decode().replace('\r\n','\n')
        head,payload=out.split('\n\n',1); return head,json.loads(payload)
    def complete():
        for _ in range(200):
            if not (base/'run/tunnel-job').exists():return
            time.sleep(.05)
        raise AssertionError('worker did not complete')
    assert '401' in api('tunnel')[0]
    assert '401' in api('tunnel-set', {'enabled':'0'}, method='POST')[0]
    head,login=api('login',{'username':'admin','password':'tunnel-test-password'},method='POST')
    cookie=next(l for l in head.splitlines() if l.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0]; csrf=login['csrfToken']
    def change(value):return api('tunnel-set',{'csrfToken':csrf,'enabled':value},cookie,'POST')
    assert '405' in api('tunnel-set',cookie=cookie)[0]
    assert '405' in api('tunnel',{},cookie,'POST')[0]
    assert '403' in api('tunnel-set',{'enabled':'0'},cookie,'POST')[0]
    assert '400' in change('0;reboot')[0]
    _,state=api('tunnel',cookie=cookie)
    assert state['enabled'] and state['configured'] and state['publicURL']=='https://example.com/module/'
    assert 'secret' not in json.dumps(state) and 'must-not-leak' not in json.dumps(state)
    (base/'running').touch(); (base/'calls').write_text('[{"id":1}]')
    assert '409' in change('0')[0] and not (base/'private/tunnel-policy').exists()
    (base/'calls').write_text('[]')
    head,state=change('0'); assert '202' in head and state['pending'] and not state['enabled']
    assert (base/'running').exists(), 'Response must precede public disconnect'
    assert '409' in change('1')[0]
    assert '409' in api('data-set',{'csrfToken':csrf,'enabled':'0'},cookie,'POST')[0]
    complete(); _,state=api('tunnel',cookie=cookie)
    assert not state['enabled'] and state['state']=='stopped' and state['lastSuccess']
    assert (base/'private/tunnel-policy').stat().st_mode&0o777==0o600
    change('1'); complete(); assert api('tunnel',cookie=cookie)[1]['state']=='running'
    # A call can begin after acceptance but before the disconnect worker.
    change('0'); (base/'calls').write_text('[{"id":2}]'); complete()
    _,state=api('tunnel',cookie=cookie)
    assert state['enabled'] and state['state']=='running' and state['lastSuccess'] is False
    (base/'calls').write_text('[]'); (base/'fail').touch(); change('0'); complete()
    assert api('tunnel',cookie=cookie)[1]['lastSuccess'] is False
    (base/'fail').unlink(); change('0'); complete()
    # Client lifetime is independent of the generic kernel network gate.
    (base/'private/data-policy').write_text('0\n'); (base/'private/data-policy').chmod(0o600)
    change('1'); complete(); _,state=api('tunnel',cookie=cookie)
    assert state['enabled'] and state['state']=='running', 'Cellular policy does not start/stop applications'
    # Invalid origins cannot become browser links. SDK config is never returned.
    (base/'private/audio-origin').write_text('javascript:alert(1)\n')
    assert not api('tunnel',cookie=cookie)[1]['publicURL']
    (base/'private/tunnel-policy').unlink(); (base/'private/tunnel-policy').symlink_to(base/'private/module-oray.json')
    assert api('tunnel',cookie=cookie)[1]['enabled'] is None
    assert '503' in change('0')[0] and 'must-not-leak' in (base/'private/module-oray.json').read_text()
    # Exercise the actual shell policy guard, replacing only client operations.
    s=(ROOT/'standalone-service.sh').read_text().split('case "${1:-}" in')[0]
    s=s.replace('ROOT=/usrdata/qdc-web', 'ROOT=$QDC_WEB_ROOT').replace('RUN=/run/qdc-web', 'RUN=$QDC_WEB_RUN')
    s+='''\ntunnel_tasks() { echo '101 102 999'; }
tunnel_task() { [ "$1" = 101 ] || [ "$1" = 102 ]; }
kill() { echo "$*" >> "$ROOT/shell-commands"; }
touch "$RUN/standalone-tunnel.pid" "$RUN/module-oray-launch.pid" "$RUN/module-oray.pid"
tunnel 1
[ ! -e "$RUN/module-oray.pid" ]

'''
    (base/'private/tunnel-policy').unlink(); (base/'private/tunnel-policy').write_text('0\n'); (base/'private/tunnel-policy').chmod(0o600)
    subprocess.run(['/bin/sh'], input=s.encode(), env=env, check=True)
    assert (base/'shell-commands').read_text().splitlines()==['-STOP 101','-STOP 102','-KILL 101','-KILL 102']*2, 'Actual stop function freezes/kills only verified owned workers'
    # The real service must retain the client even with cellular OFF.
    (base/'private/tunnel-policy').write_text('1\n'); (base/'private/data-policy').write_text('0\n')
    s=s[:s.index('tunnel_tasks() { echo')]+'''\ntunnel_tasks() { echo 101; }
    tunnel_task() { [ "$1" = 101 ]; }
    alive() { task_pid=101; return 0; }
    stop_tunnel() { exit 91; }
    tunnel 1
    '''
    subprocess.run(['/bin/sh'],input=s.encode(),env=env,check=True)
print('Tunnel controls passed: authentication, CSRF, fixed commands, delayed disconnect, serialization, call guards, saved policy, independent client lifetime, actual stop function, sanitized URL/status, symlink rejection.')
