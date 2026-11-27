# DXP2800 EM_DXP2800_V1.0.26 fan and BIOS support

Evidence date: 2026-11-01 (Asia/Shanghai). This change adds fan and AC-recovery control, and a read-only WOL interface listing.
No LED implementation or LED profile is changed in this repository.

## Inputs and evidence

- Read-only input: `E:/工作台/固件/6800plus.img`, UGOS Pro 1.20.0.0142 universal image.
- Image SHA-256: `73862c9c619f3e8b2279252326654f4915234dc8f437a71eb9a0067a724062c5`.
- Existing extraction: `firmware-inbox/work/dxp6800plus/`.
- Driver: `kernel/usr/lib/modules/6.18.15/kernel/drivers/ugreen/ug_it86x-sio.ko`.
- Driver SHA-256: `83c95d12f988f7ce8fd2f9e673a84beb71f4e091335f32c79b00ac28d178ac4a`.
- Diagnostic: `ugreen-led-diagnostics-20261031-150123.txt`, DMI product `DXP2800`,
  product_version / BIOS `EM_DXP2800_V1.0.26`; it87 absent and no fan hwmon node.

`fw/usr/sbin/ug-load-drive.sh:57..70` explicitly routes DXP2800 to this driver;
`fw/etc/startpre.d/hwmonitor.sh:26..42` selects the generic software hwmonitor
for EM_DXP2800 rather than the later JW board. No channel is inferred merely
from a different model's similar name.

Disassembly retained in `firmware-inbox/work/dxp2800-em26/ug_it86x-sio.analysis.txt`:

| Entry | Confirmed interface |
| --- | --- |
| it87x_init .init.text+0x10, comparison +0xc6 | chip ID 0x8613, SIO 0x2e/0x2f |
| set_fan.isra.0 .text+0x210, +0x264..0x2a7 | LDN 4; HWM 0xa35/0xa36; clear control 0x17 bit7; duty 0x73 |
| fan_read .text+0x4c0, +0x54a..0x594 | LDN 4 must be active; tach high 0x1a, low 0x0f |
| fan_read +0x596..0x660 | 0, 0x0fff, 0xffff invalid; RPM = 1350000 / (2*tach) |
| set_bios .text+0x6a0 / startup_read .text+0x10 | LDN4 0xf2 bit5, 0xf4 bits5/6; on=0/01, off=0/11, restore=1/11 |

The it87 hwmon correspondence is pwm3/fan3_input. An independent DXP2800 map
uses the shared IT8613 hwmon implementation first; only absence (-ENODEV)
permits direct fallback. EIO/EACCES/EBUSY never bypass hwmon. The direct path
checks active HWM, chip ID, driver ownership and the shared process lock, and
verifies control bit and PWM readback. It never loads/unloads it87.

## Scope and limits

Exact DMI product AND product_version are re-read before reads and writes.
Unknown, empty, prefixed/suffixed, V1.2/JW and GT revisions are refused before
hardware access. sys is the only fan target; PWM 40..255, --force and --apply
are required for writes. Fan/power plugin ABI is unchanged. AC recovery reuses the shared guarded IT8613 implementation after the same
exact revision check, with --force --apply and complete register readback.
Hardware automatic fan mode remains unavailable. WOL/RTC use the independently
firmware-mapped built-in wake CLI, without inventing an automatic NIC map.
`network wol interfaces` lists only eligible physical Ethernet devices using
the existing identity validator. WOL still requires an explicit administrator
selection and Magic Packet capability/readback checks. RTC retains its unique
rtc_cmos identity, UTC clock, time-window, exclusive lock and alarm readback guards.

The software watchdog gains the single sys target for custom curves; every
write still passes through the guarded plugin. An original factory curve is
not claimed or fabricated. Thermal response, actual PWM/RPM behavior and this
board's wiring remain pending physical validation; this is firmware evidence
and isolated fixture coverage, not a physical test report.

## Verification and application integration

Linux x86_64 Release cross-build and full isolated QEMU CTest: 35/35 passed.
New tests cover exact revision, missing identity, wrong boards, channel/PWM
guards, hwmon preference, absence-only fallback and propagation of read/write
errors. Existing shared tests cover locks, chip/owner checks and readback.
No host hardware is passed to QEMU. Shared AC, RTC and WOL fixtures cover
policy encoding, preservation of unrelated bits, driver/clock/capability errors,
identity changes, failed writes and readback. The new NIC listing fixture
excludes loopback, bridges and wireless and asserts zero ioctl writes.

LLLED_FPK is modified separately to import dxp2800.so / ugreenctl-fand and adapt
its API/telemetry. Its LED-only patch is built and tested inside LLLED_FPK's
isolated pinned source snapshot, never applied to this repository.
