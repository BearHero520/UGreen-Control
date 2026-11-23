#!/usr/bin/env python3
"""Simulate DSM account/unit deployment inside a disposable Debian container.

This does not emulate Package Center or approve DSM system-unit policy/ABI.
"""
import ast
import base64
from contextlib import closing
import hashlib
import http.client
import io
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import ssl
import subprocess
import sys
import tarfile
import time
import zipfile
from urllib.parse import urlsplit

UID=20001
GID=20001
PASSWORD="native-spk-disposable-password"
RESULT={"environment":"disposable Debian container; DSM Package Center not emulated", "checks":{}}
LED_AUTH='--authorize-led' in sys.argv or '--upgrade-preserve' in sys.argv
BIOS_AUTH='--authorize-bios' in sys.argv or '--upgrade-preserve' in sys.argv


def passed(name):
    RESULT["checks"][name]=True
    print("PASS "+name,flush=True)


def unpack(data,destination,owner=None):
    destination.mkdir(parents=True,exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data),mode='r:*') as archive:
        for member in archive:
            path=Path(member.name)
            assert member.isfile() and not path.is_absolute() and '..' not in path.parts
            target=destination/path
            target.parent.mkdir(parents=True,exist_ok=True)
            target.write_bytes(archive.extractfile(member).read())
            target.chmod(member.mode)
    if owner:
        for directory,_,files in os.walk(destination):
            os.chown(directory,*owner)
            for name in files: os.chown(Path(directory)/name,*owner)


def request(path='/api/about/info',authenticated=True):
    headers={"Authorization":"Basic "+base64.b64encode(('admin:'+PASSWORD).encode()).decode()} if authenticated else {}
    with closing(http.client.HTTPConnection('127.0.0.1',5088,timeout=4)) as c:
        c.request('GET',path,headers=headers)
        response=c.getresponse()
        return response.status,response.read()


def ready():
    end=time.monotonic()+50
    while time.monotonic()<end:
        try:
            if request()[0]==200: return
        except (OSError,http.client.HTTPException): pass
        time.sleep(.3)
    raise RuntimeError('native Web service did not become ready')


def proxy_request(path='', method='GET', *, cookie='', login=False, origin=None, extra=None, plain_http=False):
    parsed=urlsplit(os.environ['UGREEN_TEST_DESKTOP_PROXY'])
    authority=parsed.netloc
    headers={'X-UGREEN-Request':'1','Origin':origin if origin is not None else 'https://'+authority}
    if login: headers['Authorization']='Basic '+base64.b64encode(('admin:'+PASSWORD).encode()).decode()
    if cookie: headers['Cookie']=cookie
    headers.update(extra or {})
    connection=(http.client.HTTPConnection(parsed.hostname,5444,timeout=8) if plain_http else
                http.client.HTTPSConnection(parsed.hostname,parsed.port,timeout=8,context=ssl._create_unverified_context()))
    with closing(connection) as c:
        c.request(method,'/ugreen-toolbox/'+path,headers=headers)
        response=c.getresponse()
        return response.status,dict(response.getheaders()),response.read()


def verify_desktop_proxy():
    status,headers,body=proxy_request()
    assert status==200 and b'login-form' in body
    assert headers['X-Frame-Options']=='SAMEORIGIN' and "frame-ancestors 'self'" in headers['Content-Security-Policy']
    assert 'WWW-Authenticate' not in headers
    assert proxy_request('native-login.js')[0]==200
    assert proxy_request('api/settings')[0]==401
    assert proxy_request(plain_http=True)[0]==403
    status,headers,_=proxy_request('session','POST',login=True)
    assert status==200
    cookie=headers['Set-Cookie'].split(';',1)[0]
    for flag in ('Secure','HttpOnly','SameSite=Strict','Path=/ugreen-toolbox/'):
        assert flag in headers['Set-Cookie']
    status,_,body=proxy_request(cookie=cookie)
    assert status==200 and b'login-form' not in body and b'./assets/' in body
    import re
    asset=re.search(rb'src="\./([^"]+)"',body).group(1).decode()
    assert proxy_request(asset,cookie=cookie)[0]==200
    assert proxy_request('api/settings',cookie=cookie)[0]==200
    if not LED_AUTH:
        status,_,body=proxy_request('api/led/off','POST',cookie=cookie)
        assert status==403 and 'cross-origin' not in json.loads(body)['error']
    for origin in ('','https://evil.test'):
        status,headers,body=proxy_request('api/led/off','POST',cookie=cookie,origin=origin,
                                        extra={'X-Forwarded-Host':'evil.test','X-Forwarded-Proto':'https'})
        assert status==403 and 'cross-origin' in json.loads(body)['error']
        assert 'Access-Control-Allow-Origin' not in headers
    assert proxy_request('session/logout','POST',cookie=cookie)[0]==200
    assert proxy_request('api/settings',cookie=cookie)[0]==401
    passed('real_https_nginx_desktop_login_assets_api_csrf_and_logout')
    return proxy_request('session','POST',login=True)[1]['Set-Cookie'].split(';',1)[0]


def main():
    if os.geteuid()!=0: raise SystemExit('requires disposable root test container')
    # Standard Python container images may only provide /usr/local/bin/python3.
    # A DSM firmware probe can instead bind the real DSM interpreter here.
    if not Path('/usr/bin/python3').exists(): Path('/usr/bin/python3').symlink_to(sys.executable)
    Path('/etc/passwd').open('a').write('ugreen-toolbox:x:20001:20002:SPK fixture:/nonexistent:/usr/sbin/nologin\n')
    Path('/etc/group').open('a').write('ugreen-toolbox:x:20001:\nunrelated-primary:x:20002:\n')
    extracted=Path('/spk/extracted')
    unpack(Path('/spk/input.spk').read_bytes(),extracted)
    package=Path('/volume1/@appstore/UGreenToolbox')
    unpack((extracted/'package.tgz').read_bytes(),package,owner=(UID,GID))
    Path('/volume1').chmod(0o755)
    parent=Path('/var/packages/UGreenToolbox')
    parent.mkdir(parents=True)
    (parent/'target').symlink_to(package)
    var=parent/'var';var.mkdir();os.chown(var,UID,GID)
    upgrade='--upgrade-authorization' in sys.argv or '--upgrade-preserve' in sys.argv
    env=dict(os.environ,SYNOPKG_PKG_STATUS='UPGRADE' if upgrade else 'INSTALL',wizard_admin_password='' if upgrade else PASSWORD,
             wizard_led_control=str('--authorize-led' in sys.argv).lower(),
             wizard_bios_control=str('--authorize-bios' in sys.argv).lower(),
             wizard_update_hardware_authorization=str('--upgrade-authorization' in sys.argv).lower())
    if upgrade:
        state=Path('/volume1/@UGreenToolbox');state.mkdir(mode=0o755)
        (state/'auth').mkdir(mode=0o750)
        salt=b'0123456789abcdef'
        (state/'auth/auth.json').write_text(json.dumps({'username':'admin','salt':salt.hex(),
            'password_hash':hashlib.scrypt(PASSWORD.encode(),salt=salt,n=16384,r=8,p=1).hex()}))
        (state/'auth/auth.json').chmod(0o640)
        (state/'policy.json').write_text(json.dumps({'led_control':'--upgrade-preserve' in sys.argv,
                                                    'bios_control':'--upgrade-preserve' in sys.argv,'public_origin':''}))
        (state/'policy.json').chmod(0o600)
    for name in ('preinst','postinst'):
        invalid=subprocess.run(['/bin/sh',str(extracted/'scripts'/name)],user=UID,group=GID,extra_groups=[],
                               env=dict(env,wizard_led_control='true;invalid'),capture_output=True,text=True)
        assert invalid.returncode!=0
        assert not (var/'install-authorization.json').exists()
    passed('invalid_wizard_values_rejected_before_persistence')
    for name in ('preinst','postinst'):
        p=subprocess.run(['/bin/sh',str(extracted/'scripts'/name)],user=UID,group=GID,extra_groups=[],env=env,capture_output=True,text=True)
        assert p.returncode==0,p.stderr
    if not upgrade: assert (var/'setup.json').stat().st_uid==UID
    passed('non_root_install_scripts_and_one_use_credentials')
    # A bounded, trusted fixture journal; never reads the VM/host's real journal.
    records = [
        {'UNIT': 'pkg-UGreenToolbox-hardware.service', 'MESSAGE': 'root broker startup fixture', '__REALTIME_TIMESTAMP': '1793347200000000'},
        {'_SYSTEMD_UNIT': 'pkg-UGreenToolbox-hardware.service', 'MESSAGE': 'password=root-startup-CANARY\nsecret-continuation-CANARY'},
        {'_SYSTEMD_UNIT': 'other.service', 'MESSAGE': 'other-service-CANARY'},
    ]
    journal=Path('/usr/bin/journalctl')
    journal.write_text('#!'+sys.executable+'\nimport sys\nassert sys.argv[1:4] == ["--no-pager","--output=json","--lines=2000"]\n'
                       +'assert sys.argv[4]=="--all" and len(sys.argv)==9\nsys.stdout.write('+repr('\n'.join(json.dumps(x) for x in records)+'\n')+')\n')
    journal.chmod(0o755)
    unit=(extracted/'conf/systemd/pkg-UGreenToolbox-hardware.service').read_text()
    command=shlex.split(next(line[10:] for line in unit.splitlines() if line.startswith('ExecStart=')))
    RESULT['runtime_python']=subprocess.check_output([command[0],'-I','-S','-c','import sys;print(sys.version.split()[0])'],text=True).strip()
    hardware_log=Path('/tmp/native-hardware.log').open('w+')
    web_log=Path('/tmp/native-web.log').open('w+')
    collector_log=Path('/tmp/native-collector.log').open('w+')
    notification=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
    notification.bind('/tmp/native-notify.sock')
    notification.settimeout(45)
    hardware_env=dict(os.environ,NOTIFY_SOCKET='/tmp/native-notify.sock')
    collector_notification=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
    collector_notification.bind('/tmp/native-collector-notify.sock');collector_notification.settimeout(45)
    collector_env=dict(os.environ,NOTIFY_SOCKET='/tmp/native-collector-notify.sock')
    collector_unit=(extracted/'conf/systemd/pkg-UGreenToolbox-logs.service').read_text()
    collector_command=shlex.split(next(line[10:] for line in collector_unit.splitlines() if line.startswith('ExecStart=')))
    hardware=None;web=None;collector=None
    try:
        collector=subprocess.Popen(collector_command,env=collector_env,stdout=collector_log,stderr=subprocess.STDOUT,start_new_session=True)
        assert collector_notification.recv(128)==b'READY=1'
        hardware=subprocess.Popen(command,env=hardware_env,stdout=hardware_log,stderr=subprocess.STDOUT,start_new_session=True)
        if '--expect-lock-conflict' in sys.argv:
            assert hardware.wait(timeout=30)!=0
            hardware_log.seek(0)
            assert 'BlockingIOError' in hardware_log.read()
            with socket.socket(socket.AF_UNIX) as client:
                client.connect('/run/ugreen-nas-native/ipc/logs.sock')
                client.sendall(b'{"method":"GET","target":"/healthz"}\n')
                assert json.loads(client.recv(1024))['status']==200
            passed('log_collector_remains_ready_during_shared_hardware_lock_conflict')
            passed('native_broker_rejects_container_owned_host_lock')
            return
        assert notification.recv(128)==b'READY=1'
        assert Path('/run/ugreen-nas-native/ipc/hardware.sock').exists()
        passed('systemd_ready_is_sent_after_private_runtime_and_socket_are_ready')
        end=time.monotonic()+40
        while not Path('/run/ugreen-nas-native/ipc/hardware.sock').exists():
            if hardware.poll() is not None:
                hardware_log.seek(0);raise RuntimeError(hardware_log.read())
            assert time.monotonic()<end
            time.sleep(.2)
        web_unit=(extracted/'conf/systemd/pkg-UGreenToolbox-web.service').read_text()
        web_command=shlex.split(next(line[10:] for line in web_unit.splitlines() if line.startswith('ExecStart=')))
        web=subprocess.Popen(web_command,user=UID,group=GID,extra_groups=[],stdout=web_log,stderr=subprocess.STDOUT,start_new_session=True)
        ready()
        for action in ('start','status','stop'):
            check=subprocess.run(['/bin/sh',str(extracted/'scripts/start-stop-status'),action],user=UID,group=GID,extra_groups=[],capture_output=True,text=True)
            assert check.returncode==0,check.stderr
        passed('package_user_control_scripts_only_check_health_of_system_dependencies')
        assert json.loads(request()[1])['distribution']=='spk'
        assert Path('/volume1/@UGreenToolbox/auth/auth.json').stat().st_gid==GID
        assert Path('/run/ugreen-nas-native/ipc/hardware.sock').stat().st_gid==GID
        passed('dedicated_package_group_overrides_unrelated_account_primary_group')
        assert b'login-form' in request('/',False)[1]
        assert request('/api/settings',False)[0]==401
        assert request('/api/settings')[0]==200
        assert not (var/'setup.json').exists()
        assert not (var/'install-authorization.json').exists()
        platform=json.loads(request('/api/platform/status')[1])
        assert (platform['led_control'],platform['bios_control'])==(LED_AUTH,BIOS_AUTH)
        passed('installer_authorization_reaches_root_policy_and_api_without_hardware_writes')
        passed('native_web_broker_authentication_and_secret_cleanup')
        session_cookie=verify_desktop_proxy() if os.environ.get('UGREEN_TEST_DESKTOP_PROXY') else None
        data=Path('/volume1/@UGreenToolbox/data')
        log_dir=data/'log';log_dir.mkdir(exist_ok=True,mode=0o700)
        for index in range(6):
            name='app.log'+('.'+str(index) if index else '')
            (log_dir/name).write_text('[2026-10-30T08:00:00+0000] password=application-CANARY\n'
                                     '[2026-10-30T08:00:01+0000] rotation='+name+'\n')
        def check_export():
            status,_,body=proxy_request('api/logs/export','POST',cookie=session_cookie)
            assert status==200,body[:300]
            result=json.loads(body)
            assert result['redacted'] and not result['incomplete'],result['sources']
            with zipfile.ZipFile(io.BytesIO(base64.b64decode(result['data_base64']))) as archive:
                assert all('application/app.log'+('.'+str(i) if i else '') in archive.namelist() for i in range(6))
                text='\n'.join(archive.read(name).decode() for name in archive.namelist())
                assert 'CANARY' not in text,text
                assert 'root broker startup fixture' in text
            return result
        if session_cookie:
            check_export()
            assert proxy_request('api/logs/export','POST')[0]==401
            assert proxy_request('api/logs/export','POST',cookie=session_cookie,origin='https://evil.test')[0]==403
            assert proxy_request('api/logs/export?path=/etc/shadow','POST',cookie=session_cookie)[0]==400
            passed('authenticated_export_contains_application_rotations_root_startup_and_no_secret_canaries')
        # Even a user with the socket's group cannot impersonate the dedicated Web UID.
        proof="import socket\ns=socket.socket(socket.AF_UNIX)\ns.connect('/run/ugreen-nas-native/ipc/logs.sock')\ns.sendall(b'{\"method\":\"GET\",\"target\":\"/healthz\"}\\n')\ntry:\n assert s.recv(1024)==b''\nexcept ConnectionResetError: pass"
        assert subprocess.run([sys.executable,'-c',proof],user=20003,group=GID,extra_groups=[]).returncode==0
        passed('log_socket_rejects_wrong_uid_even_with_group_access')
        current=Path('/run/ugreen-nas-native/current').resolve()
        target=current/'app/server/portable_runtime.py'
        proof="from pathlib import Path\np=Path("+repr(str(target))+ ")\ndenied=False\ntry: p.write_text('tampered')\nexcept PermissionError: denied=True\nassert denied"
        assert subprocess.run([sys.executable,'-c',proof],user=UID,group=GID,extra_groups=[]).returncode==0
        policy=Path('/volume1/@UGreenToolbox/policy.json')
        policy_data=json.loads(policy.read_text())
        assert (policy_data['led_control'],policy_data['bios_control'])==(LED_AUTH,BIOS_AUTH)
        assert policy.stat().st_mode&0o777==0o600 and policy.stat().st_uid==0
        auth=Path('/volume1/@UGreenToolbox/auth/auth.json')
        auth_hash=hashlib.sha256(auth.read_bytes()).hexdigest()
        state=Path('/volume1/@UGreenToolbox/data/persistence-fixture')
        state.write_text('retained')
        passed('root_sealed_code_and_policy_not_package_writable')
        # Compromising package-owned content cannot alter the private runtime.
        original=(package/'native-payload.tgz').read_bytes()
        (package/'native-payload.tgz').write_bytes(b'untrusted replacement')
        encoded=ast.literal_eval(ast.parse(command[-1]).body[-1].value.args[0].args[0])
        source=base64.b64decode(encoded).decode()
        tree=ast.parse(source);config=ast.literal_eval(tree.body[-1].value.args[0])
        # A compromised package account cannot replay consent after first start.
        replay=var/'install-authorization.json'
        replay.write_text(json.dumps({'version':config['version'],'apply':True,'led_control':not LED_AUTH,'bios_control':not BIOS_AUTH}))
        replay.chmod(0o600);os.chown(replay,UID,GID)
        namespace={};exec(compile(ast.Module(body=tree.body[:-1],type_ignores=[]),'<root-unit>','exec'),namespace)
        try:
            namespace['activate'](package/'native-payload.tgz',Path('/volume1/fresh-cache'),config['sha256'])
            raise AssertionError('untrusted payload was accepted')
        except ValueError as exc:
            assert 'checksum' in str(exc)
        os.killpg(hardware.pid,signal.SIGKILL);hardware.wait(timeout=10)
        if session_cookie:
            assert request('/healthz',False)[0]==200
            assert request('/api/settings')[0]==503
            check_export()
            passed('hardware_broker_failure_keeps_desktop_health_and_log_export_available')
        hardware=subprocess.Popen(command,env=hardware_env,stdout=hardware_log,stderr=subprocess.STDOUT,start_new_session=True)
        assert notification.recv(128)==b'READY=1'
        ready()
        assert Path('/run/ugreen-nas-native/current').resolve()==current
        assert hashlib.sha256(auth.read_bytes()).hexdigest()==auth_hash and state.read_text()=='retained'
        policy_data=json.loads(policy.read_text())
        assert (policy_data['led_control'],policy_data['bios_control'])==(LED_AUTH,BIOS_AUTH)
        passed('package_user_cannot_replay_authorization_on_broker_restart')
        passed('untrusted_package_payload_rejected_and_cached_trusted_restart_recovers')
        # Trusted upgrade changes the pinned digest in the root-owned system unit.
        os.killpg(web.pid,signal.SIGTERM);web.wait(timeout=10);web=None
        os.killpg(hardware.pid,signal.SIGTERM);hardware.wait(timeout=30);hardware=None
        os.killpg(collector.pid,signal.SIGTERM);collector.wait(timeout=15);collector=None
        with tarfile.open(fileobj=io.BytesIO(original),mode='r:gz') as archive:
            files={m.name:archive.extractfile(m).read() for m in archive}
        info=json.loads(files['app/app-info.json']);info['version']='3.0.1-native-upgrade-fixture'
        files['app/app-info.json']=json.dumps(info).encode()
        output=io.BytesIO()
        with tarfile.open(fileobj=output,mode='w:gz') as archive:
            for name,data in sorted(files.items()):
                member=tarfile.TarInfo(name);member.size=len(data);archive.addfile(member,io.BytesIO(data))
        updated=output.getvalue();(package/'native-payload.tgz').write_bytes(updated)
        config['sha256']=hashlib.sha256(updated).hexdigest()
        upgrade_source=source.rsplit('\nboot(',1)[0]+'\nboot('+repr(config)+')\n'
        upgrade_command=[config['python'],'-I','-S','-c',upgrade_source]
        upgrade_log_source=source.rsplit('\nboot(',1)[0]+'\nboot('+repr(dict(config,role='logs'))+')\n'
        collector=subprocess.Popen([config['python'],'-I','-S','-c',upgrade_log_source],env=collector_env,stdout=collector_log,stderr=subprocess.STDOUT,start_new_session=True)
        assert collector_notification.recv(128)==b'READY=1'
        hardware=subprocess.Popen(upgrade_command,env=hardware_env,stdout=hardware_log,stderr=subprocess.STDOUT,start_new_session=True)
        assert notification.recv(128)==b'READY=1'
        end=time.monotonic()+40
        while Path('/run/ugreen-nas-native/current').resolve()==current:
            assert time.monotonic()<end
            time.sleep(.3)
        web=subprocess.Popen(web_command,user=UID,group=GID,extra_groups=[],stdout=web_log,stderr=subprocess.STDOUT,start_new_session=True)
        ready()
        assert json.loads(request()[1])['version']==info['version']
        assert hashlib.sha256(auth.read_bytes()).hexdigest()==auth_hash and state.read_text()=='retained'
        policy_data=json.loads(policy.read_text())
        assert (policy_data['led_control'],policy_data['bios_control'])==(LED_AUTH,BIOS_AUTH)
        passed('trusted_restart_and_upgrade_preserve_installer_authorization')
        passed('trusted_native_upgrade_switches_code_preserves_auth_and_data')
        if session_cookie:
            assert proxy_request('api/settings',cookie=session_cookie)[0]==401
            session_cookie=proxy_request('session','POST',login=True)[1]['Set-Cookie'].split(';',1)[0]
            check_export()
            passed('native_upgrade_and_service_restarts_preserve_log_history_and_export')
            passed('web_restart_invalidates_browser_session_preserves_install_password')
    finally:
        if sys.exc_info()[0] is not None:
            for label,file in [('hardware',hardware_log),('web',web_log),('collector',collector_log)]:
                file.flush();file.seek(0);print('Native '+label+' diagnostics:\n'+file.read(),flush=True)
        for process in (web,hardware,collector):
            if process is not None and process.poll() is None:
                os.killpg(process.pid,signal.SIGTERM);process.wait(timeout=30)
        hardware_log.close();web_log.close();collector_log.close()
        notification.close()
        collector_notification.close()
        Path('/results/native-spk.json').write_text(json.dumps(RESULT,indent=2)+'\n')


if __name__=='__main__':
    main()
