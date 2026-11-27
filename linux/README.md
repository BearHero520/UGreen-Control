# UGREEN工具箱：群晖 / TrueNAS 实验适配

开发分支：`synology-truenas`。应用和部署入口在本目录；仓库根目录继续维护 fnOS。

当前主交付形式为群晖原生 SPK 与 TrueNAS 应用中心安装版。
已提供 [SPK 构建与安装说明](synology/README.md)、容器构建入口、独立鉴权和隔离硬件服务。
**镜像和原生服务已在隔离 Linux 验证；DSM 套件中心与 TrueNAS 实机仍未验收。**

目标是运行在绿联 NAS 硬件上的 x86_64 DSM 7.2+ 原生套件，
以及 TrueNAS Linux 24.10+ / Custom App。原厂群晖硬件、ARM、TrueNAS CORE 不在本轮适配范围。
DSM 环境若隐藏或修改 DMI，上游不能识别真实绿联机型时，控制保持不可用。

## 部署方案

群晖 SPK 使用普通套件账户 Web 和独立系统硬件服务，无需 Container Manager。
root 单元先校验载荷快照并封存程序，套件账户无法替换 root 执行的文件。
通过 DSM HTTPS 登录后从桌面 / 主菜单直接打开内部窗口；套件自动注册同源代理，无需手动配置域名。

TrueNAS 应用中心方案使用一个镜像、两个独立容器；群晖容器配置仅保留为权限回归测试：

| 服务 | 用户与权限 | 挂载与通信 |
| --- | --- | --- |
| Web | 普通 UID/GID，默认 568:568，移除全部 capabilities | 只读认证文件及 Unix socket；不映射设备、不读取硬件数据目录 |
| 硬件服务 | 容器内 root，默认仅 CHOWN，无网络、无对外端口 | 宿主 `/proc`、`/sys` 只读；独立数据目录；宿主锁逐个共享 |

浏览器使用 `admin` 和部署时设置的独立密码登录。密码只保存 scrypt 派生值；
Web 接口要求鉴权，写入还要求同源及专用请求头。硬件服务通过 Unix socket 的真实 UID 校验调用方，
只允许固定应用 API，不接受任意命令、可执行文件路径或浏览器提供的环境变量。
默认只绑定宿主 `127.0.0.1:5088`；SPK 自动接入 DSM HTTPS 同源入口，TrueNAS 则使用 NAS 的 HTTPS 反向代理。
这是应用自己的单管理员登录，尚未接入 DSM / TrueNAS 账户和多用户权限。

灯光始终由随包 `server/bin/ugreen_leds_cli` 执行；它与仓库标准 LED CLI 的字节一致性有测试约束。
风扇、BIOS、来电恢复继续通过固定上游版本的 `ugreenctl`。不新增硬件寄存器逻辑。

部署生成器默认关闭两类硬件写入，不自动启动 LED 或风扇曲线，也不在重启时恢复 BIOS 策略。
授权灯光和授权 BIOS 是两个独立参数。写入功能仍依赖上游精确机型、控制器互斥、风险确认与回读。

## 开发与验证

仓库根目录执行：

```sh
python3 -m unittest discover -s linux/tests -p 'test_*.py'
bash linux/tests/test_platform_telemetry.sh
cd linux/web-ui
npm ci
npm run check
npm test
npm run build
```

镜像构建入口（在有 Docker 的开发机执行）：

```sh
docker build --platform linux/amd64 -t llled-platform:3.0.1-platform.1 linux
```

Dockerfile 在构建时执行前端类型检查、测试和上游载荷 SHA256 校验。
镜像默认用户为普通用户，只有部署文件中的硬件服务显式使用容器 root。
当前没有发布远程镜像；在 NAS 上构建，或在开发机导出镜像后通过 Container Manager / docker load 导入。
如果导出镜像归档，最终分发文件及校验应放在仓库根 `dist/`。

详细操作见 [群晖部署](docs/synology.md)、[TrueNAS 部署](docs/truenas.md)、
[权限与功能矩阵](docs/platform-permissions.md)。部署生成器需要宿主 Python 3.9+；
DSM 请先确认 Python 3 路径，不为运行此应用修改 TrueNAS 系统软件包。

## 当前能力边界

- 可生成两个平台的专用 Compose 配置，默认不使用 privileged、host PID、host 网络、Docker socket 或原始磁盘透传。
- 系统资源读取宿主元数据；宿主系统名称与主机名在部署时生成快照，变更后重新生成配置。
- 磁盘活动从只读 sysfs 和 `/proc/diskstats` 采样，不向存储盘发送 ATA/SCSI 命令；“空闲”仅表示没有 I/O，不代表已确认电源状态。
- 网络使用宿主链路 carrier 和字节计数；不在隔离容器里判断公网连通性或 VPN。
- LED、风扇、来电恢复必须逐机型验证设备与系统权限后单独授权。现有 N76 兼容入口的驱动依赖仍存在，不能靠透传 I2C 宣称所有机型可用。
- 自动温控曲线尚未完成进程退出与风扇接管验证，首版暂不开放启停。
- 定时开关机、驱动装卸、完整诊断打包返回明确的未适配错误；请使用 NAS 自带的定时开关机。没有挂载宿主 cron、systemd 或电源管理接口。
- 关于页读取随镜像提供的说明，更新不再链接 fnOS FPK。

本地通过不等于 NAS 验证。Unix socket 凭据、只读挂载、POSIX ACL、共享锁、崩溃恢复和 AppArmor 冷启动已在隔离 Linux 验证；
DSM / TrueNAS 的实际 ACL、服务框架、机型检测和每种硬件写入仍须在目标系统上验收。验收表见 [迁移记录](docs/migration.md)。

## 来源与许可证

保留 `SOURCE.json` 中的迁移来源记录。硬件载荷同步至与标准应用一致的固定发布版
`local-dxp4800gt-fnos-20261116`（见 `scripts/hardware-release.json`），不修改 `server/bin` 与上游插件、文档的原始字节。
应用采用 AGPL-3.0-only，第三方许可见 `LICENSE` 和 `app/server/lib/ugreenctl/upstream/`。
