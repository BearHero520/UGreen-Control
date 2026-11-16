# DXP4800 GT 实验性风扇与来电启动接入

## 来源与边界

- 官方输入：`E:\工作台\固件\DXP4800 GT.img`，UGOS Pro 1.21.1.0016，kernel 6.18.15。
- SHA-256：`ca2e91fa9e9d23a738b4689b8bb39f199cad60786253add81435559a48c27ff0`。
- 上游证据：`E:\工作台\UGREEN-NAS-Hardware\docs\DXP4800GT_HARDWARE_AUDIT.zh-CN.md`。
- 提取目录：`E:\工作台\UGREEN-NAS-Hardware\firmware-inbox\work\dxp4800gt-ca2e91fa`。
- 硬件层：上游 `ec/it55_dxp4800gt.*`，插件 `models/dxp4800gt.c`。
  本应用仅调用随包 `ugreenctl`，没有新增端口、I2C 或 sysfs 硬件写入。
- 2026-11-14 用户明确授权 GT 按官方方式实验性接入：精确 UGREEN/DXP4800 GT DMI＋稳定 EC 版本。
  该例外仅适用于 GT；数值芯片 ID 仍未知，EC 版本不能冒充芯片 ID。

## 行为

原厂 `ug_4800gt-sio` 是内建驱动，标识 IT55xx EC，不是 IT8613。
CPU 模式/PWM 为 EC 0xb0/0xb1，系统为 0xb4/0xb5；原厂上限 198，应用下限 40。
两通道独立设置，写后回读模式和 PWM。转速按原厂 0x34/35、0x38/39 组合，尚无实机标定。
端口协议、精确身份、所有者拒绝、独占锁、超时均在上游实现，`--force` 不绕过保护。
存在原厂驱动或 ACPI EC 端口所有者时拒绝访问，不自动卸载驱动。

来电启动 on/off 分别使用原厂 EC 命令 0x88/0x89，验证原厂命令状态 bit 3。
这只能表示命令已发送，不能证明持久化策略；状态查询保持 unknown。
官方 `last` 没有硬件动作，因此应用不提供“恢复断电前状态”。独立 G3 寄存器未被猜测为策略回读。
应用可以保存用户选择，保存值仅表示意图，不作为硬件状态。

v3.1.3 已接入固定 PWM 和 `stock-4800gt` 官方八点标准自动温控；五点自定义曲线仍禁用。
API 返回 `max_pwm=198`、`startup_readback=false`、`startup_restore_supported=false`。
芯片 ID 保持未知，UI 正确显示 IT55xx EC。

## 导入与验证

上游 Release 构建及 CTest、应用适配/API 测试完成后，运行：

```text
python scripts/prepare_dxp4800gt_hardware.py
python scripts/build_ugreenctl.py
python tests/run_dxp4800gt_hardware_runtime.py
```

硬件导入更新 `ugreenctl`、`ugreenctl-fand`、`dxp4800gt.so` 和来源/校验记录；导入脚本本身不构建 FPK、不推送或发布。
LED 保持此前多总线修复版本，SHA-256：
`39f74bb27e21e1d14a5f41d7ad2a640684a2aaaed0c9406176bf6cf36104a5b2`。

上游 mock 测试验证正常事务、范围、身份、锁、冲突、超时、PWM 回读失败；
QEMU 验证实际随包 CLI/plugin 的路由、dry-run、错误机型和无 EC 时拒绝。
这些不是实机验证。尚需在目标 fnOS 机器核对两路转速/PWM，并完成真实断电再供电试验。
不能仅凭官方固件来源或模拟测试声称实机修复已验证。


本次结果：上游 Release 构建成功，CTest 38/38 通过；实际随包 CLI/plugin QEMU 测试通过；
GT 适配器与 GT API 专项测试通过；启动策略持久化回归通过；Python 导入校验 8/8 通过；
项目打包输入验证通过；TypeScript 检查与 Vite 构建通过（保留既有大 chunk 提示）。
未生成 FPK、未推送、未发布，也未连接或写入实机。

GT 专项可重复命令（Bash）：

```sh
bash tests/test_dxp4800gt_profile.sh
TEST_MODEL_ONLY=dxp4800gt bash tests/test_bios_api.sh
TEST_PRODUCT='DXP4800 GT' bash tests/test_startup_persistence.sh
```

GT 启动设置测试包括 on/off 保存、重放、服务 start/stop 和 last 拒绝。
完整 API 长运行进程使用旧模拟程序，在新加 GT unknown 断言处失败；
此前其他机型断言已执行通过。GT 已使用更新后的 unknown 模拟程序单独验证，
不把旧进程标记为全量通过。验证收据保留在 `.work-almighty-release/dxp4800gt_hardware/verification.json`。


## v3.1.3 八点自动温控与实机测试包

官方八点配置与插值证据见上游 `docs/FAN_CURVE.md` 的 GT 章节。
CPU 独立使用 cpufan_curve_cpu；系统取 HDD/NVMe 较高需求。原厂 has_cpu_fan=1
分支不启用备用 sysfan_curve_cpu。插值后再限制40..198，保留当前软件守护的升速/降速保护。
这是标准曲线模式，不包含原厂静音倍率、自定义模式或高温自动关机服务。

本轮验证：Release 串行构建成功（Windows Zig 并发链接出现 Unexpected，串行重试通过）；
CTest 38/38；GT CLI/plugin/daemon QEMU 黄金目标与故障测试；GT API启停、参数来源与范围；
GT 配置保存/恢复；8项导入测试；TypeScript与Vite构建全部通过。
实际硬件仍未连接。LED 二进制 SHA-256 保持上文值。

实机安装 `dist/App.Native.UGreenLED-3.1.3.fpk` 后，在 BIOS/风扇页面确认实验性提示，
选择“原厂兼容自动”，应显示 GT 八点表和运行状态。观察 CPU 与系统两路 PWM、RPM
及 CPU/HDD/NVMe 温度，确认 PWM 始终不超过198；重启应用后核对自动模式恢复。
若显示控制器占用或写入失败，保留诊断日志，不强行卸载驱动或绕过保护。
来电自动开机仍需单独实机验证，界面不会把命令状态当成持久化策略回读。


## 2026-11-15 实机反馈修订（当前 manifest 3.1.4）

诊断输入：ugreen-led-diagnostics-20261115-085010.txt。原日志只有 LED discovery EIO
和 EC EBUSY，不能据此断言具体总线、驱动或寄存器故障已修复。
本次 LED CLI 在错误中报告 last candidate /dev/i2c-N；上游 GT EC 插件区分
DMI、owner check、lock/permission、version read 与 transaction 阶段。
version read 的 EBUSY 表示0x66已有未读输出，不主动丢弃输出。
只读诊断收集补入0x62/0x66资源及EC名称；没有卸载驱动、强制占用总线或绕过身份校验。

应用枚举改用全类型磁盘列表以显示NVMe/U.2，未知物理灯位显示未映射。
不能凭NVMe协议区分U.2与M.2，也不从控制器编号猜测LED盘位。
确认保存修复了GT机型白名单遗漏。取色器改为应用内预设色和HEX弹窗，
非法输入不可提交，取消不改值；页面保存后才提交配置。

验证：上游Release构建、42项CTest；LED选择/协议/所有者保护测试；
GT CLI/plugin/daemon模拟运行测试；NVMe枚举与API回归；前端类型检查及构建；
浏览器模拟后端验证非法HEX拦截、选色、保存刷新保留。尚未连接真实GT验证。
包通过本地入口生成到dist，不推送、不发布。安装后复现LED/BIOS操作并导出新版诊断，
以具体候选总线和EC失败阶段继续定位。当前包不能承诺已解决实机LED/EC故障。

## 2026-11-16 实机反馈：物理槽位和控制器发现

v3.1.5 能枚举 NVMe，但 HCTL 只能给 SATA 盘分配灯位。用实机日志里的 PCI 根路径对照
上游基于官方固件的 `disk/firmware_profiles.c`：`nvme0n1` 经 `00:01.1` 连接前置
1 号 U.2 槽，使用 `disk1`；`nvme2n1` 经 `00:01.6`、`nvme1n1` 经 `00:01.4`，
对应两个没有独立盘灯的 M.2 槽。设备名可能重排，应用仅消费 `ugreenctl disk map --tsv`
读取的物理映射。扫描失败或有槽位争用时不给未确认的设备分配灯位。
NVMe 活动以块设备吞吐为准，不使用 ATA `hdparm` 休眠检测。

LED 发现修正和证据见 `DXP4800GT_LED_SUPPORT.md`。BIOS 忙发生在进入 EC 事务之前，
`ec_sys` 调试组件在无活动 EC 的 QEMU 中也会存在，已修正仅凭模块存在触发的误报。
新版 `ugreenctl` 插件核对实际 ACPI 和端口资源并返回具体拦截项；实机复测仍需确认
是否有真正的占用以及独立 EC 身份，来电策略断电保持情况也尚待验证。
