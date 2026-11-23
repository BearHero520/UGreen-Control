# DSM 官方固件离线检查

检查对象为官方 **DSM 7.2.2-72806 / DS3622xs+（broadwellnk，x86_64）**。
下载、解包文件和原厂程序仅保存在忽略目录 `.work-platform-docker/dsm-firmware/`，不随源码或 SPK 分发。
来源、哈希和隔离测试结果见 [机器可读记录](2026-10-28-dsm-firmware.json)。

## 对 root 执行方案的修正

1001 / 1002 的方案存在真实 DSM 启动问题，不能因 Debian 中直接启动进程成功就认为套件可启动。

| 检查对象 | 固件证据 | 1003 的处理 |
| --- | --- | --- |
| synosystemctl | root:root、0755；非 root 分支会给 systemctl 添加 `--user` | 普通账户脚本不再调用它启动系统服务 |
| 服务状态命令 | 原厂工具支持 get-active-status，未提供 is-active | 控制脚本改为读取 Web 与 broker 的联合健康接口 |
| pkg-service 覆盖文件 | 用原厂 libsynopkg 实际读取后，Unit.Requires / Unit.After 被过滤 | 不使用该覆盖文件注入系统依赖 |
| 系统单元注册 | 原厂 EnumAvailableUnits / InstallUnits 能识别并复制 pkg-UGreenToolbox 的两个系统单元 | 保留普通安装账户与独立系统单元 |
| 系统单元启用 | 原厂生成器识别 DefaultEnable=yes，并根据 RequiredBy 创建 pkgctl 的 requires 链接 | 系统管理器负责拉起 root 硬件服务及普通 Web；Before 保证启动次序，PartOf / BindsTo 随套件停止或失败退出 |

本轮没有采用 sudoers、SUID 或把 Web 改成 root。安装与控制脚本仍为 `run-as=package`；
root 代码继续由系统单元中固定的启动器和 SHA256 约束，从独立 root 目录执行。
硬件 API、真实 UID 检查、共享锁和默认关闭写入的边界保持。

## 运行库与桌面检查

固件提供 `/usr/bin/python3 -> python3.8`，目标文件为 root:root、0755，实际版本为 3.8.15。
1003 默认使用该路径，去掉额外 Python3 套件依赖，并替换了运行代码中 Python 3.9 才提供的路径方法。
DSM 修改过 http.client：本机健康检查显式绕过其系统代理，避免 localhost 请求受 NAS 代理设置影响。

固件的 glibc 为 2.36。使用原厂动态加载器执行 `--list`，LED CLI、ugreenctl、ugreenctl-fand 的依赖解析通过；
这项检查不执行它们的程序入口。未改变硬件载荷，也未执行硬件写入。
桌面脚本中确认存在 Vue 及 v-app-instance / v-app-window，nginx 静态资源机制也存在于套件库中。

## 已执行的隔离验证

- 使用官方旧版安装镜像中的原厂解包程序读取 7.2.2 PAT，全部在无物理设备映射、无网络的容器中执行。
- 在一次性容器中调用原厂套件库，枚举、安装并生成本应用的系统单元；输出仅写入测试目录。
- 调用原厂系统生成器及 systemctl 的离线 enable 功能，确认两个 requires 链接指向对应系统服务。
- 使用固件的 Python 3.8、标准库、libpython 和 libffi 运行 SPK 中的 broker / Web，验证普通账户健康检查、登录、实际 CGI、HTTPS 同源代理、封存代码、可信升级和重启。
- Linux Python 测试 49 项通过；Windows 48 项通过、1 项 Linux socket 测试跳过。前端沿用已验证的 1002 构建。

## 尚未证明的部分

没有启动完整 DSM，也没有在套件中心执行安装。尚需实机确认：安装器与资源 worker 的完整时序、
PID 1 实际启动和 sandbox、开机 / 停止 / 卸载、DSM 窗口显示、Synology ACL、真实 DMI 和硬件读写。
不能把这一份 broadwellnk 固件检查扩大为所有 DSM 7.2 / 7.3 或所有 CPU 平台的兼容承诺。

来源：[官方固件目录](https://archive.synology.com/download/Os/DSM/7.2.2-72806)、
[官方引导工具来源](https://archive.synology.com/download/Os/DSM/7.0.1-42218)、
[系统单元注册说明](https://help.synology.com/developer-guide/resource_acquisition/systemd_user_unit.html)。
