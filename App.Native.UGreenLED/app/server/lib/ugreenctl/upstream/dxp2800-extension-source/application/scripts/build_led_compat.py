"""Build/test the LED-only EM26 patch in an isolated pinned source snapshot.

No source file in UGREEN-NAS-Hardware is modified. The output is an input to
prepare_dxp2800.py; it is not an FPK or a published release.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT.parent / 'UGREEN-NAS-Hardware'
REVISION = 'aff7991ee58709ad34b5afbc2787f0d21300368d'
WORK = ROOT / '.work-almighty-release/dxp2800'
PATCH = ROOT / 'App.Native.UGreenLED/app/server/vendor/ugreen_leds_controller/patches/dxp2800-em26.patch'

def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cmake', type=Path, default=SOURCE / 'firmware-inbox/work/tools/cmake-python/cmake/data/bin/cmake.exe')
    parser.add_argument('--zig', type=Path, default=SOURCE / 'firmware-inbox/work/tools/zig-0.15.2/zig.exe')
    args = parser.parse_args()
    source = WORK / 'led-source'
    build = WORK / 'led-build'
    source.mkdir(parents=True, exist_ok=True)
    # Only source trees are read from the fixed commit, never moving or dirty LED sources.
    directories = ['CMakeLists.txt', 'core', 'include', 'superio', 'fan', 'power',
                   'network', 'models', 'led', 'disk', 'ec', 'tests', 'tools']
    archive = subprocess.check_output(['git', '-C', str(SOURCE), 'archive', REVISION, *directories])
    with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
        for member in stream:
            if not member.isfile(): continue
            target = source / member.name
            if not target.resolve().is_relative_to(source.resolve()):
                raise ValueError('Unsafe source path')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(stream.extractfile(member).read())
    # A nested directory otherwise inherits the application's Git root and
    # git apply can silently skip paths outside its prefix.
    run(['git', 'init', '--quiet', source])
    run(['git', 'apply', '--check', PATCH], cwd=source)
    run(['git', 'apply', PATCH], cwd=source)
    test = ROOT / 'tests/test_dxp2800_led.c'
    cmake_file = source / 'CMakeLists.txt'
    cmake_file.write_text(cmake_file.read_text() + f'''
add_executable(test-dxp2800-led "{test.as_posix()}" tests/test_led_gpio.c tests/test_led_n76.c
    led/led.c led/led_n76_protocol.c led/led_protocol.c led/led_sysfs.c led/led_gpio.c
    led/led_service.c led/led_cs201x.c models/led_profiles.c core/identity.c)
target_include_directories(test-dxp2800-led PRIVATE ${{CMAKE_CURRENT_SOURCE_DIR}} ${{CMAKE_CURRENT_SOURCE_DIR}}/include)
target_compile_definitions(test-dxp2800-led PRIVATE UGREENCTL_LED_TESTING)
target_compile_options(test-dxp2800-led PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME dxp2800-led COMMAND test-dxp2800-led)
''', encoding='utf-8')
    env = dict(os.environ, UGREENCTL_ZIG=str(args.zig))
    configure = [args.cmake, '-S', source, '-B', build, '-G', 'Ninja',
                 '-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_MAKE_PROGRAM=' + str(SOURCE / 'firmware-inbox/work/tools/cmake-python/bin/ninja.exe'),
                 '-DCMAKE_TOOLCHAIN_FILE=' + str(SOURCE / 'tools/toolchains/zig-linux.cmake')]
    run(configure, env=env)
    run([args.cmake, '--build', build, '--target', 'ugreen_leds_cli', 'test-dxp2800-led', '-j', '4'], env=env)
    result = subprocess.run([sys.executable, str(SOURCE / 'tools/testing/run_linux.py'),
                            '--runtime-root', str(SOURCE / 'firmware-inbox/work/almighty-tools'),
                            str(build / 'test-dxp2800-led')], capture_output=True, text=True)
    (WORK / 'led-tests.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    print(result.stdout, end='')
    result.check_returncode()
    receipt = dict(source_revision=REVISION, patch_sha256=hashlib.sha256(PATCH.read_bytes()).hexdigest(),
                   binary_sha256=hashlib.sha256((build / 'ugreen_leds_cli').read_bytes()).hexdigest(),
                   physical_validation='not-performed')
    (WORK / 'led-build.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')

if __name__ == '__main__': main()
