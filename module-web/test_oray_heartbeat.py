"""Exercise heartbeat edits/restarts against a fake SDK, never a live modem."""
import json, os, pathlib, shutil, subprocess, tempfile, time

root = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='qdc-heartbeat-') as tmp:
    base = pathlib.Path(tmp)
    for name in ['private', 'run', 'standalone', 'www/cgi-bin']:
        (base/name).mkdir(parents=True, mode=0o700)
    shutil.copy(root/'build/api-native', base/'www/cgi-bin/api')
    config = base/'private/module-oray.json'
    original = {'auth': {'password': 'must-not-leak', 'nonce': '\\"秘密'},
                'sn': 'keep-device', 'forward': [{'port': 8788}],
                'fwconnkeepalive': 123, 'IPV6': True}
    def write_config(value):
        config.write_text(json.dumps(value, ensure_ascii=False)); config.chmod(0o600)
    write_config(original)
    (base/'private/audio-origin').write_text('https://example.com\n')
    (base/'private/audio-origin').chmod(0o600)
    (base/'calls').write_text('[]')
    (base/'running').touch()
    helper = base/'www/cgi-bin/qdc-comm'
    helper.write_text('''#!/usr/bin/env python3
import os,pathlib
r=pathlib.Path(os.environ['QDC_WEB_ROOT'])
calls=(r/'calls').read_text()
print('{"ok":false}' if calls=='unavailable' else '{"ok":true,"calls":'+calls+'}')
'''); helper.chmod(0o700)
    service = base/'standalone/service.sh'
    service.write_text('''#!/usr/bin/env python3
import os,pathlib,sys,json
r=pathlib.Path(os.environ['QDC_WEB_ROOT']);cmd=sys.argv[1]
with (r/'commands').open('a') as f:f.write(cmd+'\\n')
if cmd=='tunnel-status':print('running' if (r/'running').exists() else 'stopped')
elif cmd=='tunnel-stop':
 (r/'running').unlink(missing_ok=True)
 if (r/'sdk-update').exists():
  p=r/'private/module-oray.json';v=json.loads(p.read_text());v['auth']['nonce']='sdk-latest';p.write_text(json.dumps(v));p.chmod(0o600)
elif cmd=='tunnel-apply':
 if (r/'fail-once').exists():
  (r/'fail-once').unlink();sys.exit(1)
 policy=r/'private/tunnel-policy'
 if not policy.exists() or policy.read_text().strip()=='1':(r/'running').touch()
else:sys.exit(2)
''')
    (base/'service-fixture.py').write_text(service.read_text())
    service.write_text('#!/bin/sh\nexec python3 "$QDC_WEB_ROOT/service-fixture.py" "$@"\n'); service.chmod(0o700)
    env = {**os.environ, 'QDC_WEB_ROOT': tmp, 'QDC_WEB_RUN': str(base/'run')}
    binary = str(root/'build/api-native')
    subprocess.run([binary, '--init'], input=b'heartbeat-test-password\n', env=env, stdout=subprocess.DEVNULL, check=True)
    def api(name, body=None, cookie='', method='GET'):
        raw = json.dumps(body or {}).encode()
        e = {**env, 'REQUEST_METHOD': method, 'PATH_INFO': '/'+name,
             'CONTENT_TYPE': 'application/json', 'CONTENT_LENGTH': str(len(raw)), 'HTTP_COOKIE': cookie}
        out = subprocess.check_output([binary], input=raw, env=e).decode().replace('\r\n', '\n')
        head, payload = out.split('\n\n', 1)
        return head, json.loads(payload)
    assert '401' in api('oray-heartbeat')[0]
    assert '401' in api('oray-heartbeat-set', {}, method='POST')[0]
    head, login = api('login', {'username': 'admin', 'password': 'heartbeat-test-password'}, method='POST')
    cookie = next(l for l in head.splitlines() if l.startswith('Set-Cookie:')).split(': ', 1)[1].split(';', 1)[0]
    csrf = login['csrfToken']
    def change(tcp='60', udp='20'):
        return api('oray-heartbeat-set', {'csrfToken': csrf, 'tcpSeconds': tcp, 'udpSeconds': udp}, cookie, 'POST')
    def complete():
        for _ in range(250):
            if not (base/'run/tunnel-job').exists(): return
            time.sleep(.025)
        raise AssertionError('Worker did not finish')
    def state(): return api('oray-heartbeat', cookie=cookie)[1]
    def commands(): return (base/'commands').read_text().splitlines() if (base/'commands').exists() else []
    assert '405' in api('oray-heartbeat', {}, cookie, 'POST')[0]
    assert '405' in api('oray-heartbeat-set', cookie=cookie)[0]
    assert '403' in api('oray-heartbeat-set', {'tcpSeconds':'60','udpSeconds':'20'}, cookie, 'POST')[0]
    for bad in ['0','9','1801','-1','60.5','1e2','nan','60;reboot','']:
        assert '400' in change(bad)[0]
        assert '400' in change('60',bad)[0]
    first = state()
    assert (first['tcpSeconds'], first['forwardSeconds'], first['udpSeconds'], first['custom']) == (30,30,10,False)
    assert 'password' not in json.dumps(first) and 'must-not-leak' not in json.dumps(first)
    initial = config.read_bytes()
    assert change('30','10')[1]['unchanged'] and config.read_bytes() == initial and not commands()
    (base/'calls').write_text('[{"id":1}]')
    assert '409' in change()[0] and config.read_bytes() == initial
    (base/'calls').write_text('unavailable')
    assert '409' in change()[0]
    (base/'calls').write_text('[]')
    maintenance = base/'run/standalone-maintenance';maintenance.touch();maintenance.chmod(0o600)
    assert '409' in change()[0];maintenance.unlink()
    head, result = change('1800','1800')
    assert '202' in head and result['pending'] and config.read_bytes() == initial
    assert state()['pending'] and '409' in change()[0]
    assert '409' in api('tunnel-set', {'csrfToken':csrf,'enabled':'0'}, cookie, 'POST')[0]
    assert '409' in api('data-set', {'csrfToken':csrf,'enabled':'0'}, cookie, 'POST')[0]
    complete()
    changed = json.loads(config.read_text())
    assert {k:changed[k] for k in original} == original
    assert (changed['keepalive'],changed['fwkeepalive'],changed['udpkeepalive']) == (7200000,7200000,1800000)
    assert config.stat().st_mode & 0o777 == 0o600
    assert state()['lastSuccess'] and (base/'running').exists()
    before = commands();assert change('1800','1800')[1]['unchanged'] and commands() == before
    # A call starts between acceptance and worker: no config write/restart.
    change();(base/'calls').write_text('[{"id":2}]');complete()
    assert json.loads(config.read_text()) == changed and commands() == before
    assert state()['lastSuccess'] is False
    (base/'calls').write_text('[]')
    # Service failure restores the exact original JSON and restarts it.
    (base/'fail-once').touch();change();complete()
    assert json.loads(config.read_text()) == changed and state()['lastSuccess'] is False and (base/'running').exists()
    # SDK writes newer auth during stop: read it again, retain it.
    (base/'sdk-update').touch();change('60','30');complete()
    assert json.loads(config.read_text())['auth']['nonce'] == 'sdk-latest'
    (base/'sdk-update').unlink()
    assert (state()['tcpSeconds'],state()['udpSeconds']) == (60,30)
    # Saving while service is disabled must retain the OFF policy.
    (base/'private/tunnel-policy').write_text('0\n');(base/'private/tunnel-policy').chmod(0o600)
    (base/'running').unlink();change('30','10');complete()
    assert not (base/'running').exists() and state()['lastSuccess']
    # Invalid/duplicate config cannot be edited, nor expose auth in responses.
    for raw in ['{"keepalive":1,"keepalive":2}', '{"keepalive":"120000"}', '{"udpkeepalive":-1}', '{"auth":', '{"keepalive":NaN}']:
        config.write_text(raw)
        assert '503' in api('oray-heartbeat',cookie=cookie)[0]
        assert '503' in change()[0] and config.read_text() == raw
    write_config(original);target=base/'private/auth-sentinel';target.write_text('must-not-leak');target.chmod(0o600)
    config.unlink();config.symlink_to(target)
    assert '503' in api('oray-heartbeat',cookie=cookie)[0] and '503' in change()[0]
    assert target.read_text() == 'must-not-leak'
print('Heartbeat passed: auth/CSRF, validation, defaults, no-op, delayed reconnect, call/maintenance/switch guards, TCP×4/UDP mapping, credential preservation, failure rollback, SDK writes, OFF policy, malformed config and symlinks.')
