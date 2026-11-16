# DXP4800 GT BIOS、风扇与来电恢复审计

日期：2026-11-14。范围：核实当前上游支持与用户提供的官方固件；不启用未经验证的控制接口。

## 输入

- `E:\工作台\固件\DXP4800 GT.img`，1,450,427,392 bytes。
- SHA-256：`ca2e91fa9e9d23a738b4689b8bb39f199cad60786253add81435559a48c27ff0`。
- 外层 tar，内含 fw/kernel SquashFS、Zstandard initrd、vmlinuz。
- UGOS Pro `1.21.1.0016`，BUILD_TIME=`20261103.082916`，内核 `6.18.15`。
- vmlinuz SHA-256：`1fae55ecea9ecec4c6deaaee03313d6a0c6caccff95dad78b5622370abd107af`。
- hwmonitor-amd SHA-256：`ae6ea1a37b5476618b9f3f5f5ffc32e7912dc22cfaa020f560dad7d560dd4c35`。
- 提取目录：`firmware-inbox/work/dxp4800gt-ca2e91fa/`。镜像未修改；临时提取物不提交。

## 现有支持结论

审计上游 HEAD `abfe71a07e5b10e79d78bc637dfec14eba514de0`：

- `models/` 和 CMake 插件列表没有 DXP4800 GT 的风扇／来电恢复插件，现场 `no plugin matches DMI product 'DXP4800 GT'` 与此一致。
- `models/wake_profiles.c` 有精确 GT 的 RTC 和 WOL 档案；WOL 不自动猜网卡，需明确物理接口。它们不等于 BIOS／风扇／AC-recovery 支持。
- 不应复用 DXP4800、DXP4800 Plus 或 DXP4800 Pro 的 IT8613 硬件映射。
- 本次只更新审计文档，没有新增硬件写入实现，没有修改 LED 控制代码。

## 原厂控制链路

`fw/usr/sbin/ug-load-drive.sh` 中的精确 `DXP4800 GT` 分支请求 `ug_4800gt-sio`；
`fw/etc/startpre.d/hwmonitor.sh` 为该机型选择 `hwmonitor-amd`。

该驱动是 **built-in**：`kernel/usr/lib/modules/6.18.15/modules.builtin` 包含
`kernel/drivers/ugreen/ug_4800gt-sio.ko`；`modules.builtin.modinfo` 有
`ug_4800gt_sio.file=drivers/ugreen/ug_4800gt-sio`。没有独立 ko 不代表缺少实现。

用本地 vmlinux-to-elf 解压 vmlinuz 并恢复 kallsyms 后，从
`__initcall__kmod_ug_4800gt_sio__543_666_functions_init6` 的相对指针定位
`functions_init` (`0xffffffff835f7780`)。它精确比较 DMI `DXP4800 GT`，
日志标识 `gt it55` / `Chip IT55xx`，注册 `/proc/it86/{fan,startup,temp,version,Lan_wakeup,G3_wakeup}`。
这个 proc 目录名不能证明控制器是 IT8613。

## 已恢复的静态证据

以下地址只适用于本次固件。函数名在内核中有重名，必须通过上述 initcall 和 proc_ops 指针确认所属驱动。

| 能力 | 固件证据 | 结论 |
| --- | --- | --- |
| 系统风扇 | `set_sys_fan` `0xffffffff81cd0880` | `ec_write_memory(0xb4,1)` 然后写 `0xb5`；原厂值上限 198 |
| CPU 风扇 | `set_cpu_fan` `0xffffffff81cd0920` | `ec_write_memory(0xb0,1)` 然后写 `0xb1`；原厂值上限 198 |
| 风扇读数 | `fan_read.part.0` `0xffffffff81cd1220` | 读取 EC `0x34/0x35`、`0x38/0x39` 并组合；尚未实机核对物理通道／单位 |
| EC 通道 | `ec_write_memory` `0xffffffff81ccc9d0` | 经 EC 命令端口 `0x66`、命令 `0x81`；不是 IT8613 配置端口序列 |
| 来电启动 on | `set_power_status` `0xffffffff81cd1740` | on 分支向 EC 发命令 `0x88`，检查状态端口 bit 3 |
| 来电启动 off | 同上 | off 分支向 EC 发命令 `0x89`，检查状态端口 bit 3 |
| restore/last | 同上 `0xffffffff81cd1809..1825` | 接受 `last` 后直接返回，没有对应硬件操作；不能声称支持恢复上次状态 |
| 来电状态查询 | `get_power_status` `0xffffffff81cd06c0` | 返回 `g_power_status` 软件变量，不是持久化硬件策略回读 |
| 独立 G3 开关 | `g3wakeup_read.part.0` `0xffffffff81cd0e10` / `g3wakeup_write` `0xffffffff81cd13c0` | EC `0x9a` 可读写 0/1；尚无证据证明它与 startup 的 `0x88/0x89` 命令为同一持久化策略 |
| EC 固件版本 | `version_read.part.0` `0xffffffff81cd10b0` | EC `0x00/0x01/0x03` 格式化为 `%u.%u%u`；不是已确认芯片 ID，缺少该实机基准值 |

`hwmonitor-amd` 使用 `echo cpu N` / `echo set N` 写 `/proc/it86/fan`，
使用 `echo on` / `echo off` 写 `/proc/it86/startup`。
`fw/etc/default/dxp4800gt.conf` 配置 CPU/HDD/NVMe 温度曲线及 CPU、系统两风扇。
因此原厂曲线来自用户态软件守护，不能标称已接入硬件自动曲线。

完整反汇编在提取目录的 `gt-init.txt`、`gt-fan-set.txt`、`gt-fan-read.txt`、
`gt-power-set.txt`、`gt-power-read.txt`、`ec-write.txt`；同目录保留只读分析脚本。

## 尚未满足的实现条件

- 精确 IT55xx 芯片型号和可可靠读取的芯片／固件身份，跨 BIOS 修订的映射稳定性。
- 在目标第三方 Linux 上可与 ACPI EC、原厂 built-in 驱动、其他访问者安全互斥的传输路径。
- 风扇模式、当前 PWM 的真实回读、安全范围、转速单位和物理通道的验证。
- 来电策略的真实持久化回读及断电验证；原厂软件缓存不能充当回读证据。
- G3 `0x9a` 是进一步验证的候选，不能仅凭名称将它宣称为 startup 的回读寄存器；需要控制变量的实机验证。
- 原厂 `last` 是无操作，不能映射为上游 restore。

据此保持 GT 风扇／来电恢复未开放。后续实现必须位于本仓库，具备精确 DMI、控制器身份、所有者检查、锁、超时、写后真实回读和测试；应用仓库只负责适配。

## 验证与变更边界

本次是静态固件审计，没有连接或写入实机。LLLED_FPK 同期仅修复其随包 LED CLI 的多总线发现；其 BIOS／风扇程序和插件保持原字节内容。
固件观察、源码支持状况已核对；未把静态证据升级为实机支持。

Windows 主机通过仓库 Zig Linux 工具链完成 Release 全量构建，使用 QEMU CTest emulator 运行测试：37/37 通过。
测试目录：`firmware-inbox/work/dxp4800gt-ca2e91fa/upstream-build/Testing/Temporary/LastTest.log`。
这验证现有上游构建和回归，不代表新增 GT 控制能力。构建有一个既有测试文件 `test_it8613_hwmon.c:32` 的未初始化变量警告，未修改无关代码。


## 后续实验性实现（2026-11-14）

用户随后要求按此官方固件实现并接入应用。新增 `ec/it55_dxp4800gt.*` 硬件层、
`models/dxp4800gt.c` ABI v5 插件和 `tests/test_it55_dxp4800gt.c`。
前述“未新增实现／应用保持原字节”只描述初次审计，非本阶段状态。

- 精确匹配 UGREEN / DXP4800 GT；EC 端口 0x62/0x66，读命令 0x80、写命令 0x81。
- CPU 模式/PWM 0xb0/0xb1、系统 0xb4/0xb5；手动范围 40..198，写后逐项回读。
- 转速按原厂高低字节组合；未验证真实 RPM 标定。
- 来电 on/off 使用命令 0x88/0x89，仅校验原厂状态 bit 3；查询永远 unknown。
  不把该 bit、软件缓存或独立 G3 0x9a 当成持久化策略回读，不实现 restore。
- 拒绝原厂 proc/module、ec_sys 和占用 EC 端口的内核资源；不卸载驱动。
  使用根用户独占锁和有界轮询；身份、冲突、超时或回读异常即失败。
- EC 0/1/3 固件版本需非全零、非 0xff，并在写前保持稳定；这不是芯片 ID。
  **用户于 2026-11-14 明确确认“允许 GT 按官方方式实验性接入”。仅 GT 允许精确 DMI＋稳定 EC 版本替代数值芯片 ID；其他机型原有身份规则不变。**
- 官方八点自动曲线尚未移植；应用暂只提供固定 PWM，禁用自动模式。
- 应用只改适配器、API 和 UI；未添加应用硬件访问，LED 二进制保持独立。

测试：Release 构建及 CTest 38/38 通过；新增测试模拟成功事务、双通道隔离、
范围、锁/所有者/身份拒绝、超时和 PWM 回读失败。QEMU 候选 CLI/plugin 测试通过，
验证精确机型路由、dry-run、无 force 拒绝及不存在/占用 EC 时失败。
上述均非实机验证；来电自动开机需要目标机器完成断电再供电验证。


## 2026-11-16：fnOS 所有者排查

`ugreen-led-diagnostics-20261116-092623.txt` 来自 BIOS `DXP4800 GT V1.0 20260429`、
fnOS kernel `6.18.18.c1186-trim`。`info` 和 `power startup get` 均在 `controller owner check`
返回 `EBUSY`，尚未到独占锁、端口权限或版本读取。日志确认 `/proc/it86` 不存在，
但没有记录实际冲突项，因此不能据此断言是 ACPI EC 驱动。
QEMU 启动内置 `ec_sys` 但无活动 EC 时也复现相同误报；[Linux 6.18 `ec_sys`](https://github.com/torvalds/linux/blob/v6.18/drivers/acpi/ec_sys.c#L123-L127)
是可在 `first_ec` 为空时存在的调试组件，官方镜像也将其编译进内核。上游改为拒绝真实
debugfs EC 节点、`/proc/ioports` 冲突、ACPI `PNP0C09`/`LNXEC` 设备或 `ECDT` 引导表，
并保留原厂驱动及 proc 检查。资源或 ACPI 视图不可检查时拒绝进入端口事务。错误返回具体拦截项。
当状态端口全为 `0xff` 时报告设备不可用，避免误报有未读响应。
同一台 fnOS 实机的补充输出显示 `/sys/module/ec_sys` 存在，`/proc/it86` 和原厂 SIO 模块
路径没有列出。附近只列出 `0060`、`0061` 和 `0064` 三个独立资源，内核日志筛选没有返回
`ACPI: EC:`。旧版会在检查 `ec_sys` 节点时提前返回 `EBUSY`。这三条端口资源记录已用于
回归测试。真实机器上的 EC 版本读取、写入和回读仍需要实测。
若确认真实所有者占用，需要和它协调访问；不能通过 `--force` 或忽略资源区间来解锁控制。

## DXP4800 GT 八点自动温控（v3.1.3 应用接入）

固件：DXP4800 GT.img / UGOS 1.21.1.0016，SHA-256
`ca2e91fa9e9d23a738b4689b8bb39f199cad60786253add81435559a48c27ff0`。
原始配置 `fw/etc/default/dxp4800gt.conf`，profile `stock-4800gt`。

| 原厂配置键 | 八个温度°C,PWM点 | GT 实际用途 |
| --- | --- | --- |
| cpufan_curve_cpu | 0,20 / 70,37 / 75,53 / 80,70 / 85,98 / 90,126 / 95,159 / 100,187 | CPU 独立输出 |
| sysfan_curve_ata | 0,68 / 40,80 / 45,131 / 50,144 / 55,169 / 58,194 / 60,207 / 65,207 | 系统输出来源 |
| sysfan_curve_ssd | 0,68 / 50,80 / 55,131 / 60,144 / 65,169 / 70,194 / 75,207 / 80,207 | 系统输出来源 |
| sysfan_curve_cpu | 0,68 / 70,80 / 75,131 / 80,144 / 85,169 / 90,194 / 95,207 / 100,207 | 配置保留；has_cpu_fan=1 时不启用 |

`hwmonitor-amd` SHA-256 `ae6ea1a37b5476618b9f3f5f5ffc32e7912dc22cfaa020f560dad7d560dd4c35`：
`temp_to_pwm` 0xca67 使用相邻点线性插值，正差值加 0.5 后截断取整。
`fan_set_limit` 0xdbae 的 0xdc15..0xdc52 通过 `hwInfo.has_cpu_fan` 分支，
GT=1 时独立计算 CPU、跳过 CPU→系统曲线；0xdc65..0xdc8e 对 HDD/NVMe 结果取较大值。
DWARF 确认 `hwInfo.has_cpu_fan` 偏移 92。这里只接入标准未缩放曲线，
不复制原厂静音/全速/自定义模式或高温关机服务。

原始点保留不变，插值完成后才施加本项目安全下限 40 和 GT 驱动上限 198；
不得先将端点 207 截为 198（会改变中间温度输出）。CPU 低温值20/37在本项目升到40。
CPU 取最高有效温度；系统取 HDD/NVMe 最大需求，闲置基线68。
CPU 温度失效或启用存储温度保护且无存储温度时，两路均输出198。
复用现有立即升速、延迟降速、独占守护锁和受保护 `ugreenctl --force --apply` 写入。
GT 不接受其他机型 stock 或255范围的 generic custom profile。

代码：`fan/thermal_curve.c`、`core/fan_daemon.c`；模型/EC硬件访问继续保持独立。
应用展示数据来自 `models/dxp4800gt-fan-curve.json`，仅用于展示，不在应用计算硬件目标。
证据提取目录仍为 `firmware-inbox/work/dxp4800gt-ca2e91fa/`，新增
`gt-eight-point-disassembly.txt`、`gt-eight-point-aggregation.txt`、`gt-fan-set-limit.txt`。
新增黄金值测试覆盖各拐点、舍入、限幅顺序、传感器缺失与CPU峰值。
这是静态恢复＋软件模拟验证；尚无实机风扇/温度验证。
