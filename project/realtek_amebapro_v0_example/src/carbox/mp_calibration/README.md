# Recovery MP 主线同步

来源：客户已测通的 Recovery SDK，FW2 版本 `20261106185922_D5EC`。
2026-11-06 客户确认 MP=4 完成回覆可用；此前仪器 RX 收到 979/1000，PER 2.10%，TX/RX 验证通过。

## 入口与状态

- Normal：`OTARECOVER` 清除 FW1 启动签名并重启到 FW2；无有效 FW2 时拒绝切换。
- Recovery 默认运行 AP/OTA；只有显式 `MP=4` 写入一次性标记并重启到 MP。
- MP 启动不创建 AP、STA_OTA、promisc 或自动重连任务。
- 单 STA 初始化后执行原厂 `mp_start`，核对实际 driver 状态。
- 成功后由命令任务输出 READY、`MP=4: OK`、`[MEM] After do cmd...` 和提示符。
- 初始化失败不会输出这组成功回覆。仪器可继续等待 MEM；超时应覆盖整次重启时间。
- `MP=STATUS` 查询状态；`MP=0` 重启回 Recovery OTA；`OTACLEAR` 恢复 FW1 签名返回 Normal。
- `MP=1/2/3` 保留 USB 加载、USB 卸载、I2C 测试。
- OTA 一旦开始，本次启动拒绝切换 MP。
- 不以 PHY 计数代替软件 Rx OK，也不通过直接写 driver 标志伪造 ready。

## 主线适配

主线与客户 SDK 不是同一分区/构建配置，故采用独立的 `GCC-RELEASE/application.recovery.mk`：
普通 `ram_is` 仍使用原来的 `application.is.mk`、CarPlay 库和性能配置。

| 项目 | 本主线 | 客户已测 SDK |
|---|---|---|
| FW1 | 0x40000，长度 0x370000 | 0x40000，长度 0x4C0000 |
| FW2 | 0x3C0000，长度 0x370000 | 0x500000，长度 0x180000 |
| FatFS | 0x7C0000 | 0x700000 |

**不能将客户 SDK 的固定地址 FW2 payload 直接写入本主线 FW2。**
本次保留主线分区；`ram_recovery` 生成对应主线地址的 Recovery，300 MHz。
两种固件的物件目录已隔离，但共享镜像/链接配置，切换构建必须清理，不可并行执行 Normal/Recovery 构建。

新增模块：

- `mp_calibration.c/.h`：原厂命令与真实 MP 状态检查。
- `recovery_mp.c/.h`：一次性标记、模式互斥、OTA 保护。
- `recovery_main.c`：Recovery 独立启动入口，console 1500000 bps。
- `recovery_atcmd_sys.c`：客户测试过的 Recovery 命令表。主线 NOR OTP 仅有只读接口，因此未移入客户额外的 NOR 擦写测试命令；MP 原厂 efuse/ioctl 命令保留。
- `recovery_ota_guard.S`、`../recovery_ota_stubs.c`：OTA 开始保护及 Recovery 兼容接口。

共享文件只同步所需集成：console 命令解析/回覆、AP 启动保护、MP 栈最小值、
格式化栈优化、Recovery 关闭 Ethernet/zero-copy、OTA 目标槽接口。
Normal OTA 仍按原来的 A/B 规则选择非运行槽。

## 库来源

以原厂 `component/soc/realtek/8195b/misc/bsp/lib/common/GCC/lib_wlan_mp.a` 为输入：
`cab792c356dfece332ea395784163a369144198917e95597564e00c0630c26dd`。
输入与客户已测库一致，Makefile 校验哈希；更换库后必须重新审查内部字段布局。

最终链接 `component/common/drivers/wlan/realtek/wlan_rx_gdma/build/lib_wlan_mp.a`。
由已有 `mp-fix` 规则生成：仅修改 `rtw_recv.o` 的 A-MSDU 分配失败及 child adapter 初始化，
添加 8 字节 helper；131 个原厂成员中，另外 130 个逐个校验不变。保留 MP 原始 RX/skbuff、16-frame pool、
MP 初始化及 RF 校准算法，不套用 Normal zero-copy 对象。链接后再检查 ELF 中修复分支。

额外保留客户 Recovery 使用的 `GCC-RELEASE/usb_lib/build/lib_usbsmart_mp.a`：
`78515eb498a8dd94c0435679f99afef4189ea23a37fa03f570ede9b72c835968`。
只由 Recovery 按需链接，不覆盖 Normal 的 USB 库。
此 archive 内 OTA/USB 实现仍是客户二进制依赖。

## 构建

在 `project/realtek_amebapro_v0_example/GCC-RELEASE` 执行：

```sh
tool_prefix=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-
make clean CROSS_COMPILE="$tool_prefix"
make -j1 -B ram_lp CROSS_COMPILE="$tool_prefix"
make -j8 ram_recovery CROSS_COMPILE="$tool_prefix"
```

打包工具需要 `pyfatfs` 和 `littlefs-python`。本机临时依赖位于
`/tmp/rt8715-python-deps`，可设置 `PYTHONPATH`。

Recovery payload：`application_is/firmware_is_ota2.bin`。
Recovery 单独构建的 flash 映像不包含 Normal FW1，不应当作完整 CarPlay 出厂镜像。
Normal 构建使用同一清理/LP步骤，最后改为 `make -j8 ram_is`。

## 验证与限制

- 已同步主机测试，覆盖标记消费、错误状态、重复 mp_start、环境失效、
  driver 返回成功但状态未就绪、console 解析及 MP=4 完成回覆。
- 本主线 Recovery 与 Normal 均进行编译/链接验证。
- 客户实测成功的是上述客户 SDK 版本；本主线地址适配版本还需板端确认。
- USB 实际枚举、仪器 Channel Set Error、校准数据保存/efuse 写入不属于本次回覆修复的实测结论。

## 同步后的复审

- Recovery 使用独立 `Debug/recovery_obj`，并确保 prebuild 完成后才编译/链接。
- OTARECOVER/OTACLEAR 先获取 flash mutex 再进入临界区，避免关中断后等待锁；申请扇区缓冲失败时不再误报成功或重启。
- 本次提交使用 MP 专用 A-MSDU 修补库，区别于客户上述已测通的原厂库版本，仍需进行板端回归。

本次验证：Normal/Recovery 干净构建通过；主机状态机、命令、完成回覆、切槽失败/锁顺序及 printf 锁测试通过。
Recovery ELF 中 driver 初始化传入的 skb 大小和应用检查值均为 2136。
修补 MP 库 SHA-256：`486bf56af6400a7ebe892d70bf8b832fa780d7bc370e8cf8fc02785503157959`。
