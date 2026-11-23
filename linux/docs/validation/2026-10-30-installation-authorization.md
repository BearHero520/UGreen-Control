# SPK 1005 安装授权验证

日期：2026-10-30；分支：`synology-truenas`。
产物：`dist/UGreenToolbox-3.0.1-1005-x86_64.spk`，SHA256：`0d42818604af1ea60d36f992587d57bf11ecffc4fb17976038d1949923c6ce9a`。

## 用户操作

新安装：使用 DSM 管理员在套件中心手动安装，在向导分别选择「启用灯光控制」和「启用 BIOS／风扇／来电恢复控制」。无需 SSH 修改 policy.json。
从 1004 升级：在升级向导勾选「采用以下授权选择」，再选择需要的控制；不勾选替换选项则保留原授权。
勾选替换但两类控制都不选，可关闭全部硬件写入。安装和重启仅保存授权，不自动执行硬件写入。

## 结果

- Linux 65 项测试通过；Windows 53 项通过、12 项 Linux 专属测试跳过。
- 新装默认关闭、新装同时授权、旧版升级改为仅灯光授权、旧版升级保留原有双授权，四种真实 SPK 服务模拟各通过 18 项检查。
- 使用官方 DSM 7.2.2-72806 / DS3622xs+ 的 Python 3.8.15，实际运行打包后的普通账户安装脚本、root 封存启动器和 Web / 日志 / 硬件服务。
- 非法选项在写入前拒绝；root 策略原子保存，重启和升级后保留；伪造、重放请求无法改变已消费版本的授权。
- 单项／双项／未选、升级保留和明确撤销均有测试；错误版本、字符串布尔值、未知字段、超限文件、链接、FIFO、错误所有者及错误权限均拒绝。
- HTTPS 登录、日志导出脱敏、硬件服务失败后导出、密码和历史保留回归通过。
- 固件原始系统单元安装器与生成器登记三个服务，复制出的单元与最终 SPK 字节一致。

## 权限与边界

安装脚本、控制脚本和网页使用普通套件账户；独立 root 日志服务在网页启动前消费本版本安装请求，并把授权与版本回执一起写入 root 策略。
请求只允许两类布尔授权，不接受命令、路径、环境或其他策略字段。回执版本来自 root 系统单元内固定配置，普通账户无法修改。
同版本重装保留回执与策略，不重复消费向导选择。此机制依赖 DSM 管理员批准安装更新及其服务生命周期；它不是独立的 DSM 用户鉴权协议。
LED 与 BIOS 控制面保持分离，固定上游载荷与 canonical LED CLI 字节未改动。安装授权不会绕过机型检测、共享锁、操作确认或上游回读。

所有测试均在无真实设备映射的隔离 Linux / 临时容器中完成。真实 DSM 套件中心向导、PID 1 生命周期、ACL 和硬件控制仍待实机验收。

依据：[向导变量](https://help.synology.com/developer-guide/synology_package/wizard/WIZARD_UIFILES_v2.html)、[DSM 7 权限要求](https://help.synology.com/developer-guide/breaking_changes.html)、[系统单元机制](https://help.synology.com/developer-guide/resource_acquisition/systemd_user_unit.html)。
完整结果：[JSON 记录](2026-10-30-installation-authorization.json)。
