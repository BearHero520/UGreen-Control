# Linux / TrueNAS 迁移计划

## 已完成的项目初始化

开发位置：LLLED_FPK 仓库的 linux-app 分支，linux/ 目录。原独立仓库仅保留历史备份。

从 LLLED_FPK 已提交版本导入前端、应用适配及固定的 Almighty 运行文件。
前端构建目标调整为 app/www；硬件导入工具目标调整为 app/server。
旧 vendor 子模块、本地诊断文件、FPK、宣传素材和 fnOS 发布工作流未带入。

## 待完成

1. 统一应用路径与版本来源，移除 TRIM_*、/var/apps 和 fnOS manifest 的运行依赖。
2. 提供独立 HTTP 服务和登录会话，保护写入接口，配置同源策略和请求校验。
3. 将前端 CGI 地址改为独立 API 路由；关于页面和更新渠道使用本项目元数据。
4. 增加 Linux systemd 服务及持久化目录，不沿用 fnOS 启停脚本。
5. 提供 TrueNAS Custom App 镜像和 Compose，逐项配置设备、宿主机信息与持久化挂载。
6. 验证容器与宿主机间的锁、防止多实例争用，确认各机型驱动可用性。
7. 先验证监控，再验证用户授权的灯光、风扇与来电策略控制。

## 首版验收

- 普通 Linux 和 TrueNAS 分别完成安装、重启、升级与卸载验证。
- 未登录用户不能读取敏感配置或执行控制命令。
- 未识别型号与缺少驱动的能力明确不可用。
- 容器数据反映宿主 NAS，不误报容器自身信息。
- LED 始终调用随应用提供的 ugreen_leds_cli；BIOS 始终由上游 ugreenctl 执行。
- 记录实际验证的系统版本、NAS 型号及每项功能结果。

TrueNAS 部署参考：https://apps.truenas.com/managing-apps/installing-custom-apps/
