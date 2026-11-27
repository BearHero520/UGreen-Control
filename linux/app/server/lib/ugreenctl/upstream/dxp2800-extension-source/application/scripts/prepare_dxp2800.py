"""Compose the local EM26 application extension from separately built controls.

Run upstream CMake/CTest and scripts/build_led_compat.py first. This prepares
verified hardware inputs, updates the local pin, and does not build an FPK.
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
WORK = ROOT / '.work-almighty-release/dxp2800'
LOCK = ROOT / 'scripts/hardware-release.json'
BASE_SHA = '08a74ce4acee7f5db0d09380347e5a3e9eb2664c5a885fbd90ee0aafb0e108cc'
ID = 'dxp2800-em26-20261101'

def sha(data): return hashlib.sha256(data).hexdigest()
def encoded(value): return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + '\n').encode()

def main():
    spec = importlib.util.spec_from_file_location('hardware_import', ROOT / 'scripts/build_ugreenctl.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    current = json.loads(LOCK.read_text(encoding='utf-8'))
    base = current['base'] if current.get('extension_id') == ID else current
    if base['archive_sha256'] != BASE_SHA:
        raise ValueError('Expected the pinned DXP6800 curve base; refusing to replace unrelated changes')
    manifest, payload = module.validate_release(UPSTREAM / base['artifact_path'], base)
    payload.pop('manifest.json')
    modes = {item['path']: int(item['mode'], 8) for item in manifest['files']}
    replacements = {
        'usr/local/bin/ugreenctl': UPSTREAM / 'build-dxp2800/ugreenctl',
        'usr/local/lib/ugreenctl/models/dxp2800.so': UPSTREAM / 'build-dxp2800/models/dxp2800.so',
        'usr/local/bin/ugreenctl-fand': UPSTREAM / 'build-dxp2800/ugreenctl-fand',
        'usr/local/bin/ugreen_leds_cli': WORK / 'led-build/ugreen_leds_cli',
    }
    led = json.loads((WORK / 'led-build.json').read_text(encoding='utf-8'))
    if sha(replacements['usr/local/bin/ugreen_leds_cli'].read_bytes()) != led['binary_sha256']:
        raise ValueError('LED binary changed after its fixture tests')
    for name, path in replacements.items(): payload[name] = path.read_bytes(); modes[name] = 0o755
    sources = {'CMakeLists.txt', 'models/dxp2800.c', 'core/fan_daemon.c', 'core/identity.c', 'core/identity.h',
               'tests/test_dxp2800_plugin.c', 'tests/test_it8613_dxp2800.c', 'docs/DXP2800_EM26_SUPPORT.md', 'models/compatibility.json'}
    for directory in ('core', 'fan', 'power', 'network', 'superio', 'include', 'models', 'disk', 'ec', 'led'):
        sources.update(p.relative_to(UPSTREAM).as_posix() for p in (UPSTREAM / directory).rglob('*') if p.suffix in ('.c', '.h'))
    hashes = {}
    prefix = 'usr/local/share/doc/ugreenctl/dxp2800-extension-source/'
    for name in sorted(sources):
        data = (UPSTREAM / name).read_bytes()
        hashes['hardware/' + name] = sha(data); payload[prefix + 'hardware/' + name] = data
    app_sources = ['scripts/build_led_compat.py', 'scripts/prepare_dxp2800.py', 'tests/test_dxp2800_led.c',
                   'tests/test_dxp2800_runtime.c', 'tests/run_dxp2800_runtime.py', 'docs/DXP2800_EM26_SUPPORT.md',
                   'App.Native.UGreenLED/app/server/vendor/ugreen_leds_controller/patches/dxp2800-em26.patch']
    for name in app_sources:
        data = (ROOT / name).read_bytes()
        hashes['application/' + name] = sha(data); payload[prefix + 'application/' + name] = data
    payload['usr/local/share/doc/ugreenctl/docs/DXP2800_EM26_SUPPORT.md'] = (ROOT / 'docs/DXP2800_EM26_SUPPORT.md').read_bytes()
    payload['usr/local/share/doc/ugreenctl/models/compatibility.json'] = (UPSTREAM / 'models/compatibility.json').read_bytes()
    previous = manifest.get('local_extension')
    manifest['local_extension'] = dict(id=ID, base_archive_sha256=BASE_SHA, source_sha256=hashes,
                                       base_local_extension=previous, led_build=led,
                                       changed_payloads=sorted(replacements), physical_validation='not-performed')
    manifest.update(status='experimental-local-dxp2800-em26', verification_complete=False)
    manifest['files'] = [dict(path=name, size=len(data), mode=f'{modes.get(name, 0o644):04o}', sha256=sha(data))
                         for name, data in sorted(payload.items())]
    raw = encoded(manifest); payload['manifest.json'] = raw
    buffer = io.BytesIO()
    with gzip.GzipFile(fileobj=buffer, mode='wb', mtime=0, filename='') as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as archive:
            for name, data in sorted(payload.items()):
                member = tarfile.TarInfo(manifest['package'] + '/' + name)
                member.size, member.mode = len(data), modes.get(name, 0o644)
                archive.addfile(member, io.BytesIO(data))
    out = WORK / 'release'; out.mkdir(parents=True, exist_ok=True)
    assets = {base['archive']: buffer.getvalue(), base['manifest']: raw}
    assets['SHA256SUMS'] = ''.join(f'{sha(data)}  {name}\n' for name, data in assets.items()).encode()
    for name, data in assets.items(): (out / name).write_bytes(data)
    lock = dict(repository=base['repository'], tag='local-' + ID, commit=base['commit'],
                source_type='application-local-artifact', extension_id=ID,
                artifact_path=out.relative_to(ROOT).as_posix(), base=base,
                archive=base['archive'], archive_sha256=sha(assets[base['archive']]),
                manifest=base['manifest'], manifest_sha256=sha(raw), checksums_sha256=sha(assets['SHA256SUMS']))
    module.validate_release(out, lock)
    LOCK.write_bytes(encoded(lock))
    print('Prepared local EM26 control inputs: ' + str(out))

if __name__ == '__main__': main()
