"""Test installed Pro binaries in QEMU; no host hardware or network passthrough."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT.parent / 'UGREEN-NAS-Hardware'
WORK = ROOT / '.work-almighty-release/dxp8800pro/runtime'

def main():
    (WORK / 'models').mkdir(parents=True, exist_ok=True)
    subprocess.run([str(UPSTREAM / 'firmware-inbox/work/tools/zig-0.15.2/zig.exe'), 'cc',
        '-target', 'x86_64-linux-gnu', '-O2', '-Wall', '-Wextra',
        str(ROOT / 'tests/test_dxp8800pro_runtime.c'), '-o', str(WORK / 'test-runtime')], check=True)
    server = ROOT / 'App.Native.UGreenLED/app/server'
    for source, target in [(server / 'bin/ugreenctl', 'cli.so'),
                           (server / 'bin/ugreenctl-fand', 'fand.so'),
                           (server / 'lib/ugreenctl/models/dxp8800pro.so', 'dxp8800pro.so')]:
        shutil.copy2(source, WORK / 'models' / target)
    logs = []
    for model, args in [('DXP8800 Pro', []), ('DXP8800 Plus', ['reject'])]:
        result = subprocess.run([sys.executable, str(UPSTREAM / 'tools/testing/run_linux.py'),
            '--runtime-root', str(UPSTREAM / 'firmware-inbox/work/almighty-tools'),
            str(WORK / 'test-runtime'), *args],
            env=dict(os.environ, UGREENCTL_TEST_VM_DMI=model), capture_output=True, text=True)
        logs.append(result.stdout + result.stderr)
        print(logs[-1], end='')
        result.check_returncode()
    (WORK.parent / 'runtime-tests.log').write_text(''.join(logs), encoding='utf-8')

if __name__ == '__main__':
    main()
