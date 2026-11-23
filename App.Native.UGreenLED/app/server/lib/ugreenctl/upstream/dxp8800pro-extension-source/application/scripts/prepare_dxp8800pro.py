"""Compose tested upstream DXP8800 Pro controls over the current local pin.

Does not build an FPK, publish, or change the independent LED executable.
Run upstream CMake/CTest first, then this script and build_ugreenctl.py.
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
LOCK = ROOT / 'scripts/hardware-release.json'
ID = 'dxp8800pro-20261104'

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
    manifest, payload = module.validate_release(owner / base['artifact_path'], base,
        models=tuple(m for m in module.MODELS if m != 'dxp8800pro'))
    payload.pop('manifest.json')
    led_before = sha(payload['usr/local/bin/ugreen_leds_cli'])
    modes = {item['path']: int(item['mode'], 8) for item in manifest['files']}
    build = UPSTREAM / 'build-dxp2800'
    replacements = {
        'usr/local/bin/ugreenctl': build / 'ugreenctl',
        'usr/local/bin/ugreenctl-fand': build / 'ugreenctl-fand',
        'usr/local/lib/ugreenctl/models/dxp8800pro.so': build / 'models/dxp8800pro.so',
    }
    for name, source in replacements.items():
        payload[name] = source.read_bytes()
        modes[name] = 0o755
    hashes = {}
    prefix = 'usr/local/share/doc/ugreenctl/'
    for directory in ('core', 'fan', 'power', 'superio', 'include', 'models', 'network'):
        for path in sorted((UPSTREAM / directory).rglob('*')):
            if path.suffix not in ('.c', '.h'):
                continue
            relative = path.relative_to(UPSTREAM).as_posix()
            data = path.read_bytes()
            hashes[relative] = sha(data)
            payload[prefix + 'dxp8800pro-extension-source/' + relative] = data
    for relative in ('CMakeLists.txt', 'tests/test_dxp8800pro_plugin.c',
                     'tests/test_it8613_dxp8800pro.c', 'tests/test_thermal_curve.c',
                     'scripts/package-almighty.py'):
        data = (UPSTREAM / relative).read_bytes()
        hashes[relative] = sha(data)
        payload[prefix + 'dxp8800pro-extension-source/' + relative] = data
    for relative in ('docs/FAN_CURVE.md', 'docs/HARDWARE_SUPPORT.md',
                     'docs/DXP8800_PRO_AUDIT.zh-CN.md', 'models/compatibility.json'):
        data = (UPSTREAM / relative).read_bytes()
        hashes[relative] = sha(data)
        payload[prefix + relative] = data
    for relative in ('scripts/prepare_dxp8800pro.py', 'tests/test_dxp8800pro_runtime.c',
                     'tests/run_dxp8800pro_runtime.py'):
        data = (ROOT / relative).read_bytes()
        hashes['application/' + relative] = sha(data)
        payload[prefix + 'dxp8800pro-extension-source/application/' + relative] = data
    manifest['local_extension'] = dict(id=ID, base_archive_sha256=base['archive_sha256'],
        base_local_extension=manifest.get('local_extension'), source_sha256=hashes,
        changed_payloads=sorted(replacements), physical_validation='not-performed',
        led_binary_sha256=led_before)
    manifest.update(status='experimental-local-dxp8800pro', verification_complete=False)
    manifest['files'] = [dict(path=name, size=len(data), mode=f'{modes.get(name, 0o644):04o}', sha256=sha(data))
        for name, data in sorted(payload.items())]
    raw = encoded(manifest)
    payload['manifest.json'] = raw
    assert sha(payload['usr/local/bin/ugreen_leds_cli']) == led_before
    buffer = io.BytesIO()
    with gzip.GzipFile(fileobj=buffer, mode='wb', mtime=0, filename='') as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as archive:
            for name, data in sorted(payload.items()):
                member = tarfile.TarInfo(manifest['package'] + '/' + name)
                member.size, member.mode = len(data), modes.get(name, 0o644)
                archive.addfile(member, io.BytesIO(data))
    out = ROOT / '.work-almighty-release/dxp8800pro/release'
    out.mkdir(parents=True, exist_ok=True)
    assets = {base['archive']: buffer.getvalue(), base['manifest']: raw}
    assets['SHA256SUMS'] = ''.join(f'{sha(data)}  {name}\n' for name, data in assets.items()).encode()
    for name, data in assets.items():
        (out / name).write_bytes(data)
    lock = dict(repository=base['repository'], tag='local-' + ID, commit=base['commit'],
        source_type='application-local-artifact', extension_id=ID,
        artifact_path=out.relative_to(ROOT).as_posix(), base=base,
        archive=base['archive'], archive_sha256=sha(assets[base['archive']]),
        manifest=base['manifest'], manifest_sha256=sha(raw), checksums_sha256=sha(assets['SHA256SUMS']))
    module.validate_release(out, lock)
    LOCK.write_bytes(encoded(lock))
    print('Prepared DXP8800 Pro inputs; LED binary preserved: ' + led_before)

if __name__ == '__main__':
    main()
