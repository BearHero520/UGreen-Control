# DXP2800 EM26 灯光与风扇接入

2026-11-01，目标仅为精确 DMI `DXP2800` / `EM_DXP2800_V1.0.26`。
现场输入为 `ugreen-led-diagnostics-20261031-150123.txt`：灯控缺少修订档案，
风扇插件为占位；服务以 root 运行，it87 未加载，无风扇 hwmon 节点。

## 固件依据和实现边界

只读复用 `E:/工作台/固件/6800plus.img`（UGOS Pro 1.20.0.0142 通用镜像），
重新计算 SHA-256：
`73862c9c619f3e8b2279252326654f4915234dc8f437a71eb9a0067a724062c5`。
提取目录为硬件源项目的 `firmware-inbox/work/dxp6800plus/`。

原厂 `fw/usr/sbin/ug-load-drive.sh:57..70` 对 EM_DXP2800 选用
`leds-mcu-28a48`，并加载 `ug_it86x-sio`。这是 `.26` 采用 legacy LED
协议的依据；没有用 MCU 芯片家族或近似型号推断任意修订的写入协议。

LED 只修改应用内的 `patches/dxp2800-em26.patch`，给独立 CLI 源码快照增加
精确 EM26 档案，保留既有 EM24/V1.2/其他机型档案与全部保护。快照固定在
`aff7991ee58709ad34b5afbc2787f0d21300368d`，在应用忽略目录中构建，
不改动 `UGREEN-NAS-Hardware` 中的任何 LED 源码。应用所有操作仍调用
`server/bin/ugreen_leds_cli`，没有增加应用侧 I2C/sysfs/端口写入。

风扇代码在 `E:/工作台/UGREEN-NAS-Hardware` 中：独立 DXP2800 插件与映射，
寄存器、固件 SHA、反汇编位置见该项目 `docs/DXP2800_EM26_SUPPORT.md`。
`ug_it86x-sio.ko` SHA-256：
`83c95d12f988f7ce8fd2f9e673a84beb71f4e091335f32c79b00ac28d178ac4a`。
IT8613 系统风扇为控制 `0x17` / PWM `0x73` / 转速 `0x0f,0x1a`，
hwmon 对应 pwm3 / fan3_input。优先 hwmon；仅节点不存在时直控回退，
保留芯片/HWM 识别、所有者互斥、共享锁、PWM 40..255 和写后回读。
不会加载或卸载 it87，也不会绕过 EIO/EACCES/EBUSY 错误。

应用接入单系统风扇、实时转速/PWM、固定调速、自定义温控、风险确认。
缺失传感器时温控守护采用 PWM255。增加来电恢复 on/off/restore，使用原厂同模块的 LDN4 0xf2 bit5 /
0xf4 bits5/6 映射，保留身份检查、互斥和完整寄存器回读。WOL 由管理员
在界面中选择上游列出的物理有线网卡，校验 Magic Packet 能力、网卡身份
和写后回读；RTC 复用上游独立的 rtc_cmos/UTC/时间窗口/锁/回读保护。
没有虚构 DXP2800 原厂曲线；其他 BIOS 修订、GT/JW/V1.2 不继承 EM26
风扇和来电恢复映射。实际断电恢复、关机唤醒仍待目标 NAS 验证。

## 复现

1. 在硬件源项目用现有 Zig Linux CMake toolchain 配置 `build-dxp2800`，
   Release 构建后运行全部 CTest。Windows 使用现有 QEMU cross-compiling emulator。
2. 应用仓库执行 `python scripts/build_led_compat.py`：固定源码、核对并应用
   LED-only 补丁、构建 CLI、运行原有 LED fixture 回归及 EM26 新测试。
3. `python scripts/prepare_dxp2800.py`：基于原固定 DXP6800 curve 产物，仅替换
   dxp2800.so、ugreenctl（新增物理网卡枚举）、ugreenctl-fand、ugreen_leds_cli；其余可执行产物保持原字节。
   独立记录两部分源码来源、哈希、旧产物来源与待实机验证状态。
4. `python scripts/build_ugreenctl.py` 校验并导入，随后执行
   `python tests/run_dxp2800_runtime.py`，检查真正安装的 CLI/插件/守护程序。
5. 运行 BIOS control/API/fan-curve API、LED status batch、硬件映射回归，
   Python importer 测试和 `build_fpk.validate_project()`；前端 check/build。

本地产物位于 `.work-almighty-release/dxp2800/release`，由
`scripts/hardware-release.json` 固定。它是应用构建输入，不是 FPK 或已发布版本。
相应源码快照包含在本地扩展归档的 `dxp2800-extension-source` 中。

## 测试与实机边界

硬件项目 Linux x86_64 Release 构建及隔离 QEMU CTest 35/35 通过。
LED fixture 覆盖四盏灯的 RGB、亮度、开关、闪烁、呼吸、回读失败、
非法灯号、未知芯片、所有者占用和缺失 force/apply；并运行原 LED 回归。

实际 CLI/插件/守护程序在虚拟 DMI 下对 `/tmp` hwmon 文件运行，验证
预览不写、确认后写入/PWM回读、拒绝错误通道/低 PWM、缺传感器满速，
以及 V1.2 修订在访问 hwmon 前被拒绝。安装的 LED CLI 在 EM26 进入
硬件探测，在未列出的 EM27 于档案选择阶段拒绝。
测试没有宿主硬件透传或 NAS 写入。

这仍是固件证据和软件验证。EM26 实际灯效、风扇 RPM 响应、散热效果
尚需目标 NAS 验证，不能以 fixture 通过宣称实机成功。

补充 BIOS 接入测试：来电策略保存/恢复、WOL 网卡列表与显式选择、失败不保存、
缺少写入确认时拒绝、RTC 设置/清除及 cron 持久化均由 fixture 验证。
页面已核对网卡选择、风险确认与模拟保存后的启用状态。

验证日志：应用 `.work-almighty-release/dxp2800/led-tests.log`、
`runtime-tests.log`，以及硬件源项目
`firmware-inbox/work/dxp2800-em26/ctest-final.log`。
前端 check/build、7 项前端单测、15 项模拟 API 测试、8 项导入校验测试通过；
来电策略持久化覆盖 14 个精确产品标识及全部策略、跨机型拒绝、失败恢复。
FPK 构建输入及校验回执已验证；本次没有生成 FPK、推送或发布版本。
