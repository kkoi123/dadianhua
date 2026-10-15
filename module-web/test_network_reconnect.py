"""Exercise the actual service recovery guard with fake client/call operations."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root/'standalone-service.sh').read_text().split('case "${1:-}" in')[0]
with tempfile.TemporaryDirectory(prefix='qdc-network-guard-') as temp:
    base=Path(temp)
    for name in ['run','private','standalone','www/cgi-bin']:
        (base/name).mkdir(parents=True,exist_ok=True)
    helper=base/'www/cgi-bin/qdc-comm'
    helper.write_text('#!/bin/sh\ncat "$QDC_WEB_ROOT/calls"\n');helper.chmod(0o700)
    (base/'private/standalone-enabled').touch()
    (base/'private/module-oray.json').write_text('unchanged-private-config')
    (base/'calls').write_text('{"ok":true,"calls":[]}')
    snapshot=base/'standalone/qdc-network-watch'
    snapshot.write_text('#!/bin/sh\nif [ -e "$QDC_WEB_ROOT/offline" ]; then echo \'{"online":false}\'; else echo \'{"online":true}\'; fi\n')
    snapshot.chmod(0o700)
    source=source.replace('ROOT=/usrdata/qdc-web','ROOT=$QDC_WEB_ROOT').replace('RUN=/run/qdc-web','RUN=$QDC_WEB_RUN')
    source+='''
flock() { [ ! -e "$ROOT/busy-lock" ]; }
verify() { return 0; }
tunnel_now=0
tunnel_pending() { [ -e "$RUN/tunnel-job" ]; }
stop_tunnel() { echo stop >> "$ROOT/actions"; }
tunnel() {
    python3 - <<'PYTEST'
import os
try: os.fstat(9)
except OSError: pass
else: raise AssertionError('Client inherited maintenance lock')
PYTEST
    echo start >> "$ROOT/actions"
}
if [ "$NETWORK_REASON" = supervised ]; then tunnel_supervised; else network_reconnect "$NETWORK_REASON"; fi
'''
    env=dict(os.environ,QDC_WEB_ROOT=temp,QDC_WEB_RUN=str(base/'run'),NETWORK_REASON='network-restored')
    def run(expected,changed=False):
        actions=base/'actions';actions.unlink(missing_ok=True)
        result=subprocess.run(['/bin/sh'],input=source.encode(),env=env,capture_output=True)
        assert result.returncode==expected,result.stderr.decode()
        assert (actions.read_text().splitlines() if actions.exists() else [])==(['stop','start'] if changed else [])
        assert (base/'private/module-oray.json').read_text()=='unchanged-private-config'
    run(0,True)
    for marker in ['standalone-maintenance','data-job','tunnel-job']:
        (base/'run'/marker).touch();run(75);(base/'run'/marker).unlink()
    (base/'busy-lock').touch();run(75);(base/'busy-lock').unlink()
    (base/'offline').touch();run(75);(base/'offline').unlink()
    for policy in ['data-policy','tunnel-policy']:
        path=base/'private'/policy;path.write_text('0\n');run(3)
        path.write_text('invalid\n');run(1);path.write_text('1\n');run(0,True)
    (base/'run/standalone-tunnel-blocked').touch();run(3);(base/'run/standalone-tunnel-blocked').unlink()
    (base/'calls').write_text('{"ok":true,"calls":[{"id":1}]}');run(75)
    (base/'calls').write_text('{"ok":false,"calls":[]}');run(75)
    (base/'calls').write_text('{"ok":true,"calls":[]}');run(0,True)
    env['NETWORK_REASON']='address-changed';run(0,True)
    env['NETWORK_REASON']='invalid';run(2)
    # The supervisor must never race recovery while another operation owns
    # the data lock, or start a client during installation/network settings.
    env['NETWORK_REASON']='supervised'
    for marker in ['standalone-maintenance','data-job']:
        (base/'run'/marker).touch();run(0);(base/'run'/marker).unlink()
    (base/'busy-lock').touch();run(0);(base/'busy-lock').unlink()
print('Recovery guards passed: OFF policies, maintenance, jobs, lock, active/error call states, reconnect, child lock release, unchanged config.')
