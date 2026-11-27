import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('hardware_build', Path(__file__).resolve().parents[1] / 'scripts/build_ugreenctl.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class ImportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        elf = bytearray(64)
        elf[:7] = b'\x7fELF\x02\x01\x01'
        elf[16:18] = (3).to_bytes(2, 'little')
        elf[18:20] = (62).to_bytes(2, 'little')
        names = [*(f'usr/local/bin/{p}' for p in build.PROGRAMS), *(f'usr/local/lib/ugreenctl/models/{m}.so' for m in build.MODELS)]
        self.files = {name: bytes(elf) for name in names}
        self.files['usr/local/share/doc/ugreenctl/LICENSE'] = b'notice'

    def bundle(self, extra=None):
        manifest = dict(package='fixture', architecture='x86_64', branch='Almighty', status='experimental', files=[dict(path=n, size=len(d), sha256=build.digest(d)) for n, d in self.files.items()])
        raw = json.dumps(manifest).encode()
        (self.root / 'manifest.json').write_bytes(raw)
        with tarfile.open(self.root / 'archive.tar.gz', 'w:gz') as archive:
            for name, data in [*self.files.items(), ('manifest.json', raw), *((extra,) if extra else ())]:
                member = tarfile.TarInfo('fixture/' + name)
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
        lock = dict(archive='archive.tar.gz', manifest='manifest.json', tag='preview', commit='a' * 40)
        for key in ('archive', 'manifest'):
            lock[key + '_sha256'] = build.digest((self.root / lock[key]).read_bytes())
        sums = ''.join(f'{lock[k + "_sha256"]}  {lock[k]}\n' for k in ('archive', 'manifest')).encode()
        (self.root / 'SHA256SUMS').write_bytes(sums)
        lock['checksums_sha256'] = build.digest(sums)
        return lock

    def test_import_replaces_all_programs_and_records_published_payload(self):
        lock = self.bundle()
        manifest, files = build.validate_release(self.root, lock)
        server = self.root / 'server'
        (server / 'bin').mkdir(parents=True)
        (server / 'bin/ugreen_leds_cli').write_bytes(b'old LED')
        build.install_release(manifest, files, lock, server)
        receipt = json.loads((server / 'lib/ugreenctl/build-info.json').read_text())
        self.assertEqual(len(receipt['files']), len(build.PROGRAMS) + len(build.MODELS))
        self.assertEqual(receipt['release_tag'], 'preview')
        for p in build.PROGRAMS:
            self.assertEqual((server / 'bin' / p).read_bytes(), self.files[f'usr/local/bin/{p}'])
        self.assertEqual((server / 'lib/ugreenctl/upstream/LICENSE').read_bytes(), b'notice')

    def test_import_preserves_compatibility_wrapper_and_records_compat_elf(self):
        lock = self.bundle()
        manifest, files = build.validate_release(self.root, lock)
        server = self.root / 'server'
        (server / 'bin').mkdir(parents=True)
        wrapper = b'#!/bin/bash\n# Compatibility entry point\nexec "$0.compat" "$@"\n'
        (server / 'bin/ugreen_leds_cli').write_bytes(wrapper)
        build.install_release(manifest, files, lock, server)
        receipt = json.loads((server / 'lib/ugreenctl/build-info.json').read_text())
        self.assertEqual((server / 'bin/ugreen_leds_cli').read_bytes(), wrapper)
        self.assertEqual((server / 'bin/ugreen_leds_cli.compat').read_bytes(), self.files['usr/local/bin/ugreen_leds_cli'])
        self.assertEqual(receipt['files']['ugreen_leds_cli'], build.digest(wrapper))
        self.assertEqual(receipt['files']['ugreen_leds_cli.compat'], build.digest(self.files['usr/local/bin/ugreen_leds_cli']))

    def test_branch_download_pins_commit_and_preserves_binary_bytes(self):
        lock = self.bundle()
        lock.update(source_type='git-artifact', repository='owner/repo',
                    artifact_commit='b' * 40, artifact_path='artifacts/snapshot')
        expected = b'\x00\xff\r\nraw'
        with patch.object(build.urllib.request, 'urlopen', side_effect=lambda *a, **k: io.BytesIO(expected)) as fetch:
            build.download_assets(lock, self.root)
        self.assertEqual(fetch.call_count, 3)
        for call in fetch.call_args_list:
            self.assertIn('/' + 'b' * 40 + '/artifacts/snapshot/', call.args[0])
        self.assertEqual((self.root / lock['archive']).read_bytes(), expected)

    def test_local_application_artifact_uses_application_repository(self):
        lock = self.bundle()
        lock.update(source_type='application-local-artifact', artifact_path='release')
        directory = self.root / 'release'; directory.mkdir()
        for name in (lock['archive'], lock['manifest'], 'SHA256SUMS'):
            (directory / name).write_bytes((self.root / name).read_bytes())
        destination = self.root / 'download'; destination.mkdir()
        with patch.object(build, 'REPO_ROOT', self.root):
            build.download_assets(lock, destination)
        self.assertEqual((destination / lock['archive']).read_bytes(), (directory / lock['archive']).read_bytes())
        lock['artifact_path'] = '../escape'
        with patch.object(build, 'REPO_ROOT', self.root), self.assertRaisesRegex(ValueError, 'escapes'):
            build.download_assets(lock, destination)

    def test_wrong_architecture(self):
        name = 'usr/local/bin/ugreen_leds_cli'
        data = bytearray(self.files[name]); data[18:20] = (183).to_bytes(2, 'little')
        self.files[name] = bytes(data)
        with self.assertRaisesRegex(ValueError, 'Wrong ELF'):
            build.validate_release(self.root, self.bundle())

    def test_missing_plugin(self):
        del self.files['usr/local/lib/ugreenctl/models/dxp2800.so']
        with self.assertRaisesRegex(ValueError, 'Missing required'):
            build.validate_release(self.root, self.bundle())

    def test_tampered_archive(self):
        lock = self.bundle()
        with (self.root / 'archive.tar.gz').open('ab') as f: f.write(b'tampered')
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            build.validate_release(self.root, lock)

    def test_unsafe_path(self):
        lock = self.bundle(('../escape', b'bad'))
        with self.assertRaisesRegex(ValueError, 'Unsafe archive'):
            build.validate_release(self.root, lock)

    def test_duplicate_member(self):
        lock = self.bundle(('usr/local/bin/ugreenctl', b'duplicate'))
        with self.assertRaisesRegex(ValueError, 'Duplicate archive'):
            build.validate_release(self.root, lock)

if __name__ == '__main__':
    unittest.main()
