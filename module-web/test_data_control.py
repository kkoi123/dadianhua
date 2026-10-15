"""Authenticated dual-stack data gate. Mock routes/manager, no real modem."""
import json,os,pathlib,shutil,subprocess,tempfile,time,sys
ROOT=pathlib.Path(__file__).resolve().parent
hook=ROOT/'build/mock-sysctl-write.dylib'
probe=ROOT/'build/sysctl-write-probe'
subprocess.run(['clang','-Wall','-Wextra','-Werror',str(ROOT/'test_sysctl_write_probe.c'),'-o',str(probe)],check=True)
subprocess.run(['clang','-shared','-fPIC','-Wall','-Wextra','-Werror',str(ROOT/'test_sysctl_short_write.c'),'-o',str(hook)],check=True)
with tempfile.TemporaryDirectory(prefix='qdc-data-') as tmp:
    base=pathlib.Path(tmp);(base/'private').mkdir(mode=0o700);(base/'run').mkdir(mode=0o700);(base/'www/cgi-bin').mkdir(parents=True)
    shutil.copy(ROOT/'build/api-native',base/'www/cgi-bin/api')
    routes={'4':['default via 10.1.0.1 dev rmnet_data0','10.1.0.0/24 dev rmnet_data0 src 10.1.0.2','192.168.225.0/24 dev bridge0'], '6':['default dev rmnet_data0 metric 256','2001:db8::/64 dev rmnet_data0 metric 256','fe80::/64 dev bridge0 metric 256','2001:db8:1::/64 dev rmnet_data1 metric 256']}
    (base/'link').write_text('1\n');(base/'addresses').write_text(json.dumps({'4':['10.1.0.2/24'],'6':['2001:db8::2/64','2001:db8::3/64','fe80::1/64']}));(base/'routes').write_text(json.dumps(routes));(base/'calls').write_text('[]');(base/'sysctl').mkdir();(base/'sysctl/ipv4').write_text('1\n');(base/'sysctl/ipv6').write_text('2\n')
    helper=base/'www/cgi-bin/qdc-comm';helper.write_text('''#!/usr/bin/env python3
import os,pathlib,sys
root=pathlib.Path(os.environ['QDC_WEB_ROOT'])
if sys.argv[1] in ['roaming-state','roaming-config']:
 assert sys.argv[2]=='1'
 flag=root/'native-roaming';permission=flag.read_text().strip() if flag.exists() else '1'
 if sys.argv[1]=='roaming-config':
  assert sys.argv[3] in ['0','1']
  if (root/'native-roaming-fail').exists():print('{"ok":false}');sys.exit(0)
  permission=sys.argv[3];flag.write_text(permission)
 p=root/'roaming';value=int(p.read_text()) if p.exists() else 0
 print('{"ok":true,"roaming":'+('true' if value==1 else 'false' if value==0 else 'null')+',"registrationState":1,"permission":'+('true' if permission=='1' else 'false')+',"profile":1}')
else:print('{"ok":true,"calls":'+(root/'calls').read_text()+'}')
''');helper.chmod(0o700)
    ip=base/'ip';ip.write_text('''#!/usr/bin/env python3
import json,os,pathlib,sys
r=pathlib.Path(os.environ['QDC_WEB_ROOT']);a=sys.argv[1:];assert a[0] in ['-4','-6'];family=a[0][1:];p=r/'routes';state=json.loads(p.read_text())
addresses=r/'addresses';saved=json.loads(addresses.read_text())
if (r/'link').read_text().strip()=='0':saved['6']=[];addresses.write_text(json.dumps(saved))
if a[1]=='addr':
 if a[2]=='show':
  assert a[3:]==['dev','rmnet_data0']
  if family=='6' and (r/'stock-ipv6').exists() and (r/'link').read_text().strip()=='1':saved['6']=['2001:db8::99/64'];addresses.write_text(json.dumps(saved))
  print('\\n'.join(('inet' if family=='4' else 'inet6')+' '+x+' scope global' for x in saved[family]))
 elif a[2]=='add':
  assert a[4:]==['dev','rmnet_data0']
  saved[family].append(a[3]);addresses.write_text(json.dumps(saved))
 else:raise AssertionError(a)
 sys.exit(0)
assert a[1]=='route'
if (r/'bad-info').exists():sys.exit(1)
if a[2]=='show':
 assert len(a)==3
 print('\\n'.join(state[family]))
elif a[2]=='flush':
 assert a[3:]==['dev','rmnet_data0']
 with (r/'commands').open('a') as f:f.write('off'+family+'\\n')
 if (r/('failure'+family)).exists():sys.exit(1)
 state[family]=[line for line in state[family] if 'dev rmnet_data0' not in line];p.write_text(json.dumps(state))
elif a[2]=='replace':
 assert 'dev' in a and a[a.index('dev')+1]=='rmnet_data0'
 line=' '.join(a[3:])
 if not saved[family]:sys.exit(2) # Cannot restore a subnet without an address
 # Model the live kernel: a gateway default requires its connected subnet.
 if family=='4' and line.startswith('default via ') and not any(x.startswith('10.1.0.0/24 dev rmnet_data0') for x in state[family]):sys.exit(2)
 state[family]=[item for item in state[family] if item!=line]+[line];p.write_text(json.dumps(state))
 with (r/'commands').open('a') as f:f.write('on'+family+'\\n')
else:raise AssertionError(a)
''');ip.chmod(0o700)
    manager=base/'manager';manager.write_text('''#!/usr/bin/env python3
import sys,os,pathlib,json
r=pathlib.Path(os.environ['QDC_WEB_ROOT'])
if sys.argv[1:]==['data_call','default_profile']:print('get default profile:\\n profile index: 1\\n ip family: v4')
else:
 assert sys.argv[1:]==['data_call','up','1','v4','0']
 p=r/'routes';state=json.loads(p.read_text());state['4'].append('default via 10.1.0.1 dev rmnet_data0');p.write_text(json.dumps(state))
 with (r/'commands').open('a') as f:f.write('manager-up-no-reconnect\\n')
''');manager.chmod(0o700)
    env={**os.environ,'QDC_WEB_ROOT':tmp,'QDC_WEB_RUN':str(base/'run'),'QDC_DATA_TEST_CLI':str(manager),'QDC_DATA_TEST_IP':str(ip),'QDC_DATA_TEST_SYSCTL':str(base/'sysctl'),'QDC_DATA_TEST_LINK':str(base/'link')}
    env['DYLD_INSERT_LIBRARIES' if sys.platform=='darwin' else 'LD_PRELOAD']=str(hook)
    subprocess.run([str(probe),str(base/'sysctl/ipv6')],env=env,check=True)
    subprocess.run([str(ROOT/'build/api-native'),'--init'],input=b'data-test-password\n',env=env,stdout=subprocess.DEVNULL,check=True)
    def api(name,body=None,cookie='',method='GET'):
        raw=json.dumps(body or {}).encode();e={**env,'REQUEST_METHOD':method,'PATH_INFO':'/'+name,'CONTENT_TYPE':'application/json','CONTENT_LENGTH':str(len(raw)),'HTTP_COOKIE':cookie}
        out=subprocess.check_output([str(ROOT/'build/api-native')],input=raw,env=e).decode().replace('\r\n','\n');head,payload=out.split('\n\n',1);return head,json.loads(payload)
    def commands():return (base/'commands').read_text().splitlines() if (base/'commands').exists() else []
    def complete():
        for _ in range(160):
            if not (base/'run/data-job').exists():return
            time.sleep(.05)
        raise AssertionError('worker did not complete')
    def reconcile():subprocess.run([str(ROOT/'build/api-native'),'--data-reconcile'],env=env,check=True)
    assert '401' in api('data')[0] and '401' in api('data-set',{'enabled':'0'},method='POST')[0]
    head,login=api('login',{'username':'admin','password':'data-test-password'},method='POST');cookie=next(l for l in head.splitlines() if l.startswith('Set-Cookie:')).split(': ',1)[1].split(';',1)[0];csrf=login['csrfToken']
    def set_data(value):return api('data-set',{'csrfToken':csrf,'enabled':str(value)},cookie,'POST')
    assert '405' in api('data-set',cookie=cookie)[0]
    assert '403' in api('data-set',{'enabled':'0'},cookie,'POST')[0]
    assert '400' in api('data-set',{'csrfToken':csrf,'enabled':'0;reboot'},cookie,'POST')[0]
    _,state=api('data',cookie=cookie);assert state['enabled'] and state['connected'] and commands()==[]
    (base/'calls').write_text('[{"id":4,"state":3}]');assert '409' in set_data(0)[0] and commands()==[];(base/'calls').write_text('[]')
    head,state=set_data(0);assert '202' in head and state['accepted'] and state['pending'] and not state['enabled']
    assert commands()==[],'Response precedes delayed disconnect';assert '409' in set_data(1)[0]
    reconcile();assert commands()==[],'Supervisor skips pending switch'
    complete();_,state=api('data',cookie=cookie);assert not state['enabled'] and not state['connected'] and state['lastSuccess'];assert commands()==['off4','off6']
    assert (base/'link').read_text()=='0\n'
    assert json.loads((base/'addresses').read_text())['6']==[]
    assert (base/'run/data-ipv6.addresses').read_text()=='2001:db8::2/64\n2001:db8::3/64\nfe80::1/64\n'
    remaining=json.loads((base/'routes').read_text());assert remaining['4']==routes['4'][2:] and remaining['6']==routes['6'][2:],'USB LAN and IMS routes survive'
    assert (base/'private/data-policy').stat().st_mode&0o777==0o600
    for f in [4,6]:assert (base/f'run/data-ipv{f}.routes').stat().st_mode&0o777==0o600
    reconcile();assert commands()==['off4','off6'],'Disabled policy never reconnects'
    # Restore a vendor IPv6 route: a stale successful result must not hide it.
    (base/'link').write_text('1\n');remaining['6'].insert(0,routes['6'][0]);(base/'routes').write_text(json.dumps(remaining));_,state=api('data',cookie=cookie);assert state['connected'] and state['lastSuccess'] is False
    reconcile();assert commands()==['off4','off6','off6']
    set_data(1);complete();_,state=api('data',cookie=cookie);assert state['connected'] and state['enabled'] and state['lastSuccess'];assert 'on4' in commands() and 'on6' in commands()
    assert json.loads((base/'addresses').read_text())['6']==['2001:db8::2/64'], 'Do not accumulate privacy/link-local addresses'
    # A call may start during the acceptance delay; preserve its network.
    set_data(0);(base/'calls').write_text('[{"id":4}]');complete();_,state=api('data',cookie=cookie)
    assert state['enabled'] and state['connected'] and state['lastSuccess'] is False
    (base/'calls').write_text('[]')
    # Prefer an address freshly assigned by stock netmgr to old privacy addresses.
    set_data(0);complete();(base/'stock-ipv6').touch();set_data(1);complete()
    assert json.loads((base/'addresses').read_text())['6']==['2001:db8::99/64']
    (base/'stock-ipv6').unlink()
    # A snapshot taken during network changes can lack the IPv4 default.
    set_data(0);complete();(base/'run/data-ipv4.routes').write_text('10.1.0.0/24 dev rmnet_data0 src 10.1.0.2\n');set_data(1);complete()
    assert 'manager-up-no-reconnect' in commands(),'Restore the configured IPv4 family even if only IPv6 was saved'
    (base/'failure6').touch();set_data(0);complete();_,state=api('data',cookie=cookie);assert not state['connected'] and not state['enabled'] and state['lastSuccess'] is False,'A cleanup failure must be reported despite the link gate'
    (base/'failure6').unlink();reconcile();_,state=api('data',cookie=cookie);assert not state['connected']
    # Never interpret a snapshot as shell commands, nor accept another device.
    (base/'run/data-ipv4.routes').write_text('default dev bridge0\n');set_data(1);complete();_,state=api('data',cookie=cookie);assert not state['connected'] and state['lastSuccess'] is False
    # Independent USB gate retains the module routes and does not touch the radio.
    (base/'run/data-ipv4.routes').write_text('\n'.join(routes['4'][:2])+'\n');set_data(1);complete()
    def set_usb(value):return api('data-set',{'csrfToken':csrf,'enabled':str(value),'scope':'usb'},cookie,'POST')
    assert '400' in api('data-set',{'csrfToken':csrf,'enabled':'0','scope':'usb;reboot'},cookie,'POST')[0]
    before_routes=(base/'routes').read_text();before_commands=commands()
    (base/'calls').write_text('[{"id":4,"state":3}]')
    assert '202' in set_usb(0)[0],'USB-only gate does not interrupt module/local calls'
    complete();_,state=api('data',cookie=cookie)
    assert state['enabled'] and state['connected'] and not state['usbEnabled'] and not state['usbForwarding'] and not state['usbConnected'] and state['usbLastSuccess']
    assert (base/'routes').read_text()==before_routes and commands()==before_commands
    assert (base/'private/data-policy').read_text()=='1\n' and (base/'private/usb-policy').read_text()=='0\n'
    assert (base/'sysctl/ipv4').read_text()=='0\n' and (base/'sysctl/ipv6').read_text()=='0\n'
    assert (base/'private/usb-forwarding-baseline').read_text()=='1 2\n'
    reconcile();assert not api('data',cookie=cookie)[1]['usbForwarding']
    set_usb(1);complete();assert (base/'sysctl/ipv4').read_text()=='1\n' and (base/'sysctl/ipv6').read_text()=='2\n'
    assert api('data',cookie=cookie)[1]['usbConnected']
    (base/'calls').write_text('[]');set_data(0);complete();_,state=api('data',cookie=cookie)
    assert state['usbEnabled'] and state['usbForwarding'] and not state['usbConnected'],'Upstream off cannot pretend USB has internet'
    set_usb(0);complete();_,state=api('data',cookie=cookie);assert not state['connected'] and not state['usbForwarding']
    assert (base/'private/usb-policy').stat().st_mode&0o777==0o600
    assert (base/'private/usb-forwarding-baseline').stat().st_mode&0o777==0o600
    # Never report both protocol families blocked if one has been re-enabled.
    (base/'sysctl/ipv6').write_text('2\n');_,state=api('data',cookie=cookie);assert state['usbForwarding'] is None
    reconcile();assert (base/'sysctl/ipv6').read_text()=='0\n'
    # Data roaming is independently scoped. It uses NAS, gates both protocol
    # families, and survives a new runtime after power loss without NV writes.
    set_data(1);complete()
    policy=base/'private/roaming-policy'
    assert not policy.exists() and api('data',cookie=cookie)[1]['roamingEnabled'], 'Install preserves existing connectivity'
    def set_roaming(value):return api('data-set',{'csrfToken':csrf,'enabled':str(value),'scope':'roaming'},cookie,'POST')
    assert '401' in api('data-set',{'scope':'roaming','enabled':'0'},method='POST')[0]
    assert '403' in api('data-set',{'scope':'roaming','enabled':'0'},cookie,'POST')[0]
    assert '400' in set_roaming('2')[0]
    (base/'roaming').write_text('1');_,s=api('data',cookie=cookie)
    assert s['roaming'] and s['connected'] and s['effectiveEnabled']
    before=commands();assert '202' in set_roaming(0)[0];complete();_,s=api('data',cookie=cookie)
    assert s['enabled'] and not s['connected'] and s['roamingBlocked'] and not s['effectiveEnabled'] and not s['roamingEnabled']
    assert s['usbEnabled'] is False, 'Roaming switch leaves USB preference alone'
    assert policy.read_text()=='0\n' and policy.stat().st_mode&0o777==0o600
    assert (base/'native-roaming').read_text()=='0', 'Also update the native WDS roaming permission'
    current=json.loads((base/'routes').read_text());assert current['4']==routes['4'][2:] and current['6']==routes['6'][2:], 'USB LAN/IMS kept'
    paused=commands();reconcile();assert commands()==paused, 'Disabled roaming cannot reconnect while roaming'
    (base/'roaming').write_text('0');reconcile();_,s=api('data',cookie=cookie)
    assert s['connected'] and not s['roamingBlocked'] and not s['roamingEnabled'], 'Home network resumes automatically'
    (base/'roaming').write_text('-1');reconcile();_,s=api('data',cookie=cookie)
    assert s['roaming'] is None and not s['connected'] and s['roamingBlocked'], 'Unknown state cannot authorize roaming'
    (base/'roaming').write_text('1');assert '202' in set_roaming(1)[0];complete()
    assert api('data',cookie=cookie)[1]['connected']
    assert (base/'native-roaming').read_text()=='1'
    (base/'native-roaming-fail').touch();set_roaming(0);complete();_,s=api('data',cookie=cookie)
    assert not s['connected'] and s['lastSuccess'] is False and s['roamingApplied'] is True, 'Native write failure stays visible while Linux gate still blocks'
    (base/'native-roaming-fail').unlink();reconcile();assert (base/'native-roaming').read_text()=='0'
    set_roaming(1);complete()
    (base/'calls').write_text('[{"id":4}]');assert '409' in set_roaming(0)[0] and policy.read_text()=='1\n'
    (base/'calls').write_text('[]');set_roaming(0);(base/'calls').write_text('[{"id":4}]');complete()
    assert policy.read_text()=='1\n', 'Call starting during acceptance delay restores roaming permission'
    (base/'calls').write_text('[]');set_roaming(0);complete()
    # Simulate power loss: policy remains, runtime snapshots disappear and a
    # vendor boot has brought the link/routes up. Reconcile must still block.
    (base/'link').write_text('1\n');(base/'routes').write_text(json.dumps(routes))
    for f in (base/'run').glob('data-*'):f.unlink()
    reconcile();assert (base/'link').read_text()=='0\n' and not api('data',cookie=cookie)[1]['connected']
    policy.write_text('broken\n');assert api('data',cookie=cookie)[1]['roamingEnabled'] is None
    assert subprocess.run([str(ROOT/'build/api-native'),'--data-reconcile'],env=env).returncode==1
    assert (base/'link').read_text()=='0\n', 'Malformed policy cannot open the gate'
    policy.write_text('1\n');set_data(0);complete()
    # Proc destinations and saved values are bounded; no symlink writes.
    (base/'sysctl/ipv6').unlink();(base/'sysctl/ipv6').symlink_to(base/'private/data-policy')
    assert subprocess.run([str(ROOT/'build/api-native'),'--data-reconcile'],env=env).returncode==1
    assert (base/'private/data-policy').read_text()=='0\n'
    (base/'sysctl/ipv6').unlink();(base/'sysctl/ipv6').write_text('0\n')
    (base/'bad-info').touch();_,state=api('data',cookie=cookie);assert state['connected'] is None
    assert subprocess.run([str(ROOT/'build/api-native'),'--data-reconcile'],env=env).returncode==1
print('Data switch passed: dual-stack link/address/route restore, auth/CSRF, call guard, delayed response, serialized changes, saved off policy, vendor reconnect correction, restore, IPv6 failure, validated snapshot, USB/IMS preservation.')
