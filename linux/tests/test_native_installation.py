import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
bootstrap=None
if os.name=='posix':
    spec=importlib.util.spec_from_file_location('native_bootstrap',ROOT/'synology/native_bootstrap.py')
    bootstrap=importlib.util.module_from_spec(spec);spec.loader.exec_module(bootstrap)


@unittest.skipUnless(os.name=='posix' and os.geteuid()==0,'requires disposable root Linux fixture')
class InstallationTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.base=Path(self.temp.name)
        self.state=self.base/'state';self.state.mkdir(mode=0o700)
        self.var=self.base/'var';self.var.mkdir(mode=0o700)
        self.policy=self.state/'policy.json'
        self.request=self.var/'install-authorization.json'
        self.uid=20001
        self.reset()

    def tearDown(self): self.temp.cleanup()

    def reset(self,led=False,bios=False):
        self.policy.write_text(json.dumps({'led_control':led,'bios_control':bios,'public_origin':'https://nas.test:5001'}))
        self.policy.chmod(0o600)

    def choices(self,led=False,bios=False,apply=True,version='3.0.1-1005'):
        self.request.write_text(json.dumps({'version':version,'apply':apply,'led_control':led,'bios_control':bios}))
        os.chown(self.request,self.uid,self.uid);self.request.chmod(0o600)

    def consume(self,version='3.0.1-1005'):
        return bootstrap.installation_authorization(self.state,self.var/'setup.json',self.uid,version)

    def test_each_control_plane_and_both_or_neither_can_be_authorized(self):
        for led,bios in ((False,False),(True,False),(False,True),(True,True)):
            self.reset();self.choices(led,bios);policy=self.consume()
            self.assertEqual((policy['led_control'],policy['bios_control']),(led,bios))
            self.assertEqual(policy['public_origin'],'https://nas.test:5001')
            self.assertEqual(self.policy.stat().st_mode&0o777,0o600)
            self.assertEqual(self.policy.stat().st_uid,0)

    def test_upgrade_defaults_preserve_and_explicit_selection_can_disable(self):
        self.reset(True,True);self.choices(False,False,apply=False)
        policy=self.consume()
        self.assertTrue(policy['led_control'] and policy['bios_control'])
        self.choices(False,False,version='3.0.1-1006')
        policy=self.consume('3.0.1-1006')
        self.assertFalse(policy['led_control'] or policy['bios_control'])

    def test_replayed_requests_cannot_change_existing_authorization_on_restart(self):
        self.choices(True,False);self.consume()
        self.choices(False,True)
        policy=self.consume()
        self.assertTrue(policy['led_control']);self.assertFalse(policy['bios_control'])
        self.assertEqual(len(policy['installation_authorizations']),1)

    def test_missing_request_is_consumed_before_web_can_recreate_it(self):
        policy=self.consume()
        self.assertEqual(policy['installation_authorizations']['3.0.1-1005']['result'],'missing')
        self.choices(True,True)
        policy=self.consume()
        self.assertFalse(policy['led_control'] or policy['bios_control'])

    def test_untrusted_schema_permissions_and_links_fail_closed(self):
        for invalid in ('string_boolean','wrong_version','extra_field','oversized','symlink','hardlink','fifo','world_readable','wrong_owner'):
            with self.subTest(invalid=invalid):
                self.reset();self.request.unlink(missing_ok=True);self.choices(True,True)
                if invalid in ('string_boolean','wrong_version','extra_field'):
                    value=json.loads(self.request.read_text())
                    if invalid=='string_boolean':value['led_control']='true'
                    if invalid=='wrong_version':value['version']='3.0.1-9999'
                    if invalid=='extra_field':value['command']='untrusted'
                    self.request.write_text(json.dumps(value))
                elif invalid=='oversized':self.request.write_text('x'*4097)
                elif invalid in ('symlink','hardlink'):
                    target=self.var/('target-'+invalid);target.write_bytes(self.request.read_bytes())
                    self.request.unlink()
                    if invalid=='symlink':self.request.symlink_to(target)
                    else:os.link(target,self.request);os.chown(self.request,self.uid,self.uid)
                elif invalid=='fifo':self.request.unlink();os.mkfifo(self.request)
                elif invalid=='world_readable':self.request.chmod(0o644)
                elif invalid=='wrong_owner':os.chown(self.request,0,0)
                policy=self.consume()
                self.assertFalse(policy['led_control'] or policy['bios_control'])
                self.assertEqual(policy['installation_authorizations']['3.0.1-1005']['result'],'rejected')

    def test_policy_symlink_and_hardlink_are_rejected(self):
        target=self.state/'other.json';target.write_bytes(self.policy.read_bytes())
        self.policy.unlink();self.policy.symlink_to(target)
        with self.assertRaises(ValueError):self.consume()
        self.policy.unlink();os.link(target,self.policy)
        with self.assertRaises(ValueError):self.consume()


if __name__=='__main__':unittest.main()
