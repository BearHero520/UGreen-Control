#!/usr/bin/env python3
"""Read-only non-Pro iDX6012 EC evidence collection; never writes EC memory.

Only the firmware-evidenced fan and G3 fields are read. No module loading,
owner eviction, output-buffer draining, or arbitrary register scanning.
"""
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import time


def identity():
    root = Path('/sys/class/dmi/id')
    info = {k: (root / k).read_text().strip() for k in
            ('sys_vendor', 'product_name', 'product_version', 'bios_version')}
    if info['sys_vendor'] != 'UGREEN' or not (
        info['product_name'] == 'iDX6012' or
        (info['product_name'] == 'Default string' and
         info['product_version'] == 'iDX6011_iDX6012 V3.0 241112')):
        raise RuntimeError('Exact iDX6012/confirmed shared OEM board required')
    return info


def owners():
    for p in ['/proc/it86', '/sys/module/ug_idx6011_sio',
              '/sys/module/ug_idx6011pro_sio', '/sys/module/ug_4800gt_sio']:
        if os.path.lexists(p):
            raise RuntimeError('EC owner: ' + p)
    if Path('/sys/module/ec_sys').exists():
        # fnOS builds ec_sys into the kernel even when there is no active EC.
        # Permit this case only with explicit config and an empty, mounted
        # debugfs EC namespace. A loadable driver is always rejected.
        config = Path('/boot/config-' + os.uname().release).read_text().splitlines()
        modules = Path('/proc/modules').read_text().splitlines()
        mounts = Path('/proc/mounts').read_text().splitlines()
        if ('CONFIG_ACPI_EC_DEBUGFS=y' not in config or
            any(x.startswith('ec_sys ') for x in modules) or
            not any(x.split()[1:3] == ['/sys/kernel/debug', 'debugfs'] for x in mounts) or
            Path('/sys/kernel/debug/ec').exists() or
            any(p.is_symlink() for p in Path('/sys/bus/acpi/drivers/ec').iterdir())):
            raise RuntimeError('EC debug driver ownership not proven idle')
    for p in Path('/sys/bus/acpi/devices').glob('PNP0C09:*'):
        if int((p / 'status').read_text().strip(), 0) != 0 or (p / 'driver').exists():
            raise RuntimeError('Active ACPI EC: ' + str(p))
    visible = False
    for line in Path('/proc/ioports').read_text().splitlines():
        m = re.match(r'\s*([0-9a-f]+)-([0-9a-f]+) : (.*)', line)
        if not m:
            continue
        lo, hi = int(m[1], 16), int(m[2], 16)
        visible |= bool(lo or hi)
        if any(lo <= p <= hi for p in (0x62, 0x66)) and not m[3].startswith('PCI Bus '):
            raise RuntimeError('Port owner: ' + line)
    if not visible:
        raise RuntimeError('Port ownership unavailable')


def main():
    before = identity()
    owners()
    lock = os.open('/run/ugreenctl-it55-ec.lock',
                   os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        st = os.fstat(lock)
        if not stat.S_ISREG(st.st_mode) or st.st_uid or st.st_nlink != 1:
            raise RuntimeError('Unsafe lock file')
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        owners()
        fd = os.open('/dev/port', os.O_RDWR | os.O_CLOEXEC)
        try:
            def inp(port):
                value = os.pread(fd, 1, port)
                if len(value) != 1:
                    raise RuntimeError('Short port read')
                return value[0]

            def wait(mask, state):
                end = time.monotonic() + 0.1
                while time.monotonic() < end:
                    value = inp(0x66)
                    if value == 255:
                        raise RuntimeError('EC absent')
                    if bool(value & mask) == state:
                        return
                    time.sleep(0.0001)
                raise RuntimeError('EC timeout')

            def read(reg):
                if inp(0x66) & 1:
                    raise RuntimeError('EC has pending output; refusing to drain')
                wait(2, False)
                if os.pwrite(fd, b'\x80', 0x66) != 1:
                    raise RuntimeError('Short command write')
                wait(2, False)
                if os.pwrite(fd, bytes([reg]), 0x62) != 1:
                    raise RuntimeError('Short address write')
                wait(1, True)
                return inp(0x62)

            samples = []
            for _ in range(3):
                owners()
                if identity() != before:
                    raise RuntimeError('Identity changed')
                regs = {f'{r:02x}': read(r) for r in
                        (0x96, 0x97, 0x98, 0x99, 0x9c, 0x9d, 0x9e, 0x9f, 0xa6)}
                samples.append({'registers': regs,
                                'sysfan1_rpm': regs['96'] * 256 + regs['97'],
                                'sysfan2_rpm': regs['98'] * 256 + regs['99']})
                time.sleep(1)
            print(json.dumps({'identity': before, 'samples': samples,
                              'ec_memory_writes': 0}, indent=2))
        finally:
            os.close(fd)
    finally:
        os.close(lock)


if __name__ == '__main__':
    main()
