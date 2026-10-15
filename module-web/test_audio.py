"""Native integration: real auth, websocket and PCM child, synthetic vendor audio.
No router/modem access, no phone calls, no microphone or recordings.
"""
import base64, hashlib, json, os, pathlib, shutil, socket, struct, subprocess, tempfile, time
R=pathlib.Path(__file__).resolve().parent
subprocess.run(['clang','-shared','-fPIC','-Wall','-Wextra','-Werror',str(R/'test_mock_audio.c'),'-o',str(R/'build/mock-audio.dylib')],check=True)
class WS:
    def __init__(self,port,origin='https://example.test'):
        self.s=socket.create_connection(('127.0.0.1',port));self.s.settimeout(8)
        key='dGhlIHNhbXBsZSBub25jZQ=='
        self.s.sendall((f'GET /audio HTTP/1.1\r\nHost: example.test\r\nOrigin: {origin}\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n\r\n').encode())
        header=b''
        while not header.endswith(b'\r\n\r\n'):header+=self.s.recv(1)
        self.status=int(header.split()[1]);
        if self.status==101:assert b's3pPLMBiTxaQ9kYGzzhZRbK+xOo=' in header
    def send(self,payload,op=1,masked=True):
        self.s.sendall(self.encode(payload,op,masked))
    def encode(self,payload,op=1,masked=True):
        if isinstance(payload,str):payload=payload.encode()
        n=len(payload);head=bytes([0x80|op,(0x80 if masked else 0)|(n if n<126 else 126)])
        if n>=126:head+=struct.pack('>H',n)
        mask=b'1234';return head+(mask if masked else b'')+(bytes(x^mask[i%4] for i,x in enumerate(payload)) if masked else payload)
    def exact(self,n):
        data=b''
        while len(data)<n:
            block=self.s.recv(n-len(data))
            if not block:raise EOFError()
            data+=block
        return data
    def read(self):
        a,b=self.exact(2);n=b&127
        if n==126:n=struct.unpack('>H',self.exact(2))[0]
        return a&15,self.exact(n)
    def closed(self):
        for i in range(900):
            op,data=self.read()
            if op==8:return struct.unpack('>H',data[:2])[0],data[2:].decode()
            if i%50==0:self.send('PING')
        raise AssertionError('connection not closed')
    def close(self):self.s.close()
with tempfile.TemporaryDirectory(prefix='qdc-audio-') as folder:
    root=pathlib.Path(folder);run=root/'run';private=root/'private';sessions=run/'sessions';binpath=root/'www/bin';cgi=root/'www/cgi-bin'
    for d in [run,private,sessions,binpath,cgi]:d.mkdir(parents=True,mode=0o700)
    sid='1'*64;gen='2'*64;csrf='3'*64
    def private_write(path,text):path.write_text(text);path.chmod(0o600)
    private_write(private/'credential',f'pbkdf2-sha256-v1 600000 {"a"*32} {"b"*64} {gen}\n')
    private_write(private/'audio-origin','https://example.test\n')
    def session():private_write(sessions/sid,f'session-v1 {2**60} {csrf} {gen}\n')
    session();state=root/'state';state.write_text('7')
    helper=cgi/'qdc-comm';helper.write_text(f'''#!/usr/bin/env python3
import json,pathlib
s=pathlib.Path({str(state)!r}).read_text().strip()
if s=='busy':
 print('{{"ok":false,"error":"busy"}}');raise SystemExit()
print(json.dumps(dict(ok=True,active=s!='0',callId=int(s),callKey=format(int(s),'016x')),separators=(',',':')))
''');helper.chmod(0o700)
    wrapper=binpath/'qdc-pcm';wrapper.write_text(f'''#!/usr/bin/env python3
import os,sys
os.execv({str(R/'build/pcm-native')!r},[{str(R/'build/pcm-native')!r},'--library',{str(R/'build/mock-audio.dylib')!r}]+sys.argv[1:])
''');wrapper.chmod(0o700)
    with socket.socket() as probe:probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    log=root/'ops';env={**os.environ,'QDC_WEB_ROOT':str(root),'QDC_WEB_RUN':str(run),'QDC_AUDIO_TEST_PORT':str(port),'QDC_AUDIO_TEST_LOG':str(log)}
    def ticket():
        body=json.dumps({'csrfToken':csrf}).encode();cgi_env={**env,'REQUEST_METHOD':'POST','PATH_INFO':'/audio-ticket','CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(body)),'HTTP_COOKIE':f'qdc_session={sid}'}
        out=subprocess.check_output([str(R/'build/api-native')],input=body,env=cgi_env).decode();assert 'Status: 200' in out,out
        return json.loads(out[out.index('{'):])['ticket']
    proc=subprocess.Popen([str(R/'build/audio-native'),'--serve'],env=env,stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            try:
                s=socket.create_connection(('127.0.0.1',port),timeout=.1);s.close();break
            except OSError:time.sleep(.02)
        denied=WS(port,'https://evil.test');assert denied.status==403;denied.close();assert not log.exists()
        invalid=WS(port);invalid.send('AUTH '+'f'*64);assert invalid.closed()[0]==4001;invalid.close();assert not log.exists()
        state.write_text('0');t=ticket();inactive=WS(port);inactive.send('AUTH '+t);assert '先接通' in inactive.closed()[1];inactive.close();assert not log.exists()
        reuse=WS(port);reuse.send('AUTH '+t);assert reuse.closed()[0]==4001;reuse.close()
        state.write_text('7');ws=WS(port);t=ticket();ws.send('AUTH '+t)
        op,data=ws.read();assert op==1 and json.loads(data)['type']=='ready',data
        busy=WS(port);assert busy.status==409;busy.close()
        frame=b'\x02'+struct.pack('>HI',1,0)+struct.pack('<160h',*[1234]*160)
        ws.send(frame,2);saw_down=False;stats=None;end=time.monotonic()+3
        while time.monotonic()<end:
            op,data=ws.read()
            if op==2:assert len(data)==327 and data[0]==1 and any(data[7:]);saw_down=True
            if op==1:
                stats=json.loads(data)
                if stats.get('type')=='stats' and stats['upNonzeroSamples']>0:break
        assert saw_down and stats['downNonzeroSamples']>0 and stats['upFrames']==1
        assert 'uplink-nonzero' in log.read_text()
        ops = log.read_text()
        assert 'hw:0,5' in ops and 'hw:0,6' in ops
        assert 'hw:0,0' not in ops and 'MultiMedia1' not in ops
        assert 'unaligned-period' not in ops
        assert stats['downFrames'] <= 65, 'fast driver flooded transport'
        # A tunnel can coalesce 200 ms of speech in one TCP read. The old
        # five-frame queue disconnected both directions on this exact burst.
        ws.s.sendall(b''.join(ws.encode(frame,2) for _ in range(10)))
        end=time.monotonic()+3
        while time.monotonic()<end:
            op,data=ws.read();assert op!=8,data
            if op==1:
                stats=json.loads(data)
                if stats.get('type')=='stats' and stats['upFrames']==11:break
        assert stats['upFrames']==11 and stats['upDroppedFrames']>0,stats
        # Failed QMI reads are distinct from a confirmed hangup. Recover after
        # several failures; never keep an unconfirmed call alive indefinitely.
        state.write_text('busy');end=time.monotonic()+6.5
        while time.monotonic()<end:
            op,data=ws.read();assert op!=8,data
            if op==1 and json.loads(data).get('type')=='stats':ws.send('PING')
        state.write_text('7');end=time.monotonic()+3
        while time.monotonic()<end:
            op,data=ws.read();assert op!=8,data
            if op==1 and json.loads(data).get('type')=='stats':ws.send('PING')
        # The unknown window is 30 s now; keep the socket alive with PING and
        # wait for the bounded fail-closed stop. closed() only PINGs sparsely,
        # which would trip the 15 s client timeout before the bound elapses.
        state.write_text('busy');end=time.monotonic()+34;code=reason=None
        while time.monotonic()<end:
            op,data=ws.read()
            if op==8:code=struct.unpack('>H',data[:2])[0];reason=data[2:].decode();break
            if op==1 and json.loads(data).get('type')=='stats':ws.send('PING')
        assert code==4000 and '无法确认' in reason;ws.close();time.sleep(.3)
        state.write_text('7');ws=WS(port);ws.send('AUTH '+ticket());assert json.loads(ws.read()[1])['type']=='ready'
        (sessions/sid).unlink();code,reason=ws.closed();assert code==4001 and '登录' in reason;ws.close();time.sleep(.3)
        ops=log.read_text();assert 'AFE_PCM_RX_Voice Mixer VoLTE=0' in ops and 'VoLTE_Tx Mixer SEC_AUX_PCM_TX_VoLTE=1' in ops
        session();ws=WS(port);ws.send('AUTH '+ticket());assert json.loads(ws.read()[1])['type']=='ready'
        state.write_text('0');code,reason=ws.closed();assert code==1000 and '电话' in reason;ws.close();time.sleep(.3)
        state.write_text('7');ws=WS(port);ws.send('AUTH '+ticket());assert json.loads(ws.read()[1])['type']=='ready';ws.send(frame,2,masked=False);assert '协议' in ws.closed()[1];ws.close();time.sleep(.3)
        expired=ticket();name=run/'audio-tickets'/expired;text=name.read_text().split();text[1]='0';private_write(name,' '.join(text)+'\n');ws=WS(port);ws.send('AUTH '+expired);assert ws.closed()[0]==4001;ws.close()
        # Real audio watchdog -> real local RPC -> persistent mock VOICE client.
        # The helper deliberately cannot load a QMI library. PCM stays synthetic.
        subprocess.run(['clang','-std=c11','-shared','-fPIC','-DWATCH_MOCK','-Wall','-Wextra','-Werror',str(R/'test_mock_qmi.c'),'-o',str(R/'build/mock-watch-audio.dylib')],check=True)
        helper.unlink();shutil.copy2(R/'build/comm-native',helper)
        mode=root/'watch-mode';mode.write_text('conversation\n')
        watcher_env={**env,'QDC_TEST_LIBRARY':str(R/'build/mock-watch-audio.dylib'),'QDC_WATCH_TEST_STATE':str(mode)}
        # Restart only the test audio service so its exec helpers inherit the
        # missing-library environment, proving they solely use the local RPC.
        proc.terminate();proc.wait(timeout=8);errors=proc.stderr.read().decode();assert 'forced stop' not in errors,errors
        proc=subprocess.Popen([str(R/'build/audio-native'),'--serve'],env={**env,'QDC_TEST_LIBRARY':'/nonexistent/qmi-library'},stderr=subprocess.PIPE)
        watcher=subprocess.Popen([str(R/'build/comm-native'),'watch'],env=watcher_env,stdout=subprocess.DEVNULL)
        try:
            for _ in range(100):
                if (run/'call-control.sock').exists():break
                time.sleep(.02)
            session();ws=WS(port);ws.send('AUTH '+ticket());assert json.loads(ws.read()[1])['type']=='ready'
            started=time.monotonic();sent=0;received=0;end=started+22
            while time.monotonic()<end:
                op,data=ws.read();assert op!=8,data
                if op==2:received+=1;ws.send(frame,2);sent+=1
                if op==1 and json.loads(data).get('type')=='stats':ws.send('PING')
            assert sent>500 and received>500,'both synthetic directions must survive multiple watchdog intervals'
            result=json.loads(subprocess.check_output([str(R/'build/comm-native'),'call-end','4','e'*32],env={**watcher_env,'QDC_TEST_LIBRARY':'/nonexistent/qmi-library'},timeout=22))
            assert result['ok'];code,reason=ws.closed();assert code==1000 and '电话' in reason;ws.close()
            print('Persistent audio passed: 22 seconds duplex PCM with no helper CCI library; direct hangup stops audio.')
        finally:
            watcher.terminate();watcher.wait(timeout=6)
        print('Native audio passed: ticket/expiry/replay/origin, actual duplex synthetic PCM, single owner, logout, call end, protocol rejection and mixer cleanup.')
    finally:
        proc.terminate()
        try:proc.wait(timeout=8)
        except subprocess.TimeoutExpired:proc.kill();proc.wait();raise
        errors=proc.stderr.read().decode();assert 'forced stop' not in errors,errors
