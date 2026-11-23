"""Verify production CLI DMI routing in QEMU without I2C or host passthrough."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT.parent / 'UGREEN-NAS-Hardware'
WORK = ROOT / '.work-almighty-release/dxp4800gt'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=WORK / 'led-build/ugreen_leds_cli')
    args = parser.parse_args()
    runner = [sys.executable, str(UPSTREAM / 'tools/testing/run_linux.py'),
              '--runtime-root', str(UPSTREAM / 'firmware-inbox/work/almighty-tools')]
    outputs = []
    cases = [
        ('DXP4800 GT', 'DXP4800 GT V1.0 20260429', 5, 'i2c-dev is unavailable'),
        ('DXP4800 GT-extra', '', 3, 'no LED protocol profile'),
        ('Default string', 'iDX6011_iDX6012 V3.0 241112', 5, 'i2c-dev is unavailable'),
        ('iDX6012', 'iDX6011_iDX6012 V3.0 241112', 5, 'i2c-dev is unavailable'),
        ('Default string', 'iDX6011_iDX6012 V3.1 241112', 3, 'no LED protocol profile'),
        ('Default string', 'unrelated', 3, 'no LED protocol profile'),
        ('iDX6012-other', 'iDX6011_iDX6012 V3.0 241112', 3, 'no LED protocol profile'),
        ('DXP2800', 'EM_DXP2800_V1.0.26', 5, 'i2c-dev is unavailable'),
    ]
    for product, version, code, marker in cases:
        env = dict(os.environ, UGREENCTL_TEST_VM_DMI=product, UGREENCTL_TEST_VM_DMI_VERSION=version)
        result = subprocess.run([*runner, str(args.binary), 'all', '-status', '--force'],
                                env=env, capture_output=True, text=True)
        output = result.stdout + result.stderr
        outputs.append(f'{product} / {version}: rc={result.returncode}\n{output}')
        (WORK / 'runtime-tests.log').write_text(''.join(outputs), encoding='utf-8')
        if result.returncode != code or marker not in output:
            raise AssertionError(outputs[-1])
        print(f'{product} / {version}: expected rc={code}')
    init_cases = [
        ('iDX6012', ['power', '-initialize-idx6012', '--json'], 0, '"dry_run":true'),
        ('iDX6012', ['power', '-initialize-idx6012', '--apply', '--json'], 4, 'experimental_confirmation'),
        ('iDX6012', ['power', '-initialize-idx6012', '--force', '--apply'], 5, 'i2c-dev is unavailable'),
        ('iDX6012', ['all', '-initialize-idx6012'], 2, 'use power'),
        ('iDX6012', ['power', '-initialize-idx6012', '-status'], 2, 'use power'),
        ('DXP4800 Plus', ['power', '-initialize-idx6012'], 3, 'exact iDX6012 profile'),
    ]
    for product, cli, code, marker in init_cases:
        env = dict(os.environ, UGREENCTL_TEST_VM_DMI=product,
                   UGREENCTL_TEST_VM_DMI_VERSION='iDX6011_iDX6012 V3.0 241112')
        result = subprocess.run([*runner, str(args.binary), *cli], env=env, capture_output=True, text=True)
        output = result.stdout + result.stderr
        outputs.append(f'{product} {cli}: rc={result.returncode}\n{output}')
        (WORK / 'runtime-tests.log').write_text(''.join(outputs), encoding='utf-8')
        if result.returncode != code or marker not in output:
            raise AssertionError(outputs[-1])
        print(f'{product} {cli}: expected rc={code}')
    receipt = dict(binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                   cases=len(cases)+len(init_cases), physical_validation='not-performed',
                   test_source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (WORK / 'runtime-tests.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
