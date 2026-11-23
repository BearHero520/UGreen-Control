"""Exercise the installed EM26 controls in QEMU with /tmp hwmon fixtures only."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT.parent / 'UGREEN-NAS-Hardware'
WORK = ROOT / '.work-almighty-release/dxp2800'

def main():
    runtime = WORK / 'runtime'; (runtime / 'models').mkdir(parents=True, exist_ok=True)
    subprocess.run([str(UPSTREAM / 'firmware-inbox/work/tools/zig-0.15.2/zig.exe'), 'cc',
                    '-target', 'x86_64-linux-gnu', '-O2', '-Wall', '-Wextra',
                    str(ROOT / 'tests/test_dxp2800_runtime.c'), '-o', str(runtime / 'test-runtime')], check=True)
    server = ROOT / 'App.Native.UGreenLED/app/server'
    for source, name in [(server / 'bin/ugreenctl', 'cli.so'),
                         (server / 'bin/ugreenctl-fand', 'fand.so'),
                         (server / 'lib/ugreenctl/models/dxp2800.so', 'dxp2800.so')]:
        shutil.copy2(source, runtime / 'models' / name)
    runner = UPSTREAM / 'tools/testing/run_linux.py'
    base = [sys.executable, str(runner), '--runtime-root', str(UPSTREAM / 'firmware-inbox/work/almighty-tools')]
    outputs = []
    for version, args in [('EM_DXP2800_V1.0.26', []), ('DXP2800 V1.2 251215', ['reject'])]:
        env = dict(os.environ, UGREENCTL_TEST_VM_DMI='DXP2800', UGREENCTL_TEST_VM_DMI_VERSION=version)
        result = subprocess.run([*base, str(runtime / 'test-runtime'), *args], env=env, capture_output=True, text=True)
        outputs.append(result.stdout + result.stderr); result.check_returncode()
    # Production LED binary must select EM26, then fail because this VM has no
    # I2C devices. The unlisted EM27 must fail at profile selection instead.
    for version, code, marker in [('EM_DXP2800_V1.0.26', 5, 'i2c-dev is unavailable'),
                                  ('EM_DXP2800_V1.0.27', 3, 'no LED protocol profile')]:
        env = dict(os.environ, UGREENCTL_TEST_VM_DMI='DXP2800', UGREENCTL_TEST_VM_DMI_VERSION=version)
        result = subprocess.run([*base, str(server / 'bin/ugreen_leds_cli'), 'power', 'netdev', 'disk1', 'disk2', '-status'],
                                env=env, capture_output=True, text=True)
        outputs.append(result.stdout + result.stderr)
        if result.returncode != code or marker not in outputs[-1]:
            raise AssertionError(outputs[-1])
    log = ''.join(outputs) + 'Installed EM26 runtime integration tests passed\n'
    (WORK / 'runtime-tests.log').write_text(log, encoding='utf-8')
    print(log, end='')

if __name__ == '__main__': main()
