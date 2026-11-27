# 群晖原生 SPK 实验版

此版本以 DSM 套件中心为安装目标，不依赖 Container Manager。已完成开发包和隔离 Linux 中的原生服务模拟验证，
**尚未通过真实 DSM 的安装、systemd 资源授权及 ABI 验收**。

## 运行结构

- 安装和控制脚本以 `ugreen-toolbox` 普通套件用户运行，`conf/privilege` 使用 `run-as=package`。
- 通过 DSM 文档提供的 `conf/systemd/pkg-*.service` 注册 Web、硬件及只读日志三个独立系统服务。
- 三个单元均设置 DSM 的 `DefaultEnable=yes`。Web 与日志服务使用 `RequiredBy=pkgctl-UGreenToolbox.service`，
  硬件服务使用 `WantedBy`；由系统服务管理器启动和停止，不从普通账户调用 root systemctl。
  Web 依赖日志服务，仅尝试启动硬件服务；硬件启动失败时仍可登录和导出启动日志。
  普通账户控制脚本检查 Web 与日志服务的健康状态，硬件接口独立报告未就绪。
- Web 使用套件账户、空 capability 集；硬件服务使用独立 root 身份和受限 capability 集。
- DSM 桌面入口注册为 `type=app`，使用 DSM 提供的 Vue `v-app-instance` / `v-app-window`，在可调整大小的内部窗口中嵌入完整管理界面。
- 通过 DSM `web-config` 注册 `/ugreen-toolbox/` 同源代理，仅转发到本机 Web，不要求手动创建域名或反向代理规则。
- root 系统单元内嵌固定 SHA256 和启动代码。它读取整个载荷快照、验证校验值后，再解包到安装卷上
  root 所有的 `@UGreenToolbox/versions/<sha256>`。绝不以 root 执行套件账户可替换的脚本。
- `data/` 归 root、模式 0700；`auth/` 只向 Web 组提供读取权限；代码目录和策略文件不能由套件用户写入。
- BIOS 与 LED 分别调用原有 `ugreenctl` 与 bundled `ugreen_leds_cli`；没有新增底层硬件后端。

套件升级保留独立数据和密码，不自动恢复灯光或 BIOS 写入。卸载由 DSM 停止并移除服务，私有数据保留，避免误删。
新安装在向导中分别选择「启用灯光控制」与「启用 BIOS／风扇／来电恢复控制」，默认均不勾选。
root 日志服务在网页启动前校验安装请求并原子保存到 root 所有的 `policy.json`；不需要 SSH 或手动修改配置。
升级默认保留已有授权。需要更改时，在升级向导勾选「采用以下授权选择」，再分别选择两类控制；两类都不勾选可撤销全部写入。
每个由 root 系统单元固定的 SPK 版本只消费一次请求，并将版本回执与策略一起保存；普通套件用户或网页不能通过重放请求在重启时重新授权。
同版本重装保留已有版本回执与策略，不重新消费选择；需要更改时使用更新版本的升级向导，或由系统管理员调整 root 配置。
普通套件用户和 Web 无权修改策略。授权仅允许后续主动发起对应操作，不会在安装、升级、重启时自动写入硬件。
机型检测、共享锁与上游的确认、回读仍然生效。自动曲线和定时开关机仍不开放。

## 构建

先构建 `linux/web-ui`，在仓库根运行：

```sh
python linux/scripts/build_spk.py
```

产物和 SHA256 写入仓库根 `dist/`。SPK 的 DSM 版本号为 `3.0.1-1006`，应用内部版本保持独立。
默认使用 DSM 的 `/usr/bin/python3`（至少 3.8），不依赖额外 Python3 套件；
官方 DSM 7.2.2-72806 / DS3622xs+ 固件中已确认该路径指向 root 所有的 Python 3.8.15。
可在构建时通过 `--python` 指定受信任的绝对路径。
安装前会检查 Python 版本和解释器路径的 root 所有权，拒绝使用套件账户可篡改的解释器。
硬件二进制仍是原有固定发布版，安装前要求 glibc >= 2.27；低于此版本会中止安装，必须先在上游准备兼容载荷。
目标 DSM 的动态链接器、库和机型仍须实测，不能只凭 x86_64 宣称兼容。
本轮已用上述固件的 glibc 2.36 动态加载器验证 LED CLI、ugreenctl 与 fand 的依赖解析，
没有执行硬件入口；不等于已完成硬件运行或所有 DSM 平台 ABI 验收。

## 安装与访问

在套件中心手动安装 SPK，在向导设置至少 12 字符的管理密码。Web 登录用户名为 `admin`。
安装密码和硬件授权请求的临时文件由普通套件用户在首次 Web 启动时删除，服务中只保存 scrypt 派生值。

用 HTTPS 登录 DSM，例如 `https://NAS地址:5001`，在主菜单 / 桌面点击「UGREEN工具箱」，
或从套件中心点击「打开」。管理界面在 DSM 内部窗口显示，首次输入套件安装时设置的密码。
HTTP 登录 DSM 时会提示改用 HTTPS，不把管理密码经明文网络发送。无需单独配置 HTTPS 域名或代理。

后台仍只监听 NAS `127.0.0.1:5088`。DSM nginx 将 `/ugreen-toolbox/` 转发到后台，
覆盖代理标记与协议头，并剥离 DSM 登录 cookie，只传递本应用的会话 cookie。
浏览器会话使用随机令牌、Secure / HttpOnly / SameSite=Strict cookie，路径限于本应用，最长一小时。
关闭窗口时尝试注销；Web 重启后需要重新登录。浏览器不保存套件密码。
只允许 DSM 同源 iframe 嵌入与同源写入；容器版本仍禁止 iframe。

此处是 DSM 桌面集成，尚未接入 DSM 账户单点登录。菜单仅向管理员展示；实际 API 始终独立校验套件密码或会话，
不能把菜单可见性当作后端授权。默认 `public_origin` 留空，由本机注册的 HTTPS 代理确定 origin；
管理员若主动固定它，应使用实际 DSM HTTPS origin（含实际端口）。

## 一键导出日志

在「设备与高级 → 应用诊断日志」点击「一键导出日志」，下载 ZIP：

- `application/app.log` 及现有 `.1`–`.10` 轮转历史，每个文件最多最后 256 KiB。
- `services/root-startup.log`：只收集本套件硬件、只读日志、Web 及套件控制单元的 journal，最多 2000 条 / 1 MiB 原始输入，包含可用的 root 服务启动失败记录。
- `manifest.json`：记录来源、缺失、拒绝读取及截断状态；日志不可用时页面提示导出不完整。

独立日志服务以 root 读取固定文件和固定单元，不访问硬件，不给 Web journal 或数据目录权限。
日志服务只有 CHOWN / DAC_OVERRIDE，没有 SYS_RAWIO；通过 Unix socket 的真实 UID 验证请求。
导出需要套件登录和 DSM HTTPS 同源 POST，不能指定文件路径或命令，也不会清空日志。
包内不包含配置、认证文件、环境变量、整机 journal 或其他服务日志。

导出时再次脱敏密码、令牌、授权头、Cookie、私钥及敏感多行记录，遮盖常见 IP、MAC、URL 主机、
邮箱、共享目录、用户名及设备标识。自由文本仍可能包含无法识别的个人信息，分享前请复查。
截断尾部从完整时间戳记录开始，丢弃不完整尾行，避免暴露被截断的敏感续行。

依赖 DSM journal 已保留的记录；系统清理过的历史无法恢复。如果 Python、封存启动器或只读日志服务本身
无法启动，页面导出也不可用，需要从 DSM 系统日志排查。隔离测试不等于真实套件中心验收。

## 验收限制

固件检查发现并修正了 1001 / 1002 的用户级 synosystemctl 启动错误及不存在的 is-active 命令；应使用 1003 或更新版本。
固件证据与测试边界见 [离线检查记录](../docs/validation/2026-10-28-dsm-firmware.md)。

待实机确认：套件格式与安装向导、DSM 桌面组件与 nginx 资源生命周期、安装器生成的系统单元及实际启动 / 停止、其他版本的 Python / ELF ABI、卷权限 / Synology ACL、
开机顺序、升级和卸载、精确 DMI 与每项硬件能力。安装前不得省略这些检查。

设计依据：[DSM 权限要求](https://help.synology.com/developer-guide/breaking_changes.html)、
[系统单元注册](https://help.synology.com/developer-guide/resource_acquisition/systemd_user_unit.html)、
[套件结构](https://help.synology.com/developer-guide/synology_package/introduction.html)、
[DSM 应用窗口](https://help.synology.com/developer-guide/synology_package/package_tgz/launch_app.html)、
[官方应用示例](https://github.com/SynologyOpenSource/ExamplePackages/tree/main/ExamplePackage/ui)、
[Web Config 资源](https://help.synology.com/developer-guide/resource_acquisition/web_config.html)。
