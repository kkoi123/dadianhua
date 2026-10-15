#!/usr/bin/env python3
"""Fault injection on temporary directories; never interrupts module power."""
import json, os, pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parent
binary=root/'build/config-store-native'
with tempfile.TemporaryDirectory() as td:
    base=pathlib.Path(td); private=base/'private'; run=base/'run'
    private.mkdir(mode=0o700);run.mkdir(mode=0o700)
    env={**os.environ,'CONFIG_STORE_PRIVATE':str(private),'CONFIG_STORE_RUN':str(run)}
    def invoke(*args,good=True,extra=None):
        p=subprocess.run([str(binary),*args],env={**env,**(extra or {})},capture_output=True,text=True)
        assert (p.returncode==0)==good,(args,p.stderr)
        assert 'private-password' not in p.stdout+p.stderr
        return p
    def write(p,s):p.write_text(s);p.chmod(0o600)
    def cfg(**extra):return json.dumps({'sn':'device-one','auth':'private-password','keepalive':7200000,**extra})
    saved=private/'module-oray.json';working=run/'module-oray.json';origin=private/'audio-origin'
    write(run/'module-oray-import.json',cfg());invoke('import')
    invoke('set-origin','https://module.example.com');invoke('recover');invoke('seed')
    assert working.read_text()==saved.read_text()
    stamp=saved.stat().st_mtime_ns; origin_stamp=origin.stat().st_mtime_ns
    for _ in range(3):invoke('checkpoint');invoke('recover');invoke('set-origin','https://module.example.com')
    assert saved.stat().st_mtime_ns==stamp and origin.stat().st_mtime_ns==origin_stamp
    for invalid in ['', '{"sn":', '{}', '{"sn":"a","auth":"b","keepalive":NaN}', '{"sn":"a","sn":"b","auth":"c"}', '{"sn":"a","auth":"b"}garbage', '{"sn":"a","auth":"b","keepalive":-1}']:
        write(working,invalid);invoke('checkpoint',good=False);assert saved.read_text()==cfg()
    write(working,cfg(auth='updated-auth'))
    invoke('checkpoint');assert saved.read_text()==cfg()  # first snapshot waits
    invoke('checkpoint');assert json.loads(saved.read_text())['auth']=='updated-auth'
    write(working,cfg(sn='wrong-device'));invoke('checkpoint-stopped',good=False)
    assert json.loads(saved.read_text())['sn']=='device-one'
    write(working,cfg(auth='latest-auth'));invoke('checkpoint-stopped')
    assert json.loads(saved.read_text())['auth']=='latest-auth'
    # Simulated cold boot: empty/broken durable files, empty RAM.
    write(saved,'');write(origin,'')
    for p in run.iterdir():p.unlink()
    invoke('recover');invoke('seed')
    assert json.loads(saved.read_text())['auth']=='latest-auth'
    assert origin.read_text()=='https://module.example.com\n'
    # Truncation in SDK RAM never damages canonical durable state.
    write(working,'');invoke('checkpoint',good=False);invoke('seed')
    assert json.loads(working.read_text())['auth']=='latest-auth'
    # Symlink targets and public permissions are refused.
    target=base/'outside';write(target,'untouched');working.unlink();working.symlink_to(target)
    invoke('checkpoint',good=False);invoke('seed',good=False);assert target.read_text()=='untouched'
    working.unlink();invoke('seed');working.chmod(0o644);invoke('checkpoint',good=False);working.chmod(0o600)
    original=origin.read_text();origin.unlink();origin.symlink_to(target)
    invoke('set-origin','https://other.example.com',good=False);assert target.read_text()=='untouched'
    origin.unlink();write(origin,original)
    # Crash before/after rename leaves either old or new complete value.
    for point in ['before-rename','after-rename']:
        invoke('set-origin','https://new.example.com',good=False,extra={'CONFIG_STORE_CRASH':point})
        assert origin.read_text() in [original,'https://new.example.com\n']
        invoke('recover')
        assert origin.read_text() in [original,'https://new.example.com\n']
        invoke('set-origin','https://module.example.com')
    for bad in ['http://module.example.com','https://module.example.com/path','https://module.example.com:443','https://bad\nvalue']:
        invoke('set-origin',bad,good=False)
    # Missing Oray identity still allows origin recovery and local management.
    saved.unlink();(private/'module-oray.json.last-good').unlink()
    invoke('recover');invoke('seed',good=False)
print('PASS: stable checkpoint, recovery, no-op writes, invalid JSON, identity guard, symlinks, simulated crash and missing-config isolation')
