# 平台权限与验收边界

群晖主方案为 [原生 SPK](../synology/README.md)，TrueNAS Linux 24.10+ 采用 Custom App / Compose。
SPK 的 Web 与安装脚本使用普通套件账户；root 系统单元内嵌启动器和载荷 SHA256，
只运行独立 root 目录中校验并封存的程序。硬件服务声明仅允许 AF_UNIX。
容器与原生服务共用同名宿主锁，不能同时接管硬件。
依据 [DSM 7 权限要求](https://help.synology.com/developer-guide/breaking_changes.html)、
[DSM 系统单元机制](https://help.synology.com/developer-guide/resource_acquisition/systemd_user_unit.html)
和 [TrueNAS Custom Apps](https://apps.truenas.com/managing-apps/installing-custom-apps/)。

下面的容器权限和开机任务适用于 TrueNAS，群晖容器配置保留为隔离回归测试。
原生 SPK 由 systemd 就绪通知、服务依赖和重启策略管理；DSM 实际授权仍待实机确认。
系统级 pkgctl 单元通过 Requires / After 拉起硬件和 Web 服务，普通账户脚本只读健康状态。
不能在普通账户脚本中调用 synosystemctl 启动 root 服务：DSM 7.2.2 固件实现会为非 root 自动选择 --user。

## 必须保持的权限边界

- Web 不能以 root 运行，不能映射硬件设备、宿主 `/proc`、`/sys`、数据目录或 Docker socket。
- 硬件服务没有网络和公开端口；仅使用本地 Unix socket，检查 `SO_PEERCRED` 的 UID。
- 两类写入分别授权；所有用户可见的写入都经固定路由、请求方法、输入校验和上游控制器保护。
- 上游载荷在启动硬件服务前校验 SHA256。LED 不查找 `/usr/bin` 的替代程序，不接受客户端程序路径。
- 不加载或卸载 NAS 驱动，不修改宿主 BIOS 驱动策略来强行解锁设备。
- 保持 Docker 默认 seccomp / AppArmor。端口 I/O 额外需要 SYS_RAWIO；Docker 默认 seccomp 对该 capability 开放 `ioperm`，不能据此把整个 seccomp 关闭。
- 如果明确需要 hwmon 写入，仅挂载那个实际设备目录为可写；Docker 默认 AppArmor 会拦截 sysfs 写入，需要另行审核并加载专用 profile，再传 `--apparmor-profile <名称>`。不要自动关闭整台 NAS 的 LSM。

上述 capability 和隔离行为参考 [Docker 运行权限](https://docs.docker.com/engine/containers/run/)、
[Moby seccomp profile](https://github.com/moby/profiles/blob/main/seccomp/default.json)
和 [Moby AppArmor profile](https://github.com/moby/profiles/blob/main/apparmor/template.go)。
不同 DSM / TrueNAS 搭载的 Docker 和内核版本可能不同，以目标主机验证为准。

## 存储和 ACL

| 路径 | 所有者 / 模式 | Web 权限 |
| --- | --- | --- |
| 专用部署目录 | root:root / 0700 | 不挂载 |
| data/ | root:root / 0700 | 不挂载 |
| auth/ | root:Web GID / 0750 | 只读挂载 |
| auth/auth.json | root:Web GID / 0640 | 只读，含密码派生值 |
| ipc/ | root:Web GID / 0750 | 只读挂载目录，可连接 socket |
| ipc/hardware.sock | root:Web GID / 0660 | Unix socket 连接 |
| 宿主 /run/ugreenctl-*.lock | root:root / 0600 | 不挂载 |

使用专用、无 SMB/NFS 共享 ACL 的应用目录。生成器只调整这些目录本身及其自有文件，
不递归 chmod/chown 数据集，不设置 777。已有目录不是 root 所有时直接拒绝；
NFSv4 ACL、继承 ACL 和 Docker user namespace remap 还需实际验证，不能只看 UID 数字。
568 是默认服务身份，允许显式改成其他非零 UID/GID；它不授予 NAS 管理账户权限。
更改 UID/GID 或密码后重新创建容器，确保新文件和 socket 权限生效。

## 跨容器锁和重启

`llled-hardware.lock` 保证本应用只运行一个硬件服务；下列锁保持上游的真实宿主路径：

```text
/run/ugreenctl-led-mcu.lock
/run/ugreenctl-it8613.lock
/run/ugreenctl-fand.lock
/run/ugreenctl-wol.lock
/run/ugreenctl-rtc.lock
```

生成器使用 O_NOFOLLOW 创建或验证普通 root 锁文件，拒绝链接和宽松权限，
逐个 bind mount 到硬件容器的同名路径。不能为每个容器各造一套锁；
也不能直接把整个宿主 `/run` 挂载给容器，以免暴露 Docker 和 NAS 管理 socket。
上游继续执行自己的控制器锁和所有者检查；不会通过这些锁改变上游硬件实现。
宿主 `/proc`、`/sys` 只读提供给硬件服务，以保留原厂驱动、控制进程检测和真实遥测。

宿主 `/run` 在重启后会清空。必须在容器服务启动前运行生成器输出的 root 开机命令，
重建锁文件，再启动应用。命令可由以下方式再次取得：

```sh
python3 linux/deploy/prepare.py --print-boot-command
```

输出是自包含 Python 命令，不依赖数据池挂载或代码目录。TrueNAS 可配置 Pre Init Command；
DSM 使用 root 的开机触发任务。必须实测任务与容器服务的启动顺序；
无法保证顺序时先关闭本应用自动启动，开机准备锁文件后再手动启动，不能冒险使用私有锁回退。
不要在控制器服务运行期间删除、替换或重建已存在的锁 inode。

生成器还会写出 root 专用 `recover.py`。在 Docker 启动后的 Post Init / 开机任务中运行它，
会按部署标签定位已有容器，准备锁后先恢复硬件服务并等待健康，再恢复 Web。
不会创建第二套应用，也不会操作其他部署；运行中丢锁会拒绝自动新建 inode。
该路径已通过隔离 Linux 的整机冷启动与 AppArmor 验证，平台任务顺序仍待 NAS 实测。

## 功能矩阵

| 功能 | 默认 | 增加的权限 / 条件 | 本分支 NAS 实测 |
| --- | --- | --- | --- |
| Web 登录、固定 API、日志 | 启用 | 普通 UID、auth / ipc 权限正确 | 未验证 |
| CPU、内存、温度、内核、DMI | 只读 | 真实宿主 proc / sys | 未验证 |
| 磁盘活动 | 元数据采样 | 无原始磁盘透传；映射依赖真实 HCTL | 未验证 |
| 磁盘休眠 / 深睡眠识别 | 不提供 | 不对 ZFS / RAID 存储盘发送 ATA/SCSI 命令 | 不适用 |
| 网络流量 / 链路 | 元数据采样 | 宿主 sysfs；不判断公网和 VPN | 未验证 |
| LED | 禁止写入 | --led-control；按上游实际后端授权设备 | 未验证 |
| BIOS / 来电恢复 | 禁止写入 | --bios-control；上游精确 DMI；必要时 SYS_RAWIO | 未验证 |
| 风扇固定 PWM | 禁止写入 | BIOS 授权；准确 hwmon 或上游端口 I/O | 未验证 |
| 自动温控曲线 | 暂不开放启停 | 上游守护退出后保留最后 PWM；需完成容器退出及风扇接管验证 | 未验证 |
| 定时开关机 / 驱动管理 | 不提供 | 使用 DSM / TrueNAS 系统设置 | 不适用 |

N76 兼容 LED CLI 的驱动依赖尚未由本应用解决，`--device /dev/i2c-N` 不代表能绕过驱动。
缺少上游能力时先在 UGREEN-NAS-Hardware 验证或实现，再更新固定载荷；不能在此新增 LED sysfs 或 I2C 后门。
