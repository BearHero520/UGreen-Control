"""Import the tested GT LED CLI and upstream disk/EC changes over the current pin.

Run the upstream CMake/CTest build, build_led_compat.py --dxp4800gt,
and tests/run_dxp4800gt_runtime.py first.
Creates build inputs only, not an FPK, commit, push, or release.
"""
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT.parent / 'UGREEN-NAS-Hardware'
WORK = ROOT / '.work-almighty-release/dxp4800gt'
LOCK = ROOT / 'scripts/hardware-release.json'
ID = 'dxp4800gt-fnos-20261116'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + '\n').encode()


def main():
    spec = importlib.util.spec_from_file_location('hardware_import', ROOT / 'scripts/build_ugreenctl.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    current = json.loads(LOCK.read_text(encoding='utf-8'))
    base = current['base'] if current.get('extension_id') == ID else current
    owner = ROOT if base.get('source_type') == 'application-local-artifact' else UPSTREAM
    manifest, payload = module.validate_release(owner / base['artifact_path'], base)
    payload.pop('manifest.json')
    original = dict(payload)
    led = json.loads((WORK / 'led-build.json').read_text(encoding='utf-8'))
    runtime = json.loads((WORK / 'runtime-tests.json').read_text(encoding='utf-8'))
    binary = (WORK / 'led-build/ugreen_leds_cli').read_bytes()
    if sha(binary) != led['binary_sha256'] or sha(binary) != runtime['binary_sha256']:
        raise ValueError('LED binary differs from tested binary')
    for name, expected in led['source_sha256'].items():
        if sha((ROOT / name).read_bytes()) != expected:
            raise ValueError('Source changed since fixture tests: ' + name)
    if sha((ROOT / 'tests/run_dxp4800gt_runtime.py').read_bytes()) != runtime['test_source_sha256']:
        raise ValueError('Runtime tests changed since validation')
    payload['usr/local/bin/ugreen_leds_cli'] = binary
    modes = {item['path']: int(item['mode'], 8) for item in manifest['files']}
    sources = [*led['source_sha256'], 'scripts/prepare_dxp4800gt_diagnostics.py',
               'tests/run_dxp4800gt_runtime.py', 'docs/DXP4800GT_LED_SUPPORT.md']
    hashes = {}
    prefix = 'usr/local/share/doc/ugreenctl/dxp4800gt-fnos-extension/'
    for name in sources:
        data = (ROOT / name).read_bytes()
        hashes[name] = sha(data)
        payload[prefix + name] = data
    build = UPSTREAM / 'firmware-inbox/work/dxp4800gt-ca2e91fa/upstream-build'
    changed = ('usr/local/bin/ugreen_leds_cli', 'usr/local/bin/ugreenctl',
               'usr/local/lib/ugreenctl/models/dxp4800gt.so')
    payload[changed[1]] = (build / 'ugreenctl').read_bytes()
    payload[changed[2]] = (build / 'models/dxp4800gt.so').read_bytes()
    if b'device\tinterface\tslot_name\tled' not in payload[changed[1]]:
        raise ValueError('Upstream binary lacks the disk topology TSV contract')
    for name in ('core/hardware_cli.c', 'core/hardware_cli.h', 'core/main.c',
                 'disk/disk.c', 'disk/firmware_profiles.c', 'docs/ALL_MODELS_DISK_EVIDENCE.zh-CN.md',
                 'ec/it55_dxp4800gt.c', 'ec/it55_dxp4800gt.h', 'docs/DXP4800GT_HARDWARE_AUDIT.zh-CN.md',
                 'tests/test_disk_mapping.c', 'tests/test_hardware_cli.c', 'tests/test_it55_dxp4800gt.c'):
        data = (UPSTREAM / name).read_bytes()
        payload[prefix + 'upstream/' + name] = data
        hashes['upstream/' + name] = sha(data)
    # Preserve the daemon and unrelated plugins/programs from the current pin.
    for name, data in original.items():
        if name not in changed and not name.startswith(prefix) and payload[name] != data:
            raise ValueError('Unexpected base payload change: ' + name)
    manifest['local_extension'] = dict(id=ID, base_archive_sha256=base['archive_sha256'],
        base_local_extension=manifest.get('local_extension'), source_sha256=hashes,
        changed_payloads=list(changed), led_build=led,
        runtime_tests=runtime, physical_validation='not-performed')
    manifest.update(status='experimental-local-dxp4800gt-fnos', verification_complete=False)
    manifest['files'] = [dict(path=n, size=len(d), mode=f'{modes.get(n, 0o644):04o}', sha256=sha(d))
                         for n, d in sorted(payload.items())]
    raw = encoded(manifest)
    payload['manifest.json'] = raw
    buffer = io.BytesIO()
    with gzip.GzipFile(fileobj=buffer, mode='wb', mtime=0, filename='') as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as archive:
            for name, data in sorted(payload.items()):
                member = tarfile.TarInfo(manifest['package'] + '/' + name)
                member.size, member.mode = len(data), modes.get(name, 0o644)
                archive.addfile(member, io.BytesIO(data))
    out = WORK / 'release-fnos-20261116'
    out.mkdir(parents=True, exist_ok=True)
    assets = {base['archive']: buffer.getvalue(), base['manifest']: raw}
    assets['SHA256SUMS'] = ''.join(f'{sha(d)}  {n}\n' for n, d in assets.items()).encode()
    for name, data in assets.items():
        (out / name).write_bytes(data)
    lock = dict(repository=base['repository'], tag='local-' + ID, commit=base['commit'],
        source_type='application-local-artifact', extension_id=ID,
        artifact_path=out.relative_to(ROOT).as_posix(), base=base,
        archive=base['archive'], archive_sha256=sha(assets[base['archive']]),
        manifest=base['manifest'], manifest_sha256=sha(raw), checksums_sha256=sha(assets['SHA256SUMS']))
    module.validate_release(out, lock)
    LOCK.write_bytes(encoded(lock))
    print('Prepared GT LED/disks/EC inputs; unrelated runtime payloads preserved')


if __name__ == '__main__':
    main()
