# iDX6012 实验性 LED 适配

日期：2026-11-06。分支：`codex/idx6012-led-support`。

本次只扩展随应用提供的 `server/bin/ugreen_leds_cli`，应用仍通过该 CLI 控灯。
补丁在 LLLED_FPK 内对固定提交的隔离源码副本应用；未修改
`E:\工作台\UGREEN-NAS-Hardware` 的源码，未添加 BIOS、风扇或来电自启能力。
原有硬件程序和插件在组合构建中逐字节保留。

## 证据和边界

用户确认诊断设备为 iDX6012，2026-11-05 的诊断记录为：

```text
sys_vendor=UGREEN
product_name=Default string
product_version=iDX6011_iDX6012 V3.0 241112
bios_version=5.32
bios_date=11/12/2024
configured_profile=idx6011
```

原 CLI 在精确机型匹配处返回 unsupported，还未验证这台机器的 LED 通信。
本次增加两个实验档案：厂商 UGREEN + 精确名称 `iDX6012`；或厂商 UGREEN +
`Default string` + 精确版本 `iDX6011_iDX6012 V3.0 241112`。
不会把任意 Default string、其他版本或其他厂商映射到此档案。
共享版本字符串本身不能普遍区分 iDX6011/iDX6012；这里的型号标签基于用户对该样本的确认。

协议参考（读取于 2026-11-06）：

- [klein0r CLI 实现](https://github.com/klein0r/ugreen_leds_controller/blob/master/cli/ugreen_leds.cpp)：
  iDX6011/iDX6012 路由到 iDX SMBus block，LED ID 为电源 0、网口 1/2、硬盘 3..8。
- [协议分析](https://github.com/klein0r/ugreen_leds_controller/blob/master/docs/iDX6011Pro-LED-Protocol.md)：
  I801、地址 0x3a、11 字节 block payload、大端校验、状态地址 0x81 + ID。
  文档的逆向样机是 Pro，不能单独视为这台非 Pro 主板的实测证据。
- [非 Pro 项目](https://github.com/omgapuppy/unraid-ugreen-idx6011-led-fan/blob/main/README.md)：
  声明支持 iDX6011/iDX6012，九灯布局；风扇写入未开放。

本地官方包：
`E:\工作台\UGREEN-NAS-Hardware\firmware-inbox\input\release_agent_cli_20260930-firmware_image-1.20.12.30-intel_release_amd64_6.18_nasync.img`

- SHA-256：`10edeeb661587c4cb495e078b1f3e004d6005c83e6aa7b1c05d764c57df5b50e`
- MD5：`780940b4a35a42288216b5b727afd71e`，与本地官方目录记录一致。
- 解包位置：`firmware-inbox/work/almighty-all-models/packages/29/`。
- `fw/usr/sbin/hwmonitor-idx` 包含 iDX6012 字符串，SHA-256：
  `210c0123f23629450ec0f6cccdf71a0acab829a6efdd2710f100e7de25a1cce7`。
- 这些是型号和分析线索，不是完整镜像适合刷入 iDX6012 的证明。

## 实现

LED 补丁位于
`App.Native.UGreenLED/app/server/vendor/ugreen_leds_controller/patches/idx6012-block.patch`，
基线提交 `aff7991ee58709ad34b5afbc2787f0d21300368d`，同时保留原 EM26 LED 补丁。
复用现有 `smbus-block-be`，没有复制参考项目中的环境变量强制选型。
v3.1.1 增加根据官方模块验证的显式初始化操作，由已启用的应用守护进程调用。

现有保护保持生效：I801 按名称发现、0x3a 设备、写前 MCU ID 0xc5b2、所有者检查、
锁、ACK、状态校验、写后回读，以及 `--force --apply`。`--force` 不绕过身份或锁。
状态查询不写控制数据，也不会触发灯光初始化。自动控制关闭时不会初始化；
同一守护进程内，对同一配置档案初始化成功后不再重复。初始化失败时不继续普通控灯。

应用增加 iDX6012 选项及一个网络灯。自动识别只对上述精确共享主板样本回退。
旧设置中显式选择的 idx6011 不会被自动覆盖；在设备页面改为“自动识别”或
“UGREEN iDX6012（实验性）”后保存。
自动盘位复用现有六盘机型的 HCTL 映射：0/1 对应盘槽 5/6，2..5 对应盘槽 1..4。
手动位置绑定和磁盘序列号绑定保持原有规则，不会被此修正覆盖。

### 2026-11-08：硬盘到盘槽的映射修正

用户补充两块 SATA SSD 实际位于盘槽 1、2，智能模式却点亮灯 3、4。
原 `sequential` 用 HCTL 主机号加一，导致主机号 2、3 被错误地解释为盘槽 3、4。
灯号到前面板位置已逐灯确认，但这不能验证硬盘到物理盘槽的关系；两者是不同映射。
本次将 iDX6012 的 `hardware_hctl_profile` 改为复用 `dxp6800` 六盘路由，
覆盖全部六槽并用用户的双 SSD 布局验证。暂未改动 MCU 灯号或原有灯光协议。
该修正纳入 v3.1.2 本地安装包。双 SSD、全六槽映射及其他机型隔离的模拟回归通过；
安装 v3.1.2 后的真实磁盘活动与灯位对应关系仍待设备验证。

参考项目的
[非 Pro 盘位修正提交](https://github.com/omgapuppy/unraid-ugreen-idx6011-led-fan/commit/c4562e6af9637294c43332dcee48e21fc9dd3ccc)
明确记录 ASMedia 控制器连接盘槽 1..4、Intel PCH 连接盘槽 5..6。
它的 `src/monitor.sh` 动态发现控制器和 ATA 端口，并支持 `calibrate.sh` 校准；
README 中仍有双网络灯的旧描述，应以源码和本机八灯实测为准。
其控制器/ATA 发现可作为未来应对 HCTL 重排的改进依据；当前修复复用已有六盘映射，
尚未把社区项目的动态发现逻辑或直接 I2C 控灯实现导入本应用。
设备若更改存储拓扑导致 HCTL 重新编号，需重新核对或使用现有位置绑定功能。

## 构建及离线验证

在 LLLED_FPK 根目录依次运行：

```text
python scripts/build_led_compat.py --idx6012
python tests/run_idx6012_runtime.py
python scripts/prepare_idx6012.py
python scripts/build_ugreenctl.py
```

这些步骤构建/导入硬件输入，不生成 FPK，不提交、不推送、不发布。
临时输出在 `.work-almighty-release/idx6012/`，包括构建回执、模拟测试和 QEMU 日志。
最终 FPK 仍须另行运行本地打包入口，并输出到 `dist/`。

- C 模拟传输：八灯 RGB、亮度、开关、闪烁、呼吸、BE 命令、回读不一致、
  越界灯号、厂商及版本拒绝、芯片 ID、权限、所有者、锁与身份改变。
- 运行原有 LED/N76 回归和 DXP2800 EM26 测试。
- QEMU 运行生产 CLI：准确样本及 iDX6012 通过选型，随后因 VM 无 I2C 返回 rc=5；
  错误版本/名称仍返回 rc=3；EM26 路由保留。rc=5 在此测试中只说明识别通过，
  不代表真实通信成功。
- `tests/test_idx6012_profile.sh` 检查应用自动识别、单网络灯布局和显式设置保留。

## 实机第一步

安装由此分支另行打包的应用后，先读取状态（root）：

```sh
/vol1/@appcenter/App.Native.UGreenLED/server/bin/ugreen_leds_cli all -status
```

预期应列出 power、netdev、disk1..disk6；保存输出及新诊断包。
确认读状态成功后，再从应用单灯测试开始确认实际位置、颜色和回读。
若出现 ACK/校验/回读错误，保留原始输出，不通过降低身份验证或直接 I2C 写入解决。

实机已确认 MCU ID 0xc5b2、SMBus 状态读取，以及 ID 2 亮度写入 96 后即时回读成功；
但 15 秒后回到 0，用户确认仍在跑马灯状态。
增加官方初始化后，两轮分别取得 10 次和 20 次稳定回读，用户确认停止跑动。
随后硬盘 1 变绿保持 20 秒；电源、网络和硬盘 2..6 按顺序变绿，用户逐灯确认全部一致。
每轮都恢复原灯设置、原安装二进制和自动控制服务；恢复状态回读通过。
恢复服务后全亮与当时应用的 `mode=on`、RGB(180,180,180)、亮度 128 设置一致。
实机测试版本 CLI SHA-256：`964156fffb56ce68ee691362e88abecf082cf865e62fa081558829d3346426b0`。
v3.1.1 在该序列后增加 CLI 内部恢复电源灯和应用启动调用，已做离线回归；
最终安装包尚未在设备上升级验证，也未测试断电重启、闪烁/呼吸和真实磁盘活动映射。
BIOS/风扇/来电自启依旧不受本次 LED 补丁支持。

## 官方模块修正与显式初始化测试

官方 `kernel/drivers/ugreen/leds-mcu.ko` 的 SHA-256 为
`ab3507ada3654c14454971dab5fcd85fd2f89e7f49ce21559ceceea6d2ac2efe`。
`leds_ugreen_probe` 在 0x167d 匹配 iDX6012 后跳到 0x1933，选择
`iDX6011_led_name` 的八项表：power、network_stat、disk1..disk6，即 ID 0..7。
Pro 才使用九项表；上述社区项目的九灯假设不适用于用户这台非 Pro 设备，已修正。

官方 probe 0x16e9..0x17d6 向 ID 0 依次发送命令/参数：
`4:[0,0,0,0]`、`1:[255,0,0,0]`、`2:[255,255,255,0]`、
`3:[255,0,0,0]`、`1:[255,0,0,0]`。测试入口：
`ugreen_leds_cli power -initialize-idx6012 --force --apply --json`。
入口严格限制 iDX6012 档案、单独执行，保持身份、锁、所有者、芯片及 ACK 检查；
所有灯预读成功后才写入，验证电源灯为常亮白色、亮度 255 后恢复原电源灯设置。
省略 `--apply` 只预览，不访问控制器。普通状态查询和单次控灯命令不会自动初始化。
应用守护进程在启用自动控制后通过这个专用 CLI 入口初始化，再应用用户设置。
该命令可能停止自主动画，无法承诺恢复此前动画；不是 MCU 固件刷新操作。
