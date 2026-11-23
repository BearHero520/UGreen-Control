# DXP4800 GT LED 多总线识别

2026-11-14：修复应用随包 `server/bin/ugreen_leds_cli` 的总线发现，状态为实验性、未实机验证。

## 输入与证据

- 用户镜像：`E:\工作台\固件\DXP4800 GT.img`，1,450,427,392 bytes。
- SHA-256：`ca2e91fa9e9d23a738b4689b8bb39f199cad60786253add81435559a48c27ff0`。
- UGOS Pro 1.21.1.0016，BUILD_TIME=20261103.082916；内核 6.18.15。
- `fw/usr/sbin/ug-load-drive.sh` 精确分支 `DXP4800 GT` 加载 `leds-mcu`。
- `kernel/usr/lib/modules/6.18.15/kernel/drivers/ugreen/leds-mcu.ko` SHA-256：
  `e2d4a762a6459d975018716ef9d5ff89dc5c71f5734c8aaa75d97f54e901ee5e`。
- 模块 `ug_led_driver_init`：枚举适配器，名称以 `SMBus` 或 `Synopsys` 开头时创建地址 `0x3a` 的客户端；调用 `i2c_smbus_read_word_data(client, 0x5a)` 并比较 `0xc5b2`。
- 模块 `leds_ugreen_probe` 精确匹配 DXP4800 GT，注册六个 LED；沿用已有 block-be 协议与灯位配置。
- 现场诊断的三个 PIIX4 适配器是 port 0 / port 2 / port 1；旧实现仅按前缀计数，因 `found 3` 在通信前拒绝。

提取目录：`E:\工作台\UGREEN-NAS-Hardware\firmware-inbox\work\dxp4800gt-ca2e91fa`。
原始镜像只读。提取物和临时构建不提交。

## 实现边界与行为

补丁：`App.Native.UGreenLED/app/server/vendor/ugreen_leds_controller/patches/dxp4800gt-multibus.patch`。
只修改隔离的 LED CLI 源码快照；不修改 UGREEN-NAS-Hardware 的 LED 实现，也不在应用脚本中访问硬件。

仅精确 DMI `DXP4800 GT` 且匹配 `dxp4800gt-block` 时启用：

1. 在原有身份识别、所有者检查、全局 LED 锁之后枚举候选。
2. 候选限于 `SMBus PIIX4 adapter ` 前缀及完整名称 `Synopsys DesignWare I2C adapter`；不探测 AMDGPU 或其他地址。
3. 核验 i2c-dev 字符设备及设备号，通过普通 `I2C_SLAVE` 请求固定地址 `0x3a`；只读取芯片 ID 寄存器 `0x5a`。
4. NACK 或不匹配 ID 视为未找到；占用、权限、超时、能力不足等错误拒绝继续，不能证明唯一性时不选择任何候选。
5. 必须恰好一个 `0xc5b2`，重新打开后复核身份；保留通用 SMBus 能力、状态帧、写前身份、ACK、回读和实验性确认检查。

没有固定 `/dev/i2c-N`，没有全地址扫描，没有 `I2C_SLAVE_FORCE`，发现阶段不写 I2C 数据。
候选最多 32 条，超出拒绝。其他机型仍走原有选择规则。

## 构建和验证

```powershell
python scripts/build_led_compat.py --dxp4800gt
python tests/run_dxp4800gt_runtime.py
python scripts/prepare_dxp4800gt.py
python scripts/build_ugreenctl.py
```

- 既有 DXP2800 EM26、iDX6012、通用 LED / N76 回归通过。
- 多总线选择测试覆盖不同枚举顺序、每条 PIIX4 总线为唯一目标、无目标、重复目标及致命错误。
- 生产 `find_bus` 的系统调用模拟覆盖实际读 ID、设备节点、占用、权限、超时、重新打开后 ID 变化、非 GT 路由；检查无写操作且文件描述符闭合。
- QEMU 执行最终 CLI 的 14 个 DMI／参数／初始化保护用例；没有硬件透传。
- 构建、运行测试和源码哈希记录在 `.work-almighty-release/dxp4800gt/`；导入保留此前所有 BIOS／风扇程序和插件的字节内容。

这些测试不证明实机 LED 已恢复。安装更新包后仍需用随包 CLI 的 `all -status` 确认选中的总线、芯片和六灯状态；实际写入效果另行验证。
本次不打包、不推送、不发布版本。

## BIOS／风扇检查

硬件审计保存在负责该控制面的上游 `docs/DXP4800GT_HARDWARE_AUDIT.zh-CN.md`。
原厂控制器为 IT55xx EC 路径，不能套用 DXP4800 Plus 的 IT8613 寄存器。
上游尚无 GT 风扇／来电恢复插件；本次未开放这些写能力。
