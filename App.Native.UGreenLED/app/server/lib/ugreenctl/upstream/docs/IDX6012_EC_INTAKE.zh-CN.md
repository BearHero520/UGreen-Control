# iDX6012 非 Pro 风扇及来电唤醒：实机只读取证

2026-11-14。状态：原厂协议逆向 + 实机只读插件验证；尚未提供可写插件。
本记录不扩大其他机型的控制权限，不把共享主板标识当作唯一市场型号。

## 身份与输入

用户确认设备为 iDX6012。实际 DMI：厂商 `UGREEN`，产品 `Default string`，
product_version `iDX6011_iDX6012 V3.0 241112`，BIOS `5.32`（11/12/2024）。
系统为 fnOS Linux `6.18.18.c1186-trim`，x86_64。

使用已有解包输入，不修改镜像：

- `E:\工作台\固件\6800 pro.img`，UGOS Pro `1.17.0.0095`；SHA-256
  `7bb2746324ac852475727cf23f7016517996b91dfb293d2292d531c1e71581b0`。
  虽然文件名包含 6800，其通用内核包含独立的非 Pro iDX 驱动。
- `ug_idx6011-sio.ko`，SHA-256
  `181620c14131379f7f3894274270a024fcd327a3edfd184afa2a1232718bd870`；
  路径 `firmware-inbox/work/dxp6800pro-1.17.0.0095/kernel/usr/lib/modules/6.12.30+/kernel/drivers/ugreen/`。
- 通用镜像 `release_agent_cli_20260930-firmware_image-1.20.12.30-intel_release_amd64_6.18_nasync.img`；SHA-256
  `10edeeb661587c4cb495e078b1f3e004d6005c83e6aa7b1c05d764c57df5b50e`。
  已提取 `packages/29/fw/usr/sbin/ug-load-drive.sh` 第 23–41 行：
  DX601 系列中，Pro/iDX6021 用 `ug_idx6011pro-sio`，非 Pro 用 `ug_idx6011-sio`。
  因此先前只分析 IT8613 模块的路线不适用于本机。
- 实机 `DSDT.aml` SHA-256
  `04cdd7b4439a7b856cfcc6ee58c68151ed541d1a273c14a3476f9d6ce646be8d`。

## 原厂代码证据

EC 数据/命令端口为 `0x62/0x66`；读内存命令 `0x80`，写内存命令 `0x81`。
官方公开源码明确标注 ITE5571；尚未读出运行时芯片 ID/修订号，不能称为芯片身份已验证。
反汇编文件：`firmware-inbox/work/idx6012-hardware/ug_idx6011-sio.disasm.txt`。
已用 ELF 重定位核对下表中的函数调用目标确实是 `ec_read_memory`/`ec_write_memory`。

| 能力 | 原厂函数与地址 | 寄存器/行为 | 实机状态 |
| --- | --- | --- | --- |
| 风扇 1 转速 | `fan_read` 0xb02/0xb0c | `0x96/0x97` 高/低字节，直接组成 RPM | 1382–1383 RPM |
| 风扇 2 转速 | `fan_read` 0xb25/0xb39 | `0x98/0x99` 高/低字节 | 1369–1379 RPM |
| 两路统一调速 | `set_sys_fan` 0x3ec–0x41d | 写 `0x9c=1, 0x9d=PWM, 0x9e=1, 0x9f=PWM`；PWM 截断上限 198 | 四字节目前全为 0；未写入 |
| G3 唤醒读取 | `g3wakeup_read` 0x818 | 读 `0xa6`，仅接受 0/1 | 三次均为 1 |
| G3 唤醒设置 | `g3wakeup_write` 0xf73/0x1006 | on 写 `0xa6=1`，off 写 0 | 未写入、未断电验证 |

`0x9c/0x9e` 是模式选择，不能当成 PWM。当前四字节为 0 而转速非零，
所以零值不能解释为停转；当前有效自动 PWM 未知。
原厂函数对两路统一写相同 PWM，尚无物理风扇位置确认，不能标成 CPU/系统风扇。
`0xa6=1` 可表述为“原厂 G3 唤醒开关读值为开启”，不能据此宣称已验证真实掉电恢复。
尚无 `restore/恢复断电前状态` 模式的证据。

## 实机访问与保护

`scripts/probe_idx6012_ec.py` 只读取上表 9 个寄存器，三轮采样。
未发送 `0x81`，未修改风扇/来电唤醒设置，未加载或卸载驱动。
精确检查 UGREEN 厂商与 iDX6012 名称，或本机已确认的共享 OEM 板号组合；
拒绝其他 `Default string`。使用 `/run/ugreenctl-it55-ec.lock` 互斥，检查
锁文件所有者、类型、链接数；拒绝端口所有者及原厂驱动、拒绝未消耗 EC 输出，
使用有限等待；每轮重新确认身份与所有者。

本机 DSDT 的 `H_EC._STA` 无条件返回 0，sysfs status 为 0，无绑定 EC 驱动，
`/proc/ioports` 未占用 0x62/0x66。`ec_sys` 是内建
`CONFIG_ACPI_EC_DEBUGFS=y`，并非已加载模块；debugfs 已挂载且无 EC 实例。
探测器仅在这些条件都成立时接受该“内建但没有实例”的情形。
不能仅凭 lsmod 没有 ec_sys 就认为不存在驱动；不允许卸载所有者以获取访问。

实机结果保存于 `firmware-inbox/work/idx6012-live-20261114/ec-readonly.json`。
DSDT 反编译有外部方法解析警告；这里只使用独立且无外部依赖的 `_STA` 方法，
不将其他占位 ACPI 方法作为风扇控制证据。

## 接入边界与后续缺口

1. 在本仓库增加独立非 Pro EC 后端及精确身份匹配；不能复用 DXP6800 IT8613，
   也不能复用 DXP4800 GT 的 0xb0/0xb4 风扇或 0x88/0x89 启动命令。
2. 只读信息已经有实机证据。可写能力仍需控制器身份验证方法、模式含义和
   自动模式恢复证据；不能用“转速看起来合理”替代芯片身份检查。
3. 风扇写入需明确 `--force --apply`、所有者检查、锁、PWM 安全下限及上限 198、
   两路模式与 PWM 回读、失败恢复。不能在当前有效 PWM 未知时贸然降速。
4. 来电唤醒仅有 on/off 的原厂证据；需写后回读及独立的关机断电实测，
   后者会中断 NAS，不能通过当前在线只读检查完成。
5. 功能在上游验证后，LLLED_FPK 才接 `ugreenctl` 适配器和 UI。
   本轮没有改 LLLED_FPK、没有生成新 FPK、没有安装可写后端。

验证：实机只读探测通过；`python tests/test_idx6012_probe.py` 7 项测试通过。
独立目录 `build-idx6012-intake` 的 Linux x86_64 Release 交叉构建成功，
CTest 经 QEMU 运行 38/38 项通过（43.81 秒）；日志在该目录
`Testing/Temporary/LastTest.log`。配置使用现有 Zig Linux 工具链和
`tools/testing/run_linux.py`，未在 NAS 上执行回归测试。
以上 38 项为最初探测脚本阶段的验证；后续 C 插件验证见下文。

## 后续：官方源码与上游只读插件

找到官方 [ug_it55_functions.c](https://github.com/ugreen-opensource/kernel-6.12/blob/c75c5abd60dd45f45966720293ce1ee505fb8615/drivers/ugreen/ug_it55_functions.c)。
提交 `c75c5abd60dd45f45966720293ce1ee505fb8615`，Git blob
`0825dc2f802ba5dd85fe95ac392bbe2a5d8fbc31`；保存到
`firmware-inbox/work/idx6012-hardware/ug_it55_functions.c`，本地文本 SHA-256
`628a24a9c4f50db89bb3c1eb0e822dfb5d164c0d1978e615a8fff311e9d49292`。

新确认：

- 文件头明确用于 iDX6011/iDX6012，芯片 ITE5571。
- `functions_exit()` 明确把 `0x9c` 与 `0x9e` 写 0，注释为智能模式。
  因此当前两路模式 0 可报告 auto；有效 PWM 仍未知。
- **纠正 G3 与 startup 的混淆**：`/proc/it86/G3_wakeup` 操作 EC `0xa6`；
  `/proc/it86/startup` 操作 MMIO `0xfe001020` 位 0，0=power on、1=power off。
  原厂 `last` 分支明确没有实现。两者是独立入口，尚未验证其策略组合和实际掉电行为，
  不得用 `0xa6=1` 直接实现 `power startup get=on`。
- 源码没有运行时芯片 ID 检查；“源码标注芯片型号”不等于读取硅片身份。
  原厂 `fan off` 会写手动 PWM 0，不能当作恢复自动模式。

实现了 `ec/idx6012.c` 与 `models/idx6012.c`：

- 独立只读后端，没有 EC 内存写命令或设置回调；风扇转速采用高/低/高重读，
  避免字节翻转导致错误 RPM，三次不一致则报错。
- 全身份、锁、所有者、端口超时、短读和身份变化检查；错误时不返回部分风扇结果。
- 智能模式报告 `pwm=unknown`；仅手动模式且读值 <=198 才报告原始 PWM。
- G3 仅保留为后端诊断读取，未接入 startup API。
- `core/identity.c` 将精确 UGREEN + Default string + 完整板号映射为
  `iDX6011/iDX6012 OEM V3.0`，主 CLI 按该组合自动选插件；不放宽任意 Default string。
- `models/compatibility.json` 新增 read-only 条目，所有写能力仍 blocked。

已用独立临时目录测试新二进制及插件，未替换 NAS 上已安装程序：
`fan status` 自动识别并显示 sys1=1383、sys2=1378 RPM，mode=auto、pwm=unknown；
`info` 返回 startup=unknown。带 `--force --apply` 的 fan/startup 设置也被拒绝。
完整命令、退出码、二进制哈希与临时路径在
`firmware-inbox/work/idx6012-live-20261114/plugin-readonly.json`。
新增 `tests/test_idx6012.c` 覆盖错误厂商/机型/板号、被占用端口、锁失败、挂起输出、
超时、IO 错误、身份变化、转速字节翻转、未知模式/PWM 和非法 G3 值。
仍未改 LLLED_FPK、未打包、未修改 NAS 的硬件设置。

最终验证：Release 交叉构建成功；39/39 项 CTest 通过，43.05 秒。
新增兼容条目后同步上游包的插件白名单，单独重跑 package-runtime 与
package-policy，2/2 通过。现有工作区中的其他机型及曲线修改保留原状。


## 2026-11-15：用户授权的窄范围调速接入

用户明确允许，仅对这台 iDX6012，以精确厂商、主板版本、BIOS 版本及原厂寄存器证据
替代尚不可用的运行时芯片 ID。没有声称读取了芯片 ID，没有扩展至其他版本或电源控制。
写入还要求 BIOS 日期 11/12/2024；`--force --apply`、所有者检查和锁继续强制执行。

实现 `fan set sys 40..198`，两路同步，先写 PWM 再启用手动位；逐寄存器回读，
失败时恢复两路原始状态，恢复不确认则返回明确错误。`fan mode sys auto` 写模式位 0，
由 EC 接管。拒绝单独 CPU/单路通道及不带有效 PWM 的切换手动模式。
测试新增边界、错误 BIOS、所有者占用、写后回读失败及四个写入阶段的回滚场景。

实机测试记录 `firmware-inbox/work/idx6012-live-20261115/write-test.log`：
初始智能模式 1370/1360 RPM；PWM 150 回读成功，转速升至 1970/1949 RPM；
恢复智能模式后 6 秒降至 1378/1364 RPM。测试程序独立执行并有退出恢复，末尾 TEST_COMPLETE。
仅验证了 PWM 150 这一档，不声称所有 PWM 档位和温度工况均已验证。

LLLED_FPK 现通过受保护 ugreenctl（命令名 `ugreenctl`）接入固定 PWM 与恢复智能模式，
不接软件温控曲线、不开放来电恢复。应用使用上游 `model` 命令解析复合身份。

## 自定义自动温控接入（2026-11-15）
按用户要求复用 core/fan_daemon.c 现有 custom 软件温控，不复制相近机型寄存器或 stock 曲线。通过 identity 的精确 OEM 归一化接入 idx6012，仅路由 sys 成对输出；PWM 档位不得超过 198，failsafe 必须为 198，所有写入仍调用受保护 ugreenctl 插件。
原 39 项 CTest 与新增 idx6012-curve 测试通过。LLLED_FPK 已接入原有自定义自动入口。实机原应用 API 启动 custom 服务，目标及 applied_system_pwm 均为 150；测试后恢复设备智能模式。温度闭环长期运行尚未验证。

## 来电策略与 WOL 上游接入（2026-11-15）
原厂公开源码 get_power_status/save_power_status 的 MMIO 0xfe001020 位 0 为 0=on、1=off，last 分支无实现。新增 power/idx6012_power.c：精确 UGREEN OEM 板号与 BIOS 5.32/11-12-2024，核对 LPC PCI 8086:7e02，拒绝原厂驱动或已绑定 intel_pmc_core，要求 iomem 覆盖区域为 Reserved；独立 root 锁、仅位 0 RMW、回读与失败回滚。EC 0xa6 不用于来电策略。
实机只读值 0xd8fa3878 (on)。新 CLI 已实测 off→on 回读，保留其余位并恢复原 on 状态。尚未执行断电开机测试，不宣称 BIOS NVRAM 持久化。
实机两张 Aquantia AQC113 1d6a:04c0，atlantic 固件 1.5.42，enp87s0/enp88s0 均 Supports Wake-on pg、Wake-on g。独立 WOL profile 接入 canonical OEM 身份，自动映射恰好两张物理 PCI 网卡；排除 OVS/bridge 虚拟接口，复用上游 ioctl、锁与回读。off→on 实机驱动回读通过并恢复原 on；未执行关机唤醒测试，也未修改 EC LAN/G3 门控。
RTC 未新增模型授权；应用拆开 RTC 和 WOL 能力判断。应用隐藏不支持的 last 选项，接入上述上游 API。
测试：41 项 CTest 通过，新增身份/PWM外独立电源保护、位保留、失败回滚、WOL OEM 正反匹配。实机记录在 LLLED_FPK/.work-almighty-release/idx6012_hardware/power-wol-live.log。
