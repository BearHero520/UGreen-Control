# UGREEN-NAS-tool（Linux / TrueNAS 分支）

面向通用 Linux 与 TrueNAS Linux 版的绿联 NAS 硬件管理应用。

## 当前状态

本项目在 `BearHero520/LLLED_FPK` 的 `linux-app` 分支、`linux/` 目录中开发，不再使用独立仓库。
目前是迁移起点，尚不是可安装的 Linux / TrueNAS 成品。
已导入经过本地验证的 iOS 18 风格 React 界面、版本比较修复、灯光与 BIOS 应用适配，
以及固定版本的 Almighty x86_64 运行文件。没有复制 fnOS 打包或发布工作流。

当前 API、路径解析和部分界面仍保留 fnOS 假设，不能直接暴露或部署为独立服务。
尚未提供可运行 Docker 镜像、systemd 安装器或 TrueNAS 安装配置，也未通过 NAS 实机测试。

## 首版目标

- x86_64 Linux：独立 Web 服务、登录鉴权、可配置数据目录、systemd 生命周期。
- TrueNAS 24.10 及以上 Linux 版：Custom App / Docker Compose 部署。
- 按机型配置所需设备权限，验证宿主机遥测、驱动依赖和控制器锁。
- 平台独立的版本与更新渠道，不再指向 fnOS FPK 下载。

前端和应用适配在本分支维护；硬件能力由上游实现。BIOS/风扇/来电恢复走 `ugreenctl`，
LED 只走随应用提供的 `server/bin/ugreen_leds_cli`。

## 目录

- `web-ui/`：React 前端源代码及测试。
- `app/www/`：构建后的界面和静态资源。
- `app/server/`：应用策略、适配器、固定发布版程序与上游许可。
- `app/ui/api.cgi`：待迁移的 fnOS CGI API。
- `scripts/`：固定上游发布版的导入和校验工具。
- `docs/migration.md`：迁移范围与验收条件。

## 开发检查

从 `linux/` 目录执行：

```sh
cd web-ui
npm ci
npm run check
npm test
npm run build
cd ..
python -m unittest discover -s tests -p test_almighty_import.py
```

前端构建不表示服务已可部署。当前固定使用上游 Almighty `2026-10-27-n76-direct` 分支构建（源码 `aff7991`，资产 `7d36cc7`）。
N76 直控尚未通过本应用的兼容 LED 入口开放，现有驱动依赖仍保留。
需要重新导入硬件载荷时运行
`python scripts/build_ugreenctl.py`；当前公开资产按提交下载并验证 SHA256；旧私有 Release 才需要 `gh` 读取权限。

## 来源与许可证

应用源自 `BearHero520/LLLED_FPK` 的 `almighty` 分支；精确提交记录见 `SOURCE.json`。
应用沿用 AGPL-3.0，见 `LICENSE`；硬件组件各自许可保存在
`app/server/lib/ugreenctl/upstream/`。
