"""Native front door checks with loopback mocks, never dial a modem."""
import gzip,hashlib,json,tempfile,shutil
import concurrent.futures, http.client, http.server, os, pathlib, socket, subprocess, threading, time

root=pathlib.Path(__file__).resolve().parent
requests=[]
class Web(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def do_GET(self):
        if self.path in ('/cgi-bin/api/events','/cgi-bin/api/status?idle=1'):
            self.send_response(200)
            self.send_header('Content-Type','text/event-stream' if self.path.endswith('events') else 'application/json')
            self.send_header('Connection','close');self.end_headers()
            self.wfile.write(b'retry: 10000\n\n');self.wfile.flush()
            time.sleep(1.5)  # Beyond the test build's 500 ms relay idle limit.
            try:self.wfile.write(b'event: state\ndata: {}\n\n');self.wfile.flush()
            except (BrokenPipeError,ConnectionResetError):pass
            return
        data=(self.path+' '+self.headers.get('Cookie','')).encode()
        requests.append((self.command,self.path,self.headers.get('Origin'),b''))
        self.send_response(200);self.send_header('Content-Type','application/octet-stream')
        self.send_header('Content-Length',str(len(data)));self.send_header('Set-Cookie','session=preserved; HttpOnly')
        self.send_header('Connection','close');self.end_headers();self.wfile.write(data)
    def do_POST(self):
        data=self.rfile.read(int(self.headers['Content-Length']));requests.append((self.command,self.path,self.headers.get('Origin'),data))
        self.send_response(200);self.send_header('Content-Length',str(len(data)));self.send_header('Connection','close');self.end_headers();self.wfile.write(data)
    def log_message(self,*args):pass

web=http.server.ThreadingHTTPServer(('127.0.0.1',0),Web)
threading.Thread(target=web.serve_forever,daemon=True).start()
audio=socket.socket();audio.bind(('127.0.0.1',0));audio.listen()
def audio_server():
    while True:
        try:c,_=audio.accept()
        except OSError:return
        def echo(c):
            with c:
                header=b''
                while not header.endswith(b'\r\n\r\n'):
                    x=c.recv(1)
                    if not x:return
                    header+=x
                assert header.startswith(b'GET /audio HTTP/1.1\r\n')
                assert b'Origin: https://example.test\r\n' in header
                c.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n')
                while True:
                    x=c.recv(4096)
                    if not x:return
                    c.sendall(x)
        threading.Thread(target=echo,args=(c,),daemon=True).start()
threading.Thread(target=audio_server,daemon=True).start()
probe=socket.socket();probe.bind(('127.0.0.1',0));port=probe.getsockname()[1];probe.close()
env={**os.environ,'ENTRY_PORT':str(port),'ENTRY_WEB_PORT':str(web.server_port),'ENTRY_AUDIO_PORT':str(audio.getsockname()[1]),'ENTRY_ASSET_ROOT':str(root/'dist/www')}
process=subprocess.Popen([str(root/'build/entry-native'),'https://example.test'],env=env,stderr=subprocess.DEVNULL)
try:
    for _ in range(50):
        try:
            with socket.create_connection(('127.0.0.1',port),timeout=.1):break
        except OSError:time.sleep(.02)
    def request(method,path,body=None,headers=None):
        c=http.client.HTTPConnection('127.0.0.1',port,timeout=4)
        try:
            c.request(method,path,body,headers or {});r=c.getresponse();return r.status,dict(r.getheaders()),r.read()
        finally:c.close()
    version=json.loads((root/'build/web-assets-size.json').read_text())['version']
    path='/module/app.js?v='+version
    raw=(root/'dist/www/app.js').read_bytes();packed=(root/'dist/www/app.js.gz').read_bytes()
    status,headers,data=request('GET',path,headers={'Cookie':'sid=test','Accept-Encoding':'gzip'})
    assert status==200 and headers['Content-Type']=='text/javascript; charset=utf-8'
    assert data==packed and gzip.decompress(data)==raw
    assert headers['Cache-Control']=='public, max-age=31536000, immutable' and headers['Vary']=='Accept-Encoding'
    assert headers['Content-Encoding']=='gzip' and headers['X-Content-Type-Options']=='nosniff' and 'Set-Cookie' not in headers
    etag=headers['ETag'];assert str(len(packed))==headers['Content-Length']
    count=len(requests)
    status,headers,data=request('GET',path,headers={'Accept-Encoding':'gzip','If-None-Match':etag})
    assert status==304 and data==b'' and headers['ETag']==etag
    assert len(requests)==count, 'Cached public files bypass the backend'
    assert request('GET',path,headers={'Accept-Encoding':'gzip','If-None-Match':'W/'+etag})[0]==304
    assert request('GET',path,headers={'Accept-Encoding':'gzip','If-None-Match':'"old", '+etag})[0]==304
    status,headers,data=request('GET',path,headers={'Accept-Encoding':'gzip;q=0, *;q=1','If-None-Match':etag})
    assert status==200 and data==raw and 'Content-Encoding' not in headers and headers['ETag']!=etag
    assert request('GET',path,headers={'Accept-Encoding':'br, gzip;q=0.5'})[2]==packed
    assert request('GET',path,headers={'Accept-Encoding':'br'})[2]==raw
    status,headers,data=request('HEAD',path,headers={'Accept-Encoding':'gzip'})
    assert status==200 and data==b'' and int(headers['Content-Length'])==len(packed)
    assert request('GET','/module/app.js?v=old',headers={'If-None-Match':etag,'Accept-Encoding':'gzip'})[1]['Cache-Control']=='no-cache'
    assert request('GET','/module/app.js')[1]['Cache-Control']=='no-cache'
    for page in ['/module/','/module/index.html']:
        status,headers,data=request('GET',page,headers={'Accept-Encoding':'gzip','If-None-Match':'*'})
        assert status==200 and headers['Cache-Control']=='no-store' and gzip.decompress(data)==(root/'dist/www/index.html').read_bytes()
    assert request('POST','/module/cgi-bin/api/sim-recover',body='{}',headers={'Origin':'https://example.test','Content-Type':'application/json'})[0]==200
    assert request('GET','/module/cgi-bin/api/sim-recover')[0]==404
    assert request('POST','/module/cgi-bin/api/sim-recover',body='{}',headers={'Origin':'https://other.test','Content-Type':'application/json'})[0]==403
    assert request('GET','/module/cgi-bin/api/sim-pin-state')[0]==200
    assert request('POST','/module/cgi-bin/api/sim-pin-verify',body='{}',headers={'Origin':'https://example.test','Content-Type':'application/json'})[0]==200
    assert request('POST','/module/cgi-bin/api/sim-pin-verify',body='{}',headers={'Origin':'https://other.test','Content-Type':'application/json'})[0]==403
    assert request('GET','/module/cgi-bin/api/sim-pin-verify')[0]==404
    # No encoding/cache policy can bypass the fixed path allowlist.
    assert request('GET','/module/private/credential',headers={'Accept-Encoding':'gzip'})[0]==404
    # Missing gzip falls back to the valid identity representation.
    with tempfile.TemporaryDirectory() as tmp:
        temp=pathlib.Path(tmp);shutil.copy(root/'dist/www/app.js',temp/'app.js')
        probe=socket.socket();probe.bind(('127.0.0.1',0));fallback_port=probe.getsockname()[1];probe.close()
        fallback=subprocess.Popen([str(root/'build/entry-native'),'https://example.test'],env={**env,'ENTRY_PORT':str(fallback_port),'ENTRY_ASSET_ROOT':tmp},stderr=subprocess.DEVNULL)
        try:
            for _ in range(50):
                try:
                    c=http.client.HTTPConnection('127.0.0.1',fallback_port,timeout=2);c.request('GET',path,headers={'Accept-Encoding':'gzip'});r=c.getresponse();assert r.status==200 and r.read()==raw and r.getheader('Content-Encoding') is None;c.close();break
                except ConnectionRefusedError:time.sleep(.02)
            else:raise AssertionError('fallback entry unavailable')
            fixture=temp/'private-fixture';fixture.write_bytes(raw)
            (temp/'app.js').unlink();(temp/'app.js').symlink_to(fixture)
            c=http.client.HTTPConnection('127.0.0.1',fallback_port,timeout=2);c.request('GET',path);r=c.getresponse();assert r.status==503 and r.read()==b'';c.close()
        finally:fallback.terminate();fallback.wait(timeout=3)
    body=b'{"number":"test-only"}'
    assert request('POST','/module/cgi-bin/api/call-dial',body,{'Origin':'https://example.test','Content-Type':'application/json'})[2]==body
    assert requests[-1]==('POST','/cgi-bin/api/call-dial','https://example.test',body)
    assert request('GET','/module/cgi-bin/api/traffic')[2]==b'/cgi-bin/api/traffic '
    assert request('POST','/module/cgi-bin/api/traffic-reset',b'{}',{'Origin':'https://example.test'})[0]==200
    assert request('POST','/module/cgi-bin/api/traffic-reset',b'{}',{'Origin':'https://attacker.test'})[0]==403
    assert request('GET','/module/cgi-bin/api/data')[2]==b'/cgi-bin/api/data '
    assert request('GET','/module/cgi-bin/api/data',headers={'If-None-Match':'*'})[1]['Cache-Control']=='no-store'
    assert request('GET','/module/cgi-bin/api/tunnel',headers={'Cookie':'sid=tunnel'})[2]==b'/cgi-bin/api/tunnel sid=tunnel'
    assert request('GET','/module/cgi-bin/api/oray-status',headers={'Cookie':'sid=binding'})[2]==b'/cgi-bin/api/oray-status sid=binding'
    assert request('POST','/module/cgi-bin/api/oray-qr',b'{}',{'Origin':'https://example.test'})[0]==200
    assert request('POST','/module/cgi-bin/api/oray-qr',b'{}',{'Origin':'https://attacker.test'})[0]==403
    assert request('GET','/module/cgi-bin/api/oray-qr')[0]==404
    assert request('GET','/module/cgi-bin/api/oray-heartbeat')[0]==200
    assert request('POST','/module/cgi-bin/api/oray-heartbeat-set',b'{}',{'Origin':'https://example.test'})[0]==200
    assert request('POST','/module/cgi-bin/api/oray-heartbeat-set',b'{}',{'Origin':'https://attacker.test'})[0]==403
    assert request('POST','/module/cgi-bin/api/oray-heartbeat-set',b'{}')[0]==403
    assert request('GET','/module/cgi-bin/api/oray-heartbeat-set')[0]==404
    assert request('GET','/module/cgi-bin/api/esim-job')[0]==200
    assert request('POST','/module/cgi-bin/api/esim-job',b'{}',{'Origin':'https://example.test'})[0]==404
    for endpoint in ['esim-list','esim-operation','sms-outbox-list','sms-outbox-delete','call-history-list','call-history-clear','call-history-import']:
        assert request('POST','/module/cgi-bin/api/'+endpoint,b'{}',{'Origin':'https://example.test'})[0]==200
        assert request('POST','/module/cgi-bin/api/'+endpoint,b'{}',{'Origin':'https://attacker.test'})[0]==403
        assert request('GET','/module/cgi-bin/api/'+endpoint)[0]==404
    assert request('POST','/module/cgi-bin/api/tunnel-set',b'{"enabled":"0"}',{'Origin':'https://example.test'})[2]==b'{"enabled":"0"}'
    assert requests[-1]==('POST','/cgi-bin/api/tunnel-set','https://example.test',b'{"enabled":"0"}')
    assert request('POST','/module/cgi-bin/api/tunnel-set',b'{}',{'Origin':'https://attacker.test'})[0]==403
    assert request('POST','/module/cgi-bin/api/tunnel-set',b'{}')[0]==403
    assert request('GET','/module/cgi-bin/api/tunnel-set')[0]==404
    assert request('POST','/module/cgi-bin/api/tunnel',b'{}',{'Origin':'https://example.test'})[0]==404
    assert request('POST','/module/cgi-bin/api/data-set',b'{"enabled":"0"}',{'Origin':'https://example.test'})[2]==b'{"enabled":"0"}'
    assert request('POST','/module/cgi-bin/api/data-set',b'{}',{'Origin':'https://attacker.test'})[0]==403
    assert request('GET','/module/cgi-bin/api/data-set')[0]==404
    assert request('POST','/module/cgi-bin/api/data',b'{}',{'Origin':'https://example.test'})[0]==404
    count=len(requests)
    for path in ['/module/../private/credential','/module/cgi-bin/qdc-comm','/module/%2e%2e/private/credential','/other','/module/bin/qdc-audio']:
        assert request('GET',path)[0] in (400,404)
    assert request('POST','/module/cgi-bin/api/password',b'{}',{'Origin':'https://attacker.test'})[0]==403
    assert request('POST','/module/cgi-bin/api/password',b'{}')[0]==403
    assert request('POST','/module/cgi-bin/api/password',b'x'*8193,{'Origin':'https://example.test'})[0]==413
    assert request('GET','/module/audio',headers={'Origin':'https://example.test'})[0]==400
    assert len(requests)==count
    with socket.create_connection(('127.0.0.1',port),timeout=4) as c:
        c.sendall(b'POST /module/cgi-bin/api/login HTTP/1.1\r\nHost: local\r\nOrigin: https://example.test\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}')
        assert c.recv(100).startswith(b'HTTP/1.1 400')
    assert request('GET','/')[1]['Location']=='/module/'
    assert request('GET','/module/cgi-bin/api/events')[2]==b'retry: 10000\n\nevent: state\ndata: {}\n\n'
    assert request('GET','/module/cgi-bin/api/status?idle=1')[2]==b'retry: 10000\n\n', 'Normal requests retain idle deadline'
    # Exercise bidirectional streaming beyond both relay buffers, with binary NULs.
    with socket.create_connection(('127.0.0.1',port),timeout=5) as c:
        c.sendall(b'GET /module/audio HTTP/1.1\r\nHost: local\r\nOrigin: https://example.test\r\nConnection: keep-alive, Upgrade\r\nUpgrade: websocket\r\n\r\n')
        header=b''
        while not header.endswith(b'\r\n\r\n'):header+=c.recv(1)
        assert header.startswith(b'HTTP/1.1 101')
        data=bytes(range(256))*700
        with concurrent.futures.ThreadPoolExecutor() as pool:
            sent=pool.submit(c.sendall,data);out=b''
            while len(out)<len(data):
                x=c.recv(3072);assert x;out+=x
            sent.result();assert out==data
    print('Entry passed: fixed routes, Origin, body size/duplicate length, cookies, MIME/cache and bidirectional binary WebSocket relay.')
finally:
    process.terminate();process.wait(timeout=3);web.shutdown();audio.close()
