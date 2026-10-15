"""Exercise actual session lifetime, invalid options, revocation and CSRF failures locally."""
import json, os, pathlib, subprocess, tempfile, time
R = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='qdc-remember-') as folder:
    root = pathlib.Path(folder)
    env = {**os.environ, 'QDC_WEB_ROOT': str(root), 'QDC_WEB_RUN': str(root/'run')}
    subprocess.run([str(R/'build/api-native'), '--init'], input=b'test-pass-123\n', env=env, check=True, stdout=subprocess.DEVNULL)
    def api(action, body=None, cookie=''):
        raw = json.dumps(body).encode() if body is not None else b''
        settings = {**env, 'REQUEST_METHOD': 'POST' if body is not None else 'GET', 'PATH_INFO': '/'+action,
                    'CONTENT_TYPE': 'application/json', 'CONTENT_LENGTH': str(len(raw)), 'HTTP_COOKIE': cookie}
        text = subprocess.check_output([str(R/'build/api-native')], input=raw, env=settings).decode().replace('\r\n', '\n')
        head, data = text.split('\n\n', 1)
        return head, json.loads(data)
    def login(remember=None):
        body = {'username': 'admin', 'password': 'test-pass-123'}
        if remember is not None: body['remember'] = remember
        head, result = api('login', body)
        cookie = next(x for x in head.splitlines() if x.startswith('Set-Cookie:')).split(': ',1)[1]
        sid = cookie.split(';',1)[0].split('=',1)[1]
        return head, result, cookie.split(';',1)[0], root/'run/sessions'/sid
    for option, seconds in [(None, 3600), ('0', 3600), ('1', 604800)]:
        head, session, cookie, file = login(option)
        assert f'Max-Age={seconds};' in head
        record = file.read_text().split()
        assert abs(int(record[1]) - time.clock_gettime(time.CLOCK_MONOTONIC) - seconds) < 3
        assert api('session', cookie=cookie)[1]['authenticated']
        assert '403' in api('logout', {'csrfToken': 'wrong'}, cookie)[0]
        assert api('session', cookie=cookie)[1]['authenticated'], 'CSRF error must not revoke the session'
        if option == '1':
            # Age the creation time by two hours without a multi-hour sleep.
            record[1] = str(int(record[1]) - 7200)
            file.write_text(' '.join(record)+'\n')
            assert api('session', cookie=cookie)[1]['authenticated'], 'remembered session survives the former one-hour limit'
        record[1] = str(int(time.clock_gettime(time.CLOCK_MONOTONIC)) - 1)
        file.write_text(' '.join(record)+'\n')
        assert '401' in api('session', cookie=cookie)[0]
        assert not file.exists(), 'expired session must be removed'
    assert '400' in api('login', {'username':'admin','password':'test-pass-123','remember':'999'})[0]
    _, session, cookie, file = login('1')
    assert api('logout', {'csrfToken':session['csrfToken']}, cookie)[1]['ok']
    assert '401' in api('session', cookie=cookie)[0]
    _, first, old_cookie, _ = login('1')
    _, second, new_cookie, _ = login('1')
    assert api('password', {'csrfToken':second['csrfToken'],'currentPassword':'test-pass-123',
                            'newPassword':'new-pass-123','confirmPassword':'new-pass-123'}, new_cookie)[1]['reauthenticate']
    assert '401' in api('session', cookie=old_cookie)[0]
    assert '401' in api('session', cookie=new_cookie)[0]
print('Remember login: one-hour default, seven-day option, >one-hour use, expiry, CSRF rejection, logout and password revocation passed.')
