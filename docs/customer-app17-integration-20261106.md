# carplay_app_17 集成与 wrapper 清理

## 原始库复核

输入：`~/carplay_app_17.rar`。逐对象比较 v16 与 v17 的 14 个 archive，只有 `lib_Accessory2.a` 内的 `AirPlayScreen.o` 改变，其余对象内容一致。

`AirPlayScreen_SendScreenNormalFrame` 的 ARM Thumb 反汇编证据（函数内偏移）：

| 位置 | 行为 |
| --- | --- |
| +0xbe / +0xc6 | 比较并限制每次 write 为 4096 bytes |
| +0xce | 调用 lwip_write |
| +0xd2～+0xde | 返回 -1 时读取 errno；EAGAIN 进入原来的 poll 分支 |
| +0x104 / +0x10a | poll timeout 为 250ms，调用 lwip_poll |
| +0x140 | 非 -1 返回值继续区分 0 与非 0 |
| +0x142～+0x14e | 返回 0 时打印并退出当前 frame，释放缓冲，不空转、不推进发送指针 |
| +0x150～+0x154 | 正数返回时按实际写入字节数推进指针、减少剩余长度 |

write 返回 0 的处理按客户交付版本保留，不新增 poll 重试。该异常路径会放弃当前 frame 剩余内容；它不是正常发送完成，也不能据此断言 TCP 会话已被重新同步。函数仍未增加发送循环内的停止标志检查。本次用户接受这项异常处理方式。

v16 已确认的原生功能在 v17 中保持不变：AudioOutput priority=5、ScreenThread priority=4；导航渐进 pacing；pop 前分配并复用 PCM 缓冲；converter PLC 检查和停止 decoder 后释放队列。参见 `customer-app16-review-20261106.md`。

## SDK 清理

- 停用旧导航 purge、playout、continuity 与 PCM pacing/output hooks，由客户原生库实现，避免双重调节和旧结构偏移访问。
- 不再覆写 AudioOutput 优先级，默认 AUDIO_OUTPUT_TASK_PRIORITY=-1，使用客户原生值 5。
- 删除 SDK 的 carbox_screen_tcp_write 实现及重定位替换，保留客户原生 4KB、USB RTO 和 poll。
- 删除对应写入统计和旧 wrapper 测试；旧诊断源文件保留供历史参考，但发行配置不编译、链接这些输出 hooks。
- 保留 screen block 日志静默小函数；它只替代 puts，不改变 write、poll 或发送节奏。
- 保留 AAC decoder router，并补齐 aacDecoder_GetStreamInfo 的 handle 路由；客户 PLC 查询不能直接把 SDK router handle 当成 FDK handle。
- Wi-Fi buffer return、USB TCP recovery、其余仍需要的 SDK 修正继续保留。
- 新增原始对象 hash 校验；未知库版本或旧导航补丁组合会拒绝构建，不能只放宽 hash 后继续套旧偏移。

## 文件与校验

原始库目录：`project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/`。

默认构建继续使用源码生成的优化版 FDK；客户包附带的 lib_fdkaac.a 已同步到本地，沿用仓库既有策略不纳入版本控制。

lib_Accessory2.a SHA256：

```text
f40c1fe70d8a578b912c94d3b7d8926b4c0a75201ff7f389c4cc824a7e396c85
```

测试与构建记录目录：`/tmp/carplay17-review/`。静态审查不等同于客户实机测试。

## 验证结果

- 完整串行 clean → ram_lp（-j1 -B）→ ram_is（-j8）构建成功。
- test_screen_tx_poll_patch.py：5 项通过，包含未知代码/重定位拒绝和原生 write/poll 保持不变检查。
- 最终 ELF：旧输出 wrapper 与 carbox_screen_tcp_write 不存在；原生 4KB/poll、AudioOutput priority=5、FDK GetStreamInfo 路由已确认。
- Wi-Fi A-MSDU 修正的最终 ELF ABI/返回路径校验通过。
- 未执行硬件播放测试，需客户实机确认。

固件：`project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`

SHA256：

```text
ff79bc5c9671f76db6e5f57cf14e0a3be2501de72f985e1871664070f5e3003e
```
