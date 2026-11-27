# 群晖 DSM 原生 SPK

当前主交付形式已改为原生 SPK，不依赖 Container Manager。
安装包结构、普通账户安装脚本、root 封存程序、鉴权、共享锁和升级保留已在隔离 Linux 中模拟验证；
尚未验证 DSM 套件中心是否接受系统单元、向导和真实平台 ABI。

构建和安装说明见 [原生 SPK 指南](../synology/README.md)。本地运行
`python linux/scripts/build_spk.py`，产物与 SHA256 位于仓库根 `dist/`。
`3.0.1-1002` 起提供 DSM 内部应用窗口和自动同源代理；HTTPS 登录 DSM 后从主菜单或套件中心打开即可。
登录仍使用安装时设置的套件密码，尚未接入 DSM 账户单点登录。
当前请使用 `3.0.1-1006` 或更新版本；固件离线检查后已修正系统服务启动方式并改用 DSM 内置 Python。

1004 增加「设备与高级 → 一键导出日志」，包含应用日志、轮转历史和本套件 root 服务启动日志，导出时再次脱敏。

1005 增加安装向导硬件授权；升级可保留或显式替换已有授权，不需要手动修改 policy.json。

1006 同步主分支 v3.2.9 硬件载荷（含兼容 LED 入口与新增机型插件）。

下列 Container Manager 配置保留为共用权限回归测试方案，已不是群晖的首选安装方式。

## Container Manager 验证方案

目标为绿联 x86_64 NAS 上运行的 DSM 7.2+，并且已经安装 Container Manager。
不是原厂 Synology 机型的灯光或 BIOS 兼容承诺，也不是已经签发的原生 SPK。
DSM 若修改了绿联 DMI，应用不提供手动冒充机型的开关；必须先由上游验证检测方式。

## 构建与准备

先在有 Docker 的机器构建并导入镜像，见 [主说明](../README.md)。
部署脚本必须在 NAS 宿主以 root 运行，需要 Python 3.9+。以下路径为示例，
`/volume1/docker` 必须已经存在，`ugreen-toolbox` 必须是专用目录，不能指向已有共享数据：

```sh
sudo python3 linux/deploy/prepare.py --platform synology \
  --directory /volume1/docker/ugreen-toolbox
```

首次运行会提示设置管理密码。不会启动容器、加载驱动、启动灯控或写入硬件。
生成器保留已有密码；需要重置时加 `--reset-password` 并重新创建 Web 容器。
默认 Web 身份为 568:568，可用 `--web-uid` / `--web-gid` 设为专用非零身份，
不要为了修复共享 ACL 而给应用管理员组或 root 身份。

在 Container Manager → 项目 → 新增中导入生成的 `compose.yaml`，
或者在宿主执行：

```sh
sudo docker compose -f /volume1/docker/ugreen-toolbox/compose.yaml up -d
```

没有发布可直接拉取的镜像，必须先确认本地存在 `llled-platform:3.0.1-platform.1`。
如果使用另一个镜像名称，通过生成器的 `--image` 参数明确指定。
项目 UI 操作参考 [群晖官方 Project 说明](https://kb.synology.com/en-global/DSM/help/ContainerManager/docker_project)。

## 访问和硬件授权

默认端口仅监听 NAS 的 `127.0.0.1:5088`。可先通过 SSH 转发到本机访问；
长期使用在 DSM 登录门户 / 反向代理配置 HTTPS 域名，后端指向 `127.0.0.1:5088`。
然后重新生成配置，加 `--public-origin https://toolbox.example.com`，重新创建容器。
浏览器登录用户为 `admin`，密码为准备时设置的值；不要使用明文 HTTP 跨网络传输此密码。
反向代理须转发 Authorization 和专用请求头，不能把它们写入访问日志。

先确认宿主 DMI、系统信息和日志，再按实际机型决定授权。示例：

```sh
sudo python3 linux/deploy/prepare.py --platform synology \
  --directory /volume1/docker/ugreen-toolbox \
  --public-origin https://toolbox.example.com \
  --led-control --device /dev/i2c-2
```

上面的总线号只是参数格式示例，必须替换成经过上游确认的实际设备，不能盲目照抄。
BIOS / 风扇端口 I/O 则单独使用 `--bios-control --raw-io`；
已由内核提供 hwmon 的机型应保留它，按 [权限说明](platform-permissions.md) 显式映射准确目录，
不要卸载驱动以强制改走直控。
生成配置后仍须应用新的 Compose，并在界面主动执行控制；此脚本不会自动启动灯控或恢复风扇曲线。

## 重启和更新

按 [共享锁说明](platform-permissions.md) 配置 root 开机触发任务，在容器启动前准备锁文件。
若 DSM 任务与 Container Manager 的顺序无法保证，先保留手动启动，实测完成前不要声明自启支持。
更新时重新构建 / 导入镜像、保留部署数据和密码、重新创建容器。
控制服务重启后需要主动重新启用 LED 和风扇曲线；请先确保原系统的风扇管理策略可安全接管。

当前未完成 DSM 实机验收。请记录 DSM build、内核、Container Manager 版本、真实 DMI、设备节点、
读写功能和重启结果；不要据此文档直接进行无人值守风扇控制。
