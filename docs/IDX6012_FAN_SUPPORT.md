# iDX6012 风扇控制接入（2026-11-15）

本次只接风扇，来电恢复、WOL 与定时开机不随本次开放。
硬件实现为相邻 UGREEN-NAS-Hardware 仓库 ec/idx6012.c；应用仅调用 ugreenctl。

用户明确授权：仅对已确认 iDX6012，以精确 UGREEN 厂商、主板版本
`iDX6011_iDX6012 V3.0 241112`、BIOS `5.32` 与日期 `11/12/2024`，
以及原厂 ITE5571 源码证据，替代尚不可用的运行时芯片 ID。
该例外不适用于其他修订。程序不伪造数字芯片 ID。

功能：两路 sys 风扇同步固定 PWM 40–198；恢复 EC 智能模式；转速及手动 PWM 回读。
写入要求 --force --apply、所有者检查、互斥锁；先写 PWM 再启用手动模式，
失败恢复原状态，恢复不确认则报错。当前不提供软件五点/八点温控曲线。

应用通过上游 model 命令匹配 OEM 主板，不使用 LED 手动机型选择绕过硬件保护。
UI 沿用原有自定义自动与固定转速入口，显示 ITE5571 与 198 上限；不增加独立智能模式按钮。
/bios/fan/mode 仅允许 iDX6012 + sys + auto，继续沿用受保护写入确认。

实机：PWM 150 写入与两路回读成功，约 1370/1360 RPM 升至 1970/1949 RPM；
恢复智能模式后回落至 1378/1364 RPM。应用 CGI 的固定 PWM、智能恢复均验证成功。
未逐档测试整个 PWM 范围。测试完成留在智能模式。

构建：上游 39 项 CTest、应用机型适配测试、前端 TypeScript/Vite、8 项导入测试通过。
原厂输入、SHA256 和寄存器完整记录位于上游 docs/IDX6012_EC_INTAKE.zh-CN.md。

设备上的 v3.1.4 已备份并更新对应运行文件，未更改版本号。
备份：/var/apps/App.Native.UGreenLED/var/idx6012-backups/20261115-gr8k06p1。
部署及 API 证据保存在忽略目录 .work-almighty-release/idx6012_hardware/。
LED 二进制 SHA256 保持 39f74bb27e21e1d14a5f41d7ad2a640684a2aaaed0c9406176bf6cf36104a5b2。
本次没有重打 dist 内的旧 v3.1.4 安装包，没有提交、推送或发布。

应用回归：Linux 环境 BIOS control、iDX6012 adapter、BIOS API 全部通过。
页面首次确认发现保存白名单遗漏 idx6012，已修复并新增确认持久化/机型隔离测试。
实机 POST /bios/confirmation 成功，随后 GET /bios/status 返回 write_confirmation_acknowledged=true；风扇仍为智能模式。

2026-11-15 后续：按用户要求，上游 core/fan_daemon.c 接入 iDX6012，复用现有 custom 温控策略与守护进程，所有写入仍经过精确机型插件。OEM 名称经 core/identity.c 归一化；仅使用 sys 双风扇同步通道，曲线和保护值上限 198，不套用其他机型原厂曲线。
应用恢复原有“自定义自动”入口，数字输入、拖动曲线与保护文案遵守 max_pwm；移除新增独立恢复按钮。现有停止自动温控语义保持为最后安全 PWM。
验证：39 项原 CTest + 新增 idx6012-curve 测试通过，TypeScript/Vite、8 项发布导入测试通过；Linux 原温控 API 回归及新增 iDX6012 启动/超限拒绝测试通过。
实机使用原 /bios/fan-curve 启动临时 custom 曲线，守护服务报告 idx6012、running、applied_system_pwm=150；测试后停止并恢复设备智能模式，还原原配置文件。未替用户启用持久曲线。
本轮设备备份：/var/apps/App.Native.UGreenLED/var/idx6012-backups/idx6012-curve.TFZqv7e3。
实机日志：.work-almighty-release/idx6012_hardware/custom-live.log。未重新打包、提交或推送。

## 原厂兼容自动（用户要求的相近六盘预设）
上游新增 stock-idx6012-compat，复用 DXP6800 Pro 的 CPU/HDD/NVMe 阈值，系统 PWM 64/130/198/198，CPU 保底 65°C→125、90°C→198，保护值 198；所有输出仍走 iDX6012 成对 sys 通道。此为兼容预设，不是从 iDX6012 固件恢复的原厂曲线。UI 原入口注明采用 Pro 参数。
40 项 CTest 通过，包含低温/高温/缺温保护、预设机型隔离与范围测试；Linux 原曲线 API、自定义与兼容预设 API 回归通过；TypeScript/Vite 构建通过。长期热负载尚未验证。
用户反馈桌面图标消失时，系统应用列表仍为 installed/running；经 appcenter-cli 正常停止/启动后，用户已确认图标恢复，具体缓存/注册原因未确定。

## 来电启动、网络唤醒和下拉框修复（2026-11-15）
硬件实现已在 UGREEN-NAS-Hardware 完成：power/idx6012_power.c 精确主板/BIOS 与 Intel 8086:7e02 校验、MMIO 0xfe001020 位 0 RMW、控制器冲突、root 锁、写后回读和失败回滚；独立 WOL profile 使用两张物理 PCI 网卡的内核驱动接口。详细原厂来源、SHA256 与寄存器依据见上游 docs/IDX6012_EC_INTAKE.zh-CN.md。未复制低层控制到应用。
实机 CLI 来电 off/on、WOL off/on 回读通过，恢复原始 on/on。安装后的 /bios/status 返回 startup_available=true、startup=on、startup_readback=true、startup_restore_supported=false、wol_available=true、wol=on。没有执行关机唤醒或断电恢复实测，未宣称 BIOS NVRAM 持久化。RTC 仍独立且未开放。
界面隐藏不支持的“恢复断电前状态”；三个硬件下拉框统一圆角、44px 高度、内边距、箭头、键盘焦点与禁用样式，保留原生选择行为，浏览器截图确认布局正常。
上游 41 项 CTest、前端 TypeScript/Vite、应用接口验证通过。此轮设备备份位置记录于 .work-almighty-release/idx6012_hardware/bios-stage.txt；实机日志 power-wol-live.log 与 bios-status.json。未重新打包或发布。

## 遥测、模式切换与启动恢复修复（2026-11-15）
补入 iDX6012 双风扇遥测白名单；实机接口已返回两路有效 RPM，CPU RPM 为 -1（不存在）。前端隐藏不存在的 CPU 风扇卡片，未知 PWM 显示 —，实际运行模式改变时同步选择状态。
BIOS POST 请求使用应用互斥锁；温控服务接收 TERM 后等待退出（最多 5 秒），超时保留 PID 并拒绝模式切换。新守护子进程关闭应用锁 FD，内部恢复 EC auto 接口先停曲线，写失败时恢复旧曲线。上游硬件锁和回读保持不变。
固定 PWM 与 WOL 成功写入后保存，绑定上游识别机型、原始产品名和主板版本。应用 start 恢复用户保存的 WOL，并在已启用曲线优先的规则下恢复曲线或固定 PWM；成功启用曲线/EC auto 会清除旧固定配置。未主动迁移用户当前临时状态为持久设置；用户下一次成功应用时保存。没有新增应用侧寄存器访问。
测试：Linux 专项覆盖保存/恢复、机型隔离、失败保留、曲线优先、慢退出等待、超时拒绝切换与双路遥测；完整 BIOS API、温控 API、BIOS control 回归通过，TypeScript/Vite 通过。没有重启 NAS 进行整机重启验证。
部署备份 /var/apps/App.Native.UGreenLED/var/idx6012-backups/idx6012-restore.GZZQgWrd。仅重启遥测服务；用户原有 stock-idx6012-compat 曲线仍运行，WOL 保持 on。未打包或发布。
