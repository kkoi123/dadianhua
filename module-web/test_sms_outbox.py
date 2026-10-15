"""Persisted sent history using mocked QMI only; never contacts a real modem."""
import json, os, pathlib, subprocess, tempfile, shutil, time
R=pathlib.Path(__file__).resolve().parent
subprocess.run(['clang','-shared','-fPIC','-Wall','-Wextra','-Werror',str(R/'test_mock_qmi.c'),'-o',str(R/'build/mock-qmi.dylib')],check=True)
PDU='0001000d91683108108300f00008044f60597d';SIM='89860000000000000000'
with tempfile.TemporaryDirectory(prefix='qdc-outbox-') as folder:
    base=pathlib.Path(folder)
    for name in ['private','run','www/cgi-bin']:(base/name).mkdir(parents=True,mode=0o700)
    shutil.copy(R/'build/comm-native',base/'www/cgi-bin/qdc-comm')
    env={**os.environ,'QDC_WEB_ROOT':folder,'QDC_WEB_RUN':str(base/'run'),'QDC_TEST_LIBRARY':str(R/'build/mock-qmi.dylib'),'QDC_TEST_LOG':str(base/'operations')}
    binary=str(R/'build/api-native')
    subprocess.run([binary,'--init'],input=b'outbox-test-password\n',stdout=subprocess.DEVNULL,env=env,check=True)
    def api(name,body=None,cookie='',method='POST',scenario='normal'):
        raw=json.dumps(body or {},ensure_ascii=False).encode()
        e={**env,'QDC_TEST_SCENARIO':scenario,'REQUEST_METHOD':method,'PATH_INFO':'/'+name,'CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(raw)),'HTTP_COOKIE':cookie}
        out=subprocess.check_output([binary],input=raw,env=e).decode().replace('\r\n','\n');head,data=out.split('\n\n',1)
        return head,json.loads(data)
    for endpoint in ['sms-outbox-list','sms-outbox-delete']:
        assert '401' in api(endpoint)[0]
    head,login=api('login',{'username':'admin','password':'outbox-test-password'})
    cookie=next(l for l in head.splitlines() if l.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0];csrf=login['csrfToken']
    def call(name,body=None,**kw):return api(name,{'csrfToken':csrf,**(body or {})},cookie,**kw)
    for endpoint in ['sms-outbox-list','sms-outbox-delete']:
        assert '405' in api(endpoint,cookie=cookie,method='GET')[0]
        assert '403' in api(endpoint,{},cookie)[0]
    assert call('sms-outbox-list')[1]['messages']==[]
    sid='a'*32
    body={'requestId':sid,'sim':SIM,'pdus':PDU}
    assert call('sms-send',body)[1]['ok']
    record=base/'private'/('action-sms-'+sid);before=record.read_bytes()
    stamp=base/'private'/('sms-start-'+sid);before_stamp=stamp.read_bytes()
    head,data=call('sms-outbox-list',{'sim':SIM});row=data['messages'][0]
    assert row['id']==sid and row['status']=='submitted' and row['sentParts']==1 and row['currentSIM'] and row['timeExact']
    assert SIM not in json.dumps(data) and 'csrfToken' not in json.dumps(data)
    assert call('sms-outbox-list',{'sim':'ef:'+'0'*20})[1]['messages'][0]['currentSIM'] is False
    assert call('sms-outbox-delete',{'id':sid})[1]['deleted']
    assert call('sms-outbox-list')[1]['messages']==[]
    assert record.read_bytes()==before
    assert call('sms-outbox-delete',{'id':sid})[1]['deleted']
    # CGI restarts on every request. Hidden record still prevents modem resends.
    assert call('sms-send',body)[1]['ok'] and stamp.read_bytes()==before_stamp
    assert (base/'operations').read_text().splitlines().count('sms:20')==1
    assert call('sms-outbox-list')[1]['messages']==[]
    assert '400' in call('sms-outbox-delete',{'id':'../credentials'})[0]
    assert '404' in call('sms-outbox-delete',{'id':'0'*32})[0]
    assert '400' in call('sms-outbox-list',{'cursor':'513'})[0]
    assert call('sms-send',{'requestId':'b'*32,'sim':SIM,'pdus':PDU},scenario='timeout')[1]['uncertain']
    assert call('sms-outbox-list')[1]['messages'][0]['status']=='uncertain'
    fixtures=[('c',{'ok':False,'sentParts':1,'totalParts':2,'error':'第二段未提交'},PDU+','+PDU,'partial'),
              ('d',{'ok':False,'error':'SIM 卡已变化'},PDU,'failed'),
              ('e',{'ok':'invalid'},PDU,'uncertain'),
              ('f',{'ok':True,'sentParts':3,'totalParts':1},PDU,'uncertain')]
    for i,(digit,result,pdus,state) in enumerate(fixtures):
        p=base/'private'/('action-sms-'+digit*32);p.write_text('ef:'+'1'*20+':'+pdus+'\n'+json.dumps(result,ensure_ascii=False));p.chmod(0o600);os.utime(p,(1700000000+i,1700000000+i))
    rows={x['id'][0]:x for x in call('sms-outbox-list')[1]['messages']}
    for digit,_,_,state in fixtures:assert rows[digit]['status']==state and not rows[digit]['timeExact']
    assert rows['c']['sentParts']==1 and rows['c']['totalParts']==2 and rows['c']['error']=='第二段未提交'
    bad=base/'private'/('action-sms-'+'1'*32);bad.write_text(SIM+':'+PDU+'\n{"ok":true}trailing');bad.chmod(0o600)
    target=base/'private/sentinel';target.write_text('must-not-leak');target.chmod(0o600)
    link=base/'private'/('action-sms-'+'2'*32);link.symlink_to(target)
    hidden=base/'private'/('sms-hidden-'+'d'*32);hidden.write_text('1\n');hidden.chmod(0o644)
    result=call('sms-outbox-list')[1]
    assert result['readErrors']>=3 and 'must-not-leak' not in json.dumps(result) and 'd'*32 not in [x['id'] for x in result['messages']]
    assert '404' in call('sms-outbox-delete',{'id':'2'*32})[0]
    for n in range(10):
        p=base/'private'/('action-sms-'+format(100+n,'032x'));p.write_text(SIM+':'+PDU+'\n{"ok":true}');p.chmod(0o600);os.utime(p,(1800000000+n,1800000000+n))
    result=call('sms-outbox-list')[1];assert len(result['messages'])==8 and result['next']==8
    assert [x['timestamp'] for x in result['messages']]==sorted([x['timestamp'] for x in result['messages']],reverse=True)
    rest=call('sms-outbox-list',{'cursor':'8'})[1]
    assert not set(x['id'] for x in result['messages']) & set(x['id'] for x in rest['messages'])
print('Sent SMS backend passed: auth/CSRF/method gates, durable timestamps, old records, status normalization, SIM privacy, paging, malformed files, symlinks, persistent delete and idempotent replay without a second send.')
