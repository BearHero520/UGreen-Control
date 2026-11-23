# 2026-10-28 本地隔离验证

验证主机为 Windows 上的 QEMU Linux 虚拟机，内核 `6.12.114-0-virt`，
Docker `28.3.3`、Compose `2.36.2`、overlay2。冷启动后启用了 AppArmor 和 Docker 默认 seccomp。
未连接群晖 / TrueNAS NAS，未透传物理控制设备，未执行真实硬件写入。

机器可读记录见 [验证记录](2026-10-28-local-platform.json)。其中平台名称是部署配置 fixture，不能当作对应 NAS 系统已验收。

## DSM 官方固件检查（3.0.1-1003）

当前应使用 `dist/UGreenToolbox-3.0.1-1003-x86_64.spk`。
已下载并离线解包官方 DSM 7.2.2-72806 / DS3622xs+ 固件，检查套件库、系统服务工具、桌面组件和运行库。
检查发现 1001 / 1002 的普通账户控制脚本会调用用户级 synosystemctl，且使用了该工具不支持的 is-active。
1003 改为由 pkgctl 系统单元依赖拉起 root / Web 服务，普通脚本只读健康状态，并使用固件自带 Python 3.8。
详细证据与边界见 [固件检查记录](2026-10-28-dsm-firmware.md)。

以下为较早开发包的基础验证，不能作为旧包在 DSM 上可启动的证明。

## DSM 桌面入口增量验证（3.0.1-1002）

最新交付为 `dist/UGreenToolbox-3.0.1-1002-x86_64.spk` 及 SHA256。
桌面入口采用 DSM `type=app` 与 Vue 应用窗口，自动注册 `/ugreen-toolbox/` 的 DSM nginx 代理。
前端 API 随页面路径解析，既能在 DSM 子路径运行，也保留容器根路径运行方式。

在同一隔离 QEMU 内用真实 nginx 和一次性自签名 TLS 证书验证了 HTTPS 代理、窗口内登录页、
会话 cookie、静态资源及实际 CGI API。HTTP、跨站、缺少 origin、伪造转发头和已注销 cookie 均不能执行写入。
仅原生桌面服务允许同源 iframe，容器 HTTP 服务仍保持 DENY。代理不转发 DSM 登录 cookie。
Web 重启后旧会话失效，原有安装密码和数据保留；可信升级和程序封存检查再次通过。

Linux Python 48 项全部通过；Windows 47 项通过、1 项 Linux socket 检查跳过；前端 9 项测试、类型检查与构建通过。
记录与产物校验值见 [桌面入口验证](2026-10-28-dsm-desktop.json)。
测试中的 host 网络仅用于一次性 QEMU 内的 nginx 联调，不是交付部署配置。
DSM 本身的 Vue 组件、桌面窗口、套件打开按钮和 web-config 安装 / 启停 / 卸载仍待真实 DSM 验收。

以下为此前 3.0.1-1001 和容器镜像的基础验证，保留原记录与校验值。

## 容器

两种配置分别使用 1030:65539 和 568:568 的真实 Linux Web 身份：

- Web 无 capability，代码、认证及 IPC 挂载不可写；硬件服务无网络且默认仅 CHOWN。
- 实际 CGI、登录、跨站拒绝及默认硬件写入拒绝通过。
- 所有控制锁与宿主的设备号 / inode 一致；宿主持锁时容器无法获取；第二个硬件实例不能启动。
- 同组错误 UID 即使通过 socket 文件权限，也会被 SO_PEERCRED 拒绝。
- 权限组不匹配会阻止启动；POSIX ACL 拒绝会被部署检查识别，不会自动清空 ACL 或设置 777。
- socket ACL 不可访问时健康检查返回 503；修正后可恢复。
- 服务正常重启、进程 SIGKILL、残留 socket 和容器重建后，密码及数据保持。
- 缺少宿主锁会拒绝启动，不创建容器私有替代锁。
- 整机冷启动改变了 kernel boot ID；生成的恢复脚本按标签恢复已有实例，并在 AppArmor 下恢复健康。

Docker API 的 `docker kill` 属于管理停止；进程崩溃测试从测试主机向该容器的 PID 发送 SIGKILL，
以验证 restart policy，而不是把管理停止错误地当作自动恢复故障。

## 原生 SPK

SPK 构建、结构、完整性与 Python / glibc 安装前检查均有测试。
在一次性 Debian 容器中模拟了 DSM 账户与系统单元部署，真实运行 root broker 和 UID/GID 20001 的 Web：

- 安装脚本以普通账户运行，一次性安装密码在 Web 启动后删除。
- 账户主组与套件专用组不同时，认证文件和 socket 仍使用专用组，Web 实际运行组必须匹配。
- root 先验证载荷快照，再从独立 root 目录执行；普通套件用户不能改写运行代码和策略。
- 程序目录和 Unix socket 就绪后才发送 systemd READY 通知，避免 Web 抢先启动。
- 篡改 package 用户可写的载荷会被拒绝；已封存版本可以安全重启。
- 可信升级切换程序版本，保留原有密码和数据。
- 原生服务与容器服务共用宿主排他锁，已有容器接管时原生服务会退出。
- 原生版更新入口明确采用 SPK / 套件中心，不指向 FPK 或容器镜像。

这验证的是原生服务实现，不是 DSM 套件中心模拟器。尚未验证 DSM 是否接受系统单元、安装向导和
synosystemctl 控制、实际 Python 路径、Synology ACL、NFSv4 ACL、平台开机顺序及真实机型能力。
systemd 的 sandbox 指令还需在实际 DSM 上确认是否被内核和服务管理器正确执行。

本轮 Linux Python 测试全部通过（42 项），前端 7 项测试、类型检查和构建通过。
Windows Python 检查中仅 Linux socket roundtrip 一项跳过；该项已在 Linux 通过。
固定硬件载荷未变，LED CLI 与标准应用包内的版本逐字节一致。

本地交付文件位于仓库根 `dist/`：`UGreenToolbox-3.0.1-1001-x86_64.spk` 和
`UGreenToolbox-3.0.1-platform.1-linux-amd64.docker.tar`，均附 SHA256 校验文件。
镜像归档中的 tag 为 `llled-platform:3.0.1-platform.1`；归档完整性和镜像 ID 记录在机器可读报告中。
