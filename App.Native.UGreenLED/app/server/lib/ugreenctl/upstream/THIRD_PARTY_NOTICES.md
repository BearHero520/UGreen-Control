# Third-party notices

## Runtime implementation

LED control and the optional `ugreen_leds_cli` compatibility executable are
implemented and built in this repository in C. They do not install, link to,
invoke, or bundle the external LED CLI binary or its kernel module.
The references below record protocol provenance and historical evidence, rather
than software that users need to install.

## Legacy protocol reference and attribution

The in-tree legacy LED protocol implementation retains its source attribution to
[miskcoo/ugreen_leds_controller](https://github.com/miskcoo/ugreen_leds_controller),
MIT license, copyright (c) 2024 miskcoo@gmail.com. The full license is preserved in
[led/LICENSE.upstream-led](led/LICENSE.upstream-led) and installed with the documentation.
The source attribution in `led/led_protocol.c` is retained.

Reference source commit: `f0fcd8192abdd92244aba9f6f0d10572f5561380`.
The local `git archive` snapshot is
`firmware-inbox/work/almighty-upstream/ugreen_leds_controller-f0fcd8192abdd92244aba9f6f0d10572f5561380.tar`,
SHA-256 `e83c4c840ede2c945db3903ca64ecee3bdabf345a1ab5eefea3422931142152c`.
The extracted reference directory is `firmware-inbox/work/almighty-upstream/source/`.
Neither the snapshot nor the extracted files are included in Git or release archives.

Reference material includes the
[legacy protocol source](https://github.com/miskcoo/ugreen_leds_controller/blob/f0fcd8192abdd92244aba9f6f0d10572f5561380/cli/ugreen_leds.cpp),
[DXP2800 revision differences](https://github.com/miskcoo/ugreen_leds_controller/issues/128),
[different protocols sharing a chip identity](https://github.com/miskcoo/ugreen_leds_controller/pull/100),
and [first-hand physical bay reports](https://github.com/miskcoo/ugreen_leds_controller/issues/9).

## Disk-mapping evidence

The upstream physical slot reports used by the new sysfs implementation, their
limitations, calibration instructions and attribution are in
[disk/NOTICE.md](disk/NOTICE.md).

## Linux interfaces and N76

Linux I2C userspace API documentation:
<https://www.kernel.org/doc/html/latest/i2c/dev-interface.html>.
No Linux kernel or systemd implementation source is copied into this project.

N76 userspace transactions are independently written from the documented wire protocol in the official UGREEN source. Provenance and original SHA-256 values are retained in `tools/n76/sources.json` and `docs/N76_DIRECT_CONTROL.zh-CN.md`. No upstream kernel implementation or driver compiler is bundled.
