"""Persistent-client hangup integration: simulated QMI only, no real calls."""
import fcntl,json,os,pathlib,socket,subprocess,tempfile,time
ROOT=pathlib.Path(__file__).resolve().parent
subprocess.run(['clang','-std=c11','-shared','-fPIC','-DWATCH_MOCK','-Wall','-Wextra','-Werror',str(ROOT/'test_mock_qmi.c'),'-o',str(ROOT/'build/mock-watch.dylib')],check=True)
with tempfile.TemporaryDirectory(prefix='qdc-end-') as tmp:
    root=pathlib.Path(tmp);(root/'private').mkdir(mode=0o700);(root/'run').mkdir(mode=0o700)
    mode=root/'mode';mode.write_text('conversation\n')
    env={**os.environ,'QDC_WEB_ROOT':tmp,'QDC_WEB_RUN':str(root/'run'),'QDC_TEST_LIBRARY':str(ROOT/'build/mock-watch.dylib'),'QDC_WATCH_TEST_STATE':str(mode),'QDC_TEST_LOG':str(root/'log')}
    # Deliberately missing library in every short-lived client. Hangup must
    # never call discovery or load QMI; only the existing watcher owns it.
    client_env={**env,'QDC_TEST_LIBRARY':'/nonexistent/qmi-library'}
    def end(call='4',request='a'*32):
        return json.loads(subprocess.check_output([str(ROOT/'build/comm-native'),'call-end',call,request],env=client_env,timeout=22))
    def audio_check():
        return json.loads(subprocess.check_output([str(ROOT/'build/comm-native'),'call-audio-check'],env=client_env,timeout=4))
    def count():return (root/'log').read_text().splitlines().count('voice:21') if (root/'log').exists() else 0
    def state(condition):
        for _ in range(120):
            assert process.poll() is None,'watcher exited'
            try:
                data=json.loads((root/'run/events-state').read_text())
                if condition(data):return data
            except (FileNotFoundError,json.JSONDecodeError):pass
            time.sleep(.05)
        raise AssertionError('watcher state timed out')
    def change(value):new=root/'mode.new';new.write_text(value+'\n');new.replace(mode)
    assert not end()['ok'] and count()==0
    assert not audio_check()['ok']
    process=subprocess.Popen([str(ROOT/'build/comm-native'),'watch'],env=env,stdout=subprocess.DEVNULL)
    try:
        state(lambda d:d['callsReady'] and len(d['callState']['calls'])==1)
        sockpath=root/'run/call-control.sock';assert sockpath.stat().st_mode&0o777==0o600
        active=audio_check();assert active['ok'] and active['active'] and active['callId']==4 and len(active['callKey'])==16
        for _ in range(3):assert audio_check()==active
        assert count()==0,'read-only checks must not send hangup'
        with (root/'run/comm.lock').open('r+') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX);started=time.monotonic();assert not audio_check()['ok']
            assert time.monotonic()-started<1,'busy state query must not block hangup'
            fcntl.flock(lock,fcntl.LOCK_UN)
        assert audio_check()==active
        change('call-check-failure');failed=audio_check();assert not failed['ok'] and failed['transportError']==5
        change('conversation');assert audio_check()==active
        assert not end('0')['ok'] and not end('invalid')['ok'] and not end('4','bad')['ok']
        assert not end('5')['ok'] and count()==0,'must only end the matching call'
        # An untrusted malformed connection cannot execute a QMI mutation.
        with socket.socket(socket.AF_UNIX) as bad:bad.connect(str(sockpath));bad.sendall(b'garbage');bad.shutdown(socket.SHUT_WR)
        time.sleep(.2);assert count()==0 and process.poll() is None
        # Existing lock owner can complete; the helper does not hold comm.lock
        # while waiting for the watcher, avoiding an IPC/lock deadlock.
        with (root/'run/comm.lock').open('r+') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            pending=subprocess.Popen([str(ROOT/'build/comm-native'),'call-end','4','b'*32],env=client_env,stdout=subprocess.PIPE)
            try:
                time.sleep(.3);assert pending.poll() is None and count()==0
                fcntl.flock(lock,fcntl.LOCK_UN)
                reply,_=pending.communicate(timeout=6);assert json.loads(reply)['ok'] and count()==1
            finally:
                if pending.poll() is None:pending.kill();pending.wait()
        state(lambda d:not d['callState']['calls']);assert not end('4','b'*32)['ok'] and count()==1
        assert audio_check()['active'] is False
        time.sleep(.15)  # Allow the mock indication thread to observe idle.
        change('conversation');state(lambda d:bool(d['callState']['calls']))
        with (root/'run/comm.lock').open('r+') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX);started=time.monotonic();assert not end()['ok']
            assert 4.8<=time.monotonic()-started<9 and count()==1
            fcntl.flock(lock,fcntl.LOCK_UN)
        # A queued request already expired must never execute later.
        import ctypes
        class Request(ctypes.Structure):_fields_=[('magic',ctypes.c_char*8),('deadline',ctypes.c_uint64),('call',ctypes.c_char*4),('request',ctypes.c_char*33)]
        req=Request(b'QDCEND1',1,b'4',b'c'*32)
        with socket.socket(socket.AF_UNIX) as expired:
            expired.connect(str(sockpath));expired.sendall(bytes(req));expired.shutdown(socket.SHUT_WR)
            assert not json.loads(expired.recv(1024))['ok']
        assert count()==1
    finally:process.terminate();process.wait(timeout=6)
    assert not sockpath.exists()
    # A transport error after a command returns uncertain; it is never retried.
    change('conversation');process=subprocess.Popen([str(ROOT/'build/comm-native'),'watch'],env={**env,'QDC_HANGUP_TEST_TIMEOUT':'1'},stdout=subprocess.DEVNULL)
    try:
        state(lambda d:d['callsReady'] and bool(d['callState']['calls']))
        response=end();assert not response['ok'] and response['uncertain'] and count()==2
        time.sleep(.5);assert count()==2 and process.poll() is None
    finally:process.terminate();process.wait(timeout=6)
print('Call control passed: persistent VOICE audio checks without helper QMI load, busy/failure recovery, confirmed idle, matching hangup ID, private socket, malformed/expired rejection, bounded lock wait, no automatic command retries, watcher cleanup.')
