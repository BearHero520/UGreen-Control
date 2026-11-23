import base64
import io
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'app/server'))
import log_export as logs

STAMP = '[2026-10-30T08:00:00+0000] '
CANARIES = {
    'password': 'short-pass-CANARY', 'access_token': 'short-token-CANARY',
    'Authorization': 'Bearer short-auth-CANARY', 'Cookie': 'sid=short-cookie-CANARY',
    'api_key': 'short-api-CANARY', 'credential': 'short-cred-CANARY',
    'csrf': 'short-csrf-CANARY', 'synosid': 'short-sid-CANARY',
    'private_key': 'short-private-CANARY', 'session': 'short-session-CANARY',
    'pwd': 'short-pwd-CANARY', '密码': 'short-chinese-CANARY',
}


def unpack(result):
    with zipfile.ZipFile(io.BytesIO(base64.b64decode(result['data_base64']))) as archive:
        return {name: archive.read(name).decode() for name in archive.namelist()}


class RedactionTests(unittest.TestCase):
    def test_credentials_json_shell_encoded_keys_and_continuations(self):
        for key, canary in CANARIES.items():
            for template in ('{}={}', '"{}": "{}"', '{}:\n{}\ncontinued-value-CANARY'):
                text = STAMP + template.format(key, canary) + '\n' + STAMP + 'operation finished\n'
                result = logs.redact(text)
                self.assertNotIn(canary, result)
                self.assertNotIn('continued-value-CANARY', result)
                self.assertIn('operation finished', result)
        for key in ('pass%77ord', 'pass\\u0077ord', 'pass\\x77ord', 'pass\u200bword', 'ｐａｓｓｗｏｒｄ'):
            self.assertNotIn('encoded-CANARY', logs.redact(STAMP + key + '=encoded-CANARY\n'))
        for scheme in ('Bearer', 'Basic'):
            self.assertNotIn('bare-auth-CANARY', logs.redact(STAMP + scheme + ' bare-auth-CANARY\n'))

    def test_pem_network_paths_url_userinfo_and_device_identifiers(self):
        values = ['192.168.8.42', '2001:db8::1234', 'aa:bb:cc:dd:ee:ff', '0011.2233.4455', 'alice@example.com',
                  '/volume1/private-share/finance.xlsx', '/homes/alice/keys', 'nas.private.test']
        text = '\n'.join(STAMP + value for value in values[:-1]) + '\n'
        text += STAMP + 'URL=https://nas.private.test/path\n'
        text += STAMP + 'https://alice:url-pass-CANARY@private.test/\n'
        text += STAMP + '-----BEGIN PRIVATE KEY-----\nshort-PEM-CANARY\n-----END PRIVATE KEY-----\n'
        for key in ('host', 'user', 'hostname', 'username', 'serial_number', 'Serial Number', 'wwn', 'machine_id', '序列号'):
            text += STAMP + key + '=identity-CANARY\n'
        result = logs.redact(text)
        for value in values + ['url-pass-CANARY', 'short-PEM-CANARY', 'identity-CANARY']:
            self.assertNotIn(value, result)

    def test_no_hardware_collection_or_config_in_export_contract(self):
        with patch.object(logs, 'application_logs', return_value=({}, [])):
            result = logs.export_logs('/unused', include_journal=False)
        files = unpack(result)
        self.assertTrue(result['redacted'] and result['incomplete'])
        self.assertEqual(set(files), {'manifest.json', 'README.txt', 'services/root-startup.log'})
        self.assertNotIn('auth.json', ''.join(files))


@unittest.skipUnless(os.name == 'posix' and os.geteuid() == 0, 'root-only disposable Linux filesystem fixtures')
class CollectorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.data = Path(self.temp.name)
        self.directory = self.data / 'log'; self.directory.mkdir(mode=0o700)

    def tearDown(self):
        self.temp.cleanup()

    def test_zip_has_all_current_and_rotated_logs_and_no_canaries(self):
        content = '\n'.join(STAMP + key + '=' + value for key, value in CANARIES.items()) + '\n'
        for name in logs.LOG_NAMES:
            (self.directory / name).write_text(content + STAMP + 'rotation=' + name + '\n')
        (self.directory / 'auth.json').write_text('MUST-NOT-COLLECT')
        journal = lambda: (STAMP + 'password=journal-CANARY\n' + STAMP + 'root service started\n', {'status': 'included'})
        result = logs.export_logs(self.data, journal_reader=journal)
        files = unpack(result)
        self.assertFalse(result['incomplete'])
        self.assertEqual(len([x for x in files if x.startswith('application/')]), 11)
        self.assertIn('root service started', files['services/root-startup.log'])
        all_text = '\n'.join(files.values())
        for value in list(CANARIES.values()) + ['MUST-NOT-COLLECT', 'journal-CANARY']:
            self.assertNotIn(value, all_text)

    def test_reject_links_fifo_writable_and_wrong_owner(self):
        target = self.data / 'private'; target.write_text('link-CANARY')
        (self.directory / 'app.log').symlink_to(target)
        os.link(target, self.directory / 'app.log.1')
        os.mkfifo(self.directory / 'app.log.2')
        path = self.directory / 'app.log.3'; path.write_text('owner-CANARY'); os.chown(path, 20001, 20001)
        path = self.directory / 'app.log.4'; path.write_text('writable-CANARY'); path.chmod(0o666)
        files, manifest = logs.application_logs(self.data)
        self.assertFalse(files)
        self.assertEqual(sum(x['status'] == 'unsafe_or_unreadable' for x in manifest), 5)

    def test_tail_drops_sensitive_continuation_and_incomplete_records(self):
        path = self.directory / 'app.log'
        path.write_text(STAMP + 'password=\n' + 'sensitive-tail-CANARY\n' * 20000 +
                        STAMP + 'safe structured event\n' + STAMP + 'token=partial-CANARY')
        files, manifest = logs.application_logs(self.data)
        self.assertIn('safe structured event', files['application/app.log'])
        self.assertNotIn('CANARY', files['application/app.log'])
        self.assertTrue(manifest[0]['truncated'])

    def test_journal_filters_units_fields_and_redacts_multiline(self):
        records = [
            {'_SYSTEMD_UNIT': logs.UNITS[0], 'MESSAGE': 'root startup safe', '__REALTIME_TIMESTAMP': '1793347200000000',
             '_CMDLINE': 'password=cmdline-CANARY', '_HOSTNAME': 'hostname-CANARY'},
            {'UNIT': logs.UNITS[0], 'MESSAGE': 'token:\nmultiline-CANARY'},
            {'_SYSTEMD_UNIT': 'other.service', 'MESSAGE': 'other-unit-CANARY'},
        ]
        fixture = self.data / 'journal.py'
        fixture.write_text('import sys\nsys.stdout.write(' + repr('\n'.join(json.dumps(x) for x in records) + '\n') + ')\n')
        with patch.object(logs, 'journal_command', return_value=[sys.executable, str(fixture)]):
            text, manifest = logs.journal_logs()
        self.assertEqual(manifest['records'], 2)
        self.assertEqual(manifest['status'], 'included')
        self.assertIn('root startup safe', text)
        self.assertNotIn('CANARY', text)

    def test_journal_unavailable_is_explicit_and_request_paths_are_fixed(self):
        with patch.object(logs, 'journal_command', return_value=None):
            result = logs.export_logs(self.data)
        self.assertTrue(result['incomplete'])
        with patch.object(logs, 'journal_command', return_value=[sys.executable, '-c', 'pass']):
            self.assertEqual(logs.journal_logs()[1]['status'], 'no_records')
        path = str(self.data / 'logs.sock')
        with logs.LogServer(path, logs.LogHandler) as server:
            server.web_uid = 20001; server.data_dir = self.data; server.export_lock = threading.Lock()
            worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
            try:
                for request in ({'method': 'POST', 'target': '/logs/export?path=/etc/shadow', 'body': ''},
                                {'method': 'POST', 'target': '/logs/export', 'body': '/etc/shadow'},
                                {'method': 'GET', 'target': '/logs/export'}, []):
                    with socket.socket(socket.AF_UNIX) as client:
                        client.connect(path); client.sendall(json.dumps(request).encode() + b'\n')
                        self.assertEqual(json.loads(client.recv(2048))['status'], 400)
                server.export_lock.acquire()
                with socket.socket(socket.AF_UNIX) as client:
                    client.connect(path); client.sendall(b'{"method":"POST","target":"/logs/export","body":""}\n')
                    self.assertEqual(json.loads(client.recv(2048))['status'], 429)
                server.export_lock.release()
            finally:
                server.shutdown(); worker.join()


if __name__ == '__main__':
    unittest.main()
