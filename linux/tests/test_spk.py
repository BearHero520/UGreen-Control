import ast
import base64
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shlex
import tarfile
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("spk_builder",ROOT/"scripts/build_spk.py")
builder=importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class SpkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory()
        cls.path,cls.payload_hash=builder.build(Path(cls.temp.name))
        with tarfile.open(cls.path) as archive:
            cls.members={m.name:(archive.extractfile(m).read(),m.mode) for m in archive}
        with tarfile.open(fileobj=io.BytesIO(cls.members['package.tgz'][0]),mode='r:gz') as archive:
            cls.package={m.name:archive.extractfile(m).read() for m in archive}

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_required_layout_lifecycle_and_checksum(self):
        for name in ('INFO','package.tgz','conf/privilege','conf/resource','WIZARD_UIFILES/install_uifile','PACKAGE_ICON.PNG','PACKAGE_ICON_256.PNG'):
            self.assertIn(name,self.members)
        for name in ('preinst','postinst','preupgrade','postupgrade','preuninst','postuninst','start-stop-status'):
            self.assertEqual(self.members['scripts/'+name][1],0o755)
        expected=self.path.with_suffix('.spk.sha256').read_text().split()[0]
        self.assertEqual(expected,hashlib.sha256(self.path.read_bytes()).hexdigest())

    def test_install_and_upgrade_wizards_keep_control_planes_separate_and_explicit(self):
        for name in ('install_uifile', 'upgrade_uifile'):
            pages=json.loads(self.members['WIZARD_UIFILES/'+name][0])
            fields={field['key']:field for page in pages for item in page['items'] for field in item.get('subitems',[])}
            for key in ('wizard_led_control','wizard_bios_control'):
                self.assertIs(fields[key]['defaultValue'],False)
            if name=='upgrade_uifile':
                self.assertIs(fields['wizard_update_hardware_authorization']['defaultValue'],False)
                self.assertNotIn('wizard_admin_password',fields)
        script=self.members['scripts/postinst'][0].decode()
        self.assertIn('install-authorization.json',script)
        self.assertNotIn('policy.json',script)

    def test_package_user_and_separate_root_system_unit(self):
        privilege=json.loads(self.members['conf/privilege'][0])
        self.assertEqual(privilege['defaults'],{'run-as':'package'})
        self.assertNotIn('ctrl-script',privilege)
        web=self.members['conf/systemd/pkg-UGreenToolbox-web.service'][0].decode()
        hardware=self.members['conf/systemd/pkg-UGreenToolbox-hardware.service'][0].decode()
        self.assertIn('User=ugreen-toolbox',web)
        self.assertIn('CapabilityBoundingSet=\n',web)
        self.assertIn('User=root',hardware)
        self.assertIn('Type=notify',hardware)
        self.assertIn('RestrictAddressFamilies=AF_UNIX\n',hardware)
        self.assertNotIn('CAP_SYS_ADMIN',hardware)
        self.assertNotIn('docker',web+hardware)

    def test_root_unit_embeds_verified_snapshot_bootstrap(self):
        unit=self.members['conf/systemd/pkg-UGreenToolbox-hardware.service'][0].decode()
        line=next(line.removeprefix('ExecStart=') for line in unit.splitlines() if line.startswith('ExecStart='))
        command=shlex.split(line)
        self.assertEqual(command[:3],['/usr/bin/python3','-I','-S'])
        expression=ast.parse(command[-1]).body[-1].value.args[0]
        encoded=ast.literal_eval(expression.args[0])
        source=base64.b64decode(encoded).decode()
        config=ast.literal_eval(ast.parse(source).body[-1].value.args[0])
        self.assertEqual(config['sha256'],self.payload_hash)
        self.assertEqual(config['version'],builder.VERSION)
        self.assertEqual(hashlib.sha256(self.package['native-payload.tgz']).hexdigest(),config['sha256'])

    def test_native_payload_preserves_hardware_bytes_and_is_not_container_package(self):
        with tarfile.open(fileobj=io.BytesIO(self.package['native-payload.tgz']),mode='r:gz') as archive:
            cli=archive.extractfile('app/server/bin/ugreen_leds_cli').read()
            self.assertEqual(cli,(ROOT.parent/'App.Native.UGreenLED/app/server/bin/ugreen_leds_cli').read_bytes())
            self.assertIn('app/server/native_runtime.py',archive.getnames())
            self.assertNotIn('Dockerfile',archive.getnames())
        self.assertNotIn(b'ContainerManager',self.members['INFO'][0])

    def test_native_preflight_checks_interpreter_trust_and_glibc(self):
        preflight=self.members['scripts/preinst'][0].decode()
        self.assertIn('st_uid!=0',preflight)
        self.assertIn('CS_GNU_LIBC_VERSION',preflight)
        self.assertIn('(2,27)',preflight)
        self.assertIn('(3,8)',preflight)
        self.assertIn('wizard_admin_password',preflight)
        for name in ('preinst','postinst'):
            script=self.members['scripts/'+name][0].decode()
            ast.parse(script.split("<<'PY'\n",1)[1].rsplit('\nPY',1)[0])

    def test_system_package_dependencies_start_root_services_without_user_systemctl(self):
        self.assertNotIn('conf/systemd/override/pkg-service',self.members)
        for role in ('hardware','web','logs'):
            unit=self.members[f'conf/systemd/pkg-UGreenToolbox-{role}.service'][0].decode()
            self.assertIn('PartOf=pkgctl-UGreenToolbox.service',unit)
            self.assertIn('BindsTo=pkgctl-UGreenToolbox.service',unit)
            self.assertIn(('WantedBy=' if role == 'hardware' else 'RequiredBy=')+'pkgctl-UGreenToolbox.service',unit)
            self.assertIn('DefaultEnable=yes',unit)
            self.assertIn('StandardError=journal',unit)
        logs=self.members['conf/systemd/pkg-UGreenToolbox-logs.service'][0].decode()
        self.assertNotIn('CAP_SYS_RAWIO',logs)
        self.assertNotIn('Requires=pkg-UGreenToolbox-hardware.service',logs)
        script=self.members['scripts/start-stop-status'][0].decode()
        self.assertNotIn('synosystemctl start',script)
        self.assertNotIn('synosystemctl is-active',script)
        self.assertIn('/healthz',script)
        self.assertNotIn(b'install_dep_packages',self.members['INFO'][0])

    def test_dsm_desktop_app_and_automatic_same_origin_proxy_are_registered(self):
        config=json.loads(self.package['ui/config'])
        app=config['UGreenToolbox.js'][builder.DESKTOP_APP]
        self.assertEqual(app['type'],'app')
        self.assertEqual(app['appWindow'],builder.DESKTOP_APP)
        self.assertFalse(app['allUsers'])
        self.assertNotIn('.url',config)
        resources=json.loads(self.members['conf/resource'][0])
        self.assertEqual(resources['web-config']['nginx-static-config']['enable'],[
            {'type':'dsm','relpath':'etc/dsm-desktop.conf'}])
        self.assertIn(b'instuninst_restart_services="nginx.service"',self.members['INFO'][0])
        self.assertIn(b'proxy_pass http://127.0.0.1:5088/',self.package['etc/dsm-desktop.conf'])
        self.assertNotIn(b'$http_cookie',self.package['etc/dsm-desktop.conf'])
        with tarfile.open(fileobj=io.BytesIO(self.package['native-payload.tgz']),mode='r:gz') as archive:
            self.assertIn('app/server/native_desktop.py',archive.getnames())
            self.assertIn('app/www/native-login.html',archive.getnames())


if __name__=='__main__':
    unittest.main()
