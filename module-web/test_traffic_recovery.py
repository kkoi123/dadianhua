"""Power-loss and shutdown regressions; synthetic local files only."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import selectors
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent
passed = []
with tempfile.TemporaryDirectory(prefix='qdc-traffic-recovery-') as temporary:
    work = Path(temporary)
    shim = work / 'runner.c'
    shim.write_text('''#define _POSIX_C_SOURCE 200809L
#include "traffic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
pid_t recovery_getpid(void) { return 4242; }
int recovery_rename(const char *from, const char *to) {
    int disk = strstr(to, "/private/traffic") != NULL;
    if (disk && getenv("CRASH_BEFORE_RENAME")) _exit(99);
    int result = rename(from, to);
    if (!result && disk && getenv("CRASH_AFTER_RENAME")) _exit(99);
    return result;
}
int main(int argc, char **argv) {
    QdcTraffic t;
    if (!qdc_traffic_update(&t, argc > 1 && !strcmp(argv[1], "reset"),
                                 argc > 1 && !strcmp(argv[1], "flush"))) return 1;
    printf("%" PRIu64 " %" PRIu64 "\\n", t.received, t.sent);
    return 0;
}
''')
    obj, binary, entry = work / 'traffic.o', work / 'runner', work / 'entry'
    common = ['clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT)]
    subprocess.run(common + ['-DTRAFFIC_TEST', '-Dgetpid=recovery_getpid',
        '-Drename=recovery_rename', '-c', str(ROOT / 'traffic.c'), '-o', str(obj)], check=True)
    subprocess.run(common + [str(shim), str(obj), '-o', str(binary)], check=True)
    subprocess.run(common + ['-DENTRY_TEST', '-DTRAFFIC_TEST', str(ROOT / 'entry.c'),
        str(ROOT / 'traffic.c'), '-o', str(entry)], check=True)

    def fixture(name):
        base = work / name
        (base / 'private').mkdir(parents=True, mode=0o700)
        (base / 'run').mkdir(mode=0o700)
        (base / 'proc/sys/kernel/random').mkdir(parents=True)
        (base / 'sys/rmnet_data0/statistics').mkdir(parents=True)
        (base / 'proc/sys/kernel/random/boot_id').write_text('00000000-0000-0000-0000-000000000001\n')
        env = {**os.environ, 'QDC_WEB_ROOT': str(base), 'QDC_WEB_RUN': str(base / 'run'),
               'QDC_WEB_PROC_ROOT': str(base / 'proc'), 'QDC_TRAFFIC_SYS': str(base / 'sys'),
               'QDC_TRAFFIC_TIME': '100'}
        return base, env

    def counters(base, rx, tx):
        for name, value in [('ifindex', 4), ('statistics/rx_bytes', rx), ('statistics/tx_bytes', tx)]:
            (base / 'sys/rmnet_data0' / name).write_text(str(value) + '\n')

    def run(env, action='', expected=None, code=0):
        p = subprocess.run([str(binary)] + ([action] if action else []), env=env,
                           capture_output=True, text=True, timeout=4)
        assert p.returncode == code, (p.returncode, p.stderr)
        if expected is not None:
            assert tuple(map(int, p.stdout.split())) == expected, (p.stdout, expected)

    def state(base, ram=False):
        fields = (base / ('run/traffic' if ram else 'private/traffic')).read_text().split()
        return tuple(map(int, fields[5:7]))

    def reboot(base, env, rx, tx):
        shutil.rmtree(base / 'run')
        (base / 'run').mkdir(mode=0o700)
        (base / 'proc/sys/kernel/random/boot_id').write_text('00000000-0000-0000-0000-000000000002\n')
        env['QDC_TRAFFIC_TIME'] = '2'
        counters(base, rx, tx)

    def entry_ready(process):
        # Newly compiled executables can take time to pass macOS validation.
        # Wait for the actual listener/installed signal handler, not a sleep.
        with selectors.DefaultSelector() as ready:
            ready.register(process.stderr, selectors.EVENT_READ)
            assert ready.select(timeout=5), 'Entry failed to reach startup'
        assert b'listening on loopback:' in process.stderr.readline()

    base, env = fixture('ordinary-power-loss')
    counters(base, 1000, 500); run(env, expected=(0, 0))
    counters(base, 1300, 650); run(env, 'flush', expected=(300, 150))
    counters(base, 1500, 750); env['QDC_TRAFFIC_TIME'] = '125'; run(env, expected=(500, 250))
    reboot(base, env, 20, 10); run(env, expected=(320, 160)); run(env, expected=(320, 160))
    passed.append('Saved totals restore once after power loss; uncheckpointed delta is not invented')

    base, env = fixture('crash-before-rename')
    counters(base, 1000, 500); run(env)
    counters(base, 1300, 650); run(env, 'flush', expected=(300, 150))
    old = (base / 'private/traffic').read_bytes()
    counters(base, 1800, 950)
    run({**env, 'CRASH_BEFORE_RENAME': '1'}, 'flush', code=99)
    orphan = list((base / 'private').glob('traffic.tmp.*'))
    assert len(orphan) == 1 and (base / 'private/traffic').read_bytes() == old
    # Also emulate an existing backup from the pre-fix PID-only implementation.
    legacy = base / 'private/traffic.tmp.4242'
    legacy.write_text('truncated legacy checkpoint'); legacy.chmod(0o600)
    target = base / 'outside'; target.write_text('must remain unchanged')
    unsafe_link = base / 'private/traffic.tmp.safe00'; unsafe_link.symlink_to(target)
    hard = base / 'private/traffic.tmp.hard00'; os.link(target, hard)
    public = base / 'private/traffic.tmp.open00'; public.write_text('do not delete'); public.chmod(0o644)
    unrelated = base / 'private/traffic.tmp.notes.txt'; unrelated.write_text('unrelated')
    reboot(base, env, 20, 10)
    for tick in [2, 7, 12, 37]:
        env['QDC_TRAFFIC_TIME'] = str(tick); run(env, expected=(320, 160))
    assert not orphan[0].exists() and not legacy.exists()
    assert unsafe_link.is_symlink() and hard.exists() and public.exists() and unrelated.exists()
    assert target.read_text() == 'must remain unchanged'
    assert (base / 'private/traffic').stat().st_mode & 0o777 == 0o600
    passed.append('Interrupted new/legacy temporary files recover without deleting symlinks, hardlinks or unrelated files')

    base, env = fixture('commit-before-ram')
    counters(base, 1000, 500); run(env)
    counters(base, 1300, 650)
    run({**env, 'CRASH_AFTER_RENAME': '1'}, 'flush', code=99)
    run(env, expected=(300, 150)); run(env, expected=(300, 150))
    old_ram = (base / 'run/traffic').read_bytes()
    run({**env, 'CRASH_AFTER_RENAME': '1'}, 'reset', code=99)
    (base / 'run/traffic').write_bytes(old_ram)
    run(env, expected=(0, 0))
    reboot(base, env, 20, 10); run(env, expected=(20, 10)); run(env, expected=(20, 10))
    passed.append('Committed checkpoint/reset wins over stale RAM after interrupted rename sequence')

    base, env = fixture('lock-contention')
    counters(base, 1000, 500); run(env)
    counters(base, 1300, 650); run(env, expected=(300, 150))
    with (base / 'run/traffic.lock').open('r+b') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        started = time.monotonic(); run(env, code=1)
        assert time.monotonic() - started < .4
        p = subprocess.Popen([str(binary), 'flush'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            time.sleep(.15); assert p.poll() is None
            fcntl.flock(lock, fcntl.LOCK_UN)
            out, err = p.communicate(timeout=3)
            assert p.returncode == 0 and tuple(map(int, out.split())) == (300, 150), err
        finally:
            if p.poll() is None: p.kill(); p.wait()
        assert state(base) == (300, 150)
        fcntl.flock(lock, fcntl.LOCK_EX)
        started = time.monotonic(); run(env, 'flush', code=1)
        assert 1.4 <= time.monotonic() - started < 3
    passed.append('Final flush waits for released lock, ordinary samples stay nonblocking and stuck lock times out')

    base, env = fixture('entry-shutdown')
    counters(base, 1000, 500); run(env)
    counters(base, 1300, 650); run(env, expected=(300, 150))
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
    p = subprocess.Popen([str(entry), 'https://example.test'], env={**env, 'ENTRY_PORT': str(port)},
                         stderr=subprocess.PIPE)
    try:
        entry_ready(p)
        with (base / 'run/traffic.lock').open('r+b') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX); p.terminate(); time.sleep(.15)
            assert p.poll() is None, (p.returncode, p.stderr.read())
            fcntl.flock(lock, fcntl.LOCK_UN)
        _, err = p.communicate(timeout=3)
        assert p.returncode == 0, err
        assert state(base) == (300, 150)
    finally:
        if p.poll() is None: p.kill(); p.wait()
    # Persistent lock contention must produce an explicit failed exit/log.
    p = subprocess.Popen([str(entry), 'https://example.test'], env={**env, 'ENTRY_PORT': str(port)},
                         stderr=subprocess.PIPE)
    try:
        entry_ready(p)
        with (base / 'run/traffic.lock').open('r+b') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX); p.terminate()
            _, err = p.communicate(timeout=3)
        assert p.returncode == 1 and b'Final traffic checkpoint failed:' in err, err
    finally:
        if p.poll() is None: p.kill(); p.wait()
    passed.append('Actual entry SIGTERM saves after contention or reports bounded flush failure with nonzero exit')

report = {'scope': 'local synthetic counters only; no module operations', 'passed': passed}
(ROOT / 'build/traffic-recovery-fix-test.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
print(json.dumps(report, ensure_ascii=False, indent=2))
