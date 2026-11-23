# DXP8800 Pro：2026-11-04 固件与现有接入核查

## 结论

按用户授权，现已接入精确 `DXP8800 Pro`：独立 `dxp8800pro.so` 插件、
独立 IT8613 通道映射、CPU/成对系统 PWM、三路 RPM 与来电恢复。保留
`--force --apply`、芯片识别、所有者互斥、锁、PWM 40..255 与读回。
自动温控为 `stock-8800pro` 软件守护，使用已恢复的 1.17 `dxp8800.conf`；
并非声称恢复了 1.20 的全部默认配置，也不声称已实机验证。
DXP8800、Plus、Ultra 不随此 Pro 接入被自动开放。WOL 自动网卡映射维持原状。

## 输入与证据

- 原包：`E:\工作台\固件\6800plus.img`，1,303,473,152 字节。
- 本次重新计算 SHA-256：`73862c9c619f3e8b2279252326654f4915234dc8f437a71eb9a0067a724062c5`。
- UGOS Pro `1.20.0.0142`，构建 `20260917.094918`，模块目录内核 `6.18.15`。
- 格式：GNU tar，内含 `kernel.squashfs`、`fw.squashfs`、`oem.squashfs`、`apt.squashfs`。
- 官网机型与这个通用包的既有对应记录：`docs/ALL_MODELS_FIRMWARE_INTAKE.zh-CN.md`。
  本次核验本地原包与已有提取物，未把文件名中的 6800 当作硬件兼容依据。
- 提取目录：`firmware-inbox/work/dxp6800plus/`。原始包没有修改。

关键文件（相对于提取目录）：

| 文件 | SHA-256 |
| --- | --- |
| `kernel/usr/lib/modules/6.18.15/kernel/drivers/ugreen/dxp480t/ug_it86x-cpufan.ko` | `dbecee7bcd4920fdc685a3162e7b7402ac12ee0994e7a87f43d1ce08da87ed4b` |
| `fw/usr/sbin/hwmonitor-480t` | `079d7a703e3ebd87aa59540712cba443c67e6863c5decaff187c06e5bcaa1541` |
| `kernel/usr/lib/modules/6.18.15/kernel/drivers/ugreen/dxp68a88/leds-mcu-68a88.ko` | `3bb43319469e2f92ffa357c6935e2198c2cb8359753897c4ded63820071eb9c7` |

## 风扇与来电恢复

`fw/usr/sbin/ug-load-drive.sh:85` 的 DXP8800 前缀分支加载 `ug_it86x-cpufan`。
`fw/etc/startpre.d/hwmonitor.sh` 把这类带 CPU 风扇的型号路由到 `hwmonitor-480t`。
控制器是 IT8613 Super I/O；配置端口为 `0x2e/0x2f`，HWM 为 `0xa35/0xa36`。
自动温控由用户态守护执行，不能声称是硬件自动模式。

反汇编与重定位证据在 `ug_it86x-cpufan.ko.analysis.txt`：

| 能力 | 原厂分支及证据 | 寄存器 |
| --- | --- | --- |
| CPU PWM | `set_cpu_fan.isra.0`：`0xa16` 指向 DXP8800 字符串，`0xa27` 跳到 `0x996` | 清 `0x16` bit 7，写 `0x6b` |
| 系统 PWM | `set_fan.isra.0`：`0xc67` 的 DXP8800 比较，`0xc78` 跳到 `0xd11`；继而到 `0xce0/0xce8` | 清 `0x17` / `0x1e` bit 7，同值写 `0x73` / `0x7b` |
| RPM | `fan_read` 的 DXP8800 比较 `0x3ea..0x401` 接到第三路读取 | CPU `0x0e/0x19`；系统 1 `0x0f/0x1a`；系统 2 `0x80/0x81` |
| 来电恢复 | `set_bios.isra.0` `0x460`；`startup_read` `0x710` | 配置寄存器 `0xf2/0xf4` |

这些是固件静态证据，并非在用户 DXP8800 Pro 上完成的转速、PWM 或断电恢复验证。
接入在此仓库建立精确 Pro 型号、独立映射与测试，复用所有者检查、锁、
芯片识别、PWM 安全下限和回读；不能在 LLLED shell 中直接操作寄存器。

## 曲线不能直接借用 6800

`curve-annotated.txt:1043` 对应 `hwmonitor-480t` 的 `0x9924` DXP8800 分支；
`0x9945` 选择 `/etc/default/dxp8800.conf`，而不是 `dxp6800.conf`。
本次复用的 1.20 提取物未找到完整的 `dxp8800.conf`，因此不把旧配置冒充 1.20 参数。

既有 1.17 提取物
`firmware-inbox/work/dxp4800pro-1.17.0.95/rootfs-base/etc/default/dxp8800.conf`
的 SHA-256 为 `976f3c260cb81858e897d971b6b8862e6cc5aa5e9ab5840b21b2e959fb8741d7`。
它与同目录 `dxp6800.conf` 已存在明确差异：

| 参数 | DXP8800 旧配置 | DXP6800 旧配置 |
| --- | --- | --- |
| CPU 起始 PWM | 70 | 80 |
| CPU 65°C 系统 PWM 保底 | 135 | 125 |
| CPU 90°C 系统 PWM 保底 | 210 | 220 |

因此仅凭相同 IT8613 通道映射，不能把 `stock-6800pro` 标为 8800 原厂曲线。

## WOL 与 LED 独立结论

`models/wake_profiles.c` 已有精确 DXP8800 Pro WOL/RTC 条目，但 WOL 自动映射数为 0，
表示重命名物理网卡的映射未建立，需显式接口。日志中的
`automatic NIC mapping is unconfirmed` 与此一致，并不证明网卡不支持 WOL。

LED 的原厂路由为独立 `leds-mcu-68a88`。其反汇编
`firmware-inbox/work/almighty-led-all/leds-mcu-68a88.ko.disasm.txt` 中
`0x1146` 设置长度 12，`0x114b` 调用 `i2c_smbus_write_i2c_block_data`；
这与兼容 CLI 当前 legacy 分帧方向一致，不支持仅凭日志切换到 SMBus block 协议。
没有实机 DMI/CLI 哈希、原始读回值和所有者状态，尚不能判定日志里的 MCU 错误根因。

## 接入及验证

上游实现：`models/dxp8800pro.c`、`fan/it8613_dxp8800pro.c`、
`core/fan_daemon.c`、`fan/thermal_curve.c`；能力目录与打包白名单同步更新。
独立测试覆盖型号元数据、写入确认、三路映射、系统成对写入、
hwmon 优先且错误不触发直控回退、独立曲线与温度缺失保护。
CMake Release 构建通过；37 个 CTest 均通过（补齐打包白名单后重跑失败项）。

LLLED_FPK 同步应用适配、Pro 专属插件检查、页面说明和 stock 曲线入口。
`scripts/prepare_dxp8800pro.py` 组合本地可验证硬件输入，保留原 LED CLI 字节不变。
随包 CLI/plugin/fand 已在无硬件透传的 QEMU 中验证：三路 PWM、系统联动、
无 `--apply` 不写入、确认要求、下限、错误 DMI/显式 model 拒绝、
独立原厂曲线、错误曲线拒绝、温度缺失转速保护。应用 BIOS 适配与型号测试通过。
此前 LED 缓存串行化、失败退避及型号显示修复保留。
原包与临时提取目录不变；未向实机写入、未打包 FPK、未推送或发布。
