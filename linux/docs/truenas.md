# TrueNAS Linux 实验部署

目标为 x86_64 TrueNAS Linux 24.10+ 的 Apps → Custom App → Install via YAML。
TrueNAS CORE / FreeBSD 和旧 Kubernetes Apps 不适用。
官方入口见 [Installing Custom Apps](https://apps.truenas.com/managing-apps/installing-custom-apps/)。

## 数据集和配置

先在存储池中建立专用应用父目录，例如 `/mnt/tank/apps`，不要挂载池根目录、SMB 数据或 ZFS 管理 socket。
部署生成器在其下准备 `ugreen-toolbox` 的 data / auth / ipc；仅修改这些目录本身，
不递归调整数据集权限。使用无共享 ACL 的独立目录；若 NFSv4 ACL 阻止访问，应修正该应用目录的 ACL，不能设置 777。

先在有 Docker 的机器构建并导入镜像，见 [主说明](../README.md)。
本地已构建的实验镜像也可从仓库根 `dist/` 导入：

```sh
cd dist
sha256sum -c UGreenToolbox-3.0.1-platform.1-linux-amd64.docker.tar.sha256
sudo docker load -i UGreenToolbox-3.0.1-platform.1-linux-amd64.docker.tar
```

导入后镜像名为 `llled-platform:3.0.1-platform.1`。归档来自隔离 Linux 构建，并非 TrueNAS 实机验收产物。
宿主使用系统已有的 Python，以 root 执行，不安装额外系统软件包：

```sh
sudo python3 linux/deploy/prepare.py --platform truenas \
  --directory /mnt/tank/apps/ugreen-toolbox
```

首次运行设置管理密码。默认 UID/GID 568:568 只用于非特权 Web 容器；
硬件容器是 root，但没有网络、不公开端口、移除全部 capabilities 后仅加回 CHOWN。
程序不会修改 TrueNAS 系统服务或自动进行硬件写入。

在 Custom App YAML 编辑器中粘贴生成的 `compose.yaml` 完整内容。
JSON 是合法 YAML，生成器直接使用结构化 JSON 避免路径和布尔值的 YAML 歧义。
先确认镜像已存在于本机；若使用私有镜像库，通过 `--image` 指定已经可拉取的镜像。

## 访问和权限

默认 Web 仅在 NAS `127.0.0.1:5088`，可通过 SSH 转发进行首次检查。
如果 HTTPS 反向代理需要通过 NAS LAN 地址连接后端，生成时明确设置实际地址和公开 origin：

```sh
sudo python3 linux/deploy/prepare.py --platform truenas \
  --directory /mnt/tank/apps/ugreen-toolbox \
  --bind-address 192.168.1.10 \
  --public-origin https://toolbox.example.com
```

将示例地址换成实际 NAS 地址，并用防火墙限制 5088 只供反向代理访问。
浏览器只访问 HTTPS 入口，以 `admin` 和部署密码登录；这与 TrueNAS 管理员账号独立。
反向代理须保留 Authorization 和专用请求头，避免把认证头写入日志。

确认监控正常后再授权硬件。例如上游已验证的端口 I/O 路径：

```sh
sudo python3 linux/deploy/prepare.py --platform truenas \
  --directory /mnt/tank/apps/ugreen-toolbox \
  --bios-control --raw-io
```

上例仅表示配置格式，不代表你的机型已经通过该路径验证；也没有授权灯光。
LED 使用独立 `--led-control`，实际设备用重复的 `--device` 指定，只允许控制字符设备，拒绝 `/dev/sda` 等存储盘透传。
若上游优先使用 hwmon，保留驱动，并显式给出 `--hwmon-path /sys/devices/.../hwmon/hwmonN`；
默认 AppArmor 拒绝 sysfs 写入时，应加载审核后的专用 profile，再用 `--apparmor-profile` 指定。
不为这个应用启用 privileged、SYS_ADMIN、SYS_MODULE、SYS_BOOT、host PID 或 host 网络。

## 开机、升级和功能限制

按 [共享锁说明](platform-permissions.md) 取得自包含 root 开机命令，
在 System → Advanced → Init/Shutdown Scripts 中配置为 Pre Init Command，
确保它先于 Apps 容器执行。TrueNAS 对此阶段定义见 [Init/Shutdown API](https://api.truenas.com/v26.0/api_methods_initshutdownscript.create.html)。
必须在实际版本验证顺序和失败行为；不能保证时先禁用本应用自动启动，开机准备锁文件后手动启动。

不要用容器里的 shutdown 代替 NAS 关机。本应用没有接管 TrueNAS 的电源计划、ZFS、磁盘休眠策略或风扇驱动。
磁盘监控只读取宿主元数据，不发送存储盘控制命令。
更新镜像和密码后重新部署 YAML，保留 data/auth；容器重建后控制服务需主动重新开启。

当前未完成 TrueNAS 实机验收。请记录版本、内核、Docker、真实 DMI、LSM、设备节点、
每项控制与重启结果后再决定开放哪些功能。
