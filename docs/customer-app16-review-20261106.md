# carplay_app_16 原始库复核

输入：`/home/kevin/carplay_app_16.rar`。独立解包和反汇编记录：`/tmp/carplay16-review.zKqOs7`。此记录先记录原始库检查；随后已完成 v16 集成和 clean build，最终提交采用后续收到的 v17，详见 customer-app17-integration-20261106.md。

## 三项确认已进入库

均来自原始 `lib_Accessory2.a`，未套 SDK patch。

| 项目 | v16 直接证据 |
| --- | --- |
| 画面 4KB 分批 | AirPlayScreen_SendScreenNormalFrame +0xbe 比较 4096，+0xc6 把较大长度限制为 4096，+0xce 调用 lwip_write |
| 音讯输出优先级 | AirPlayAudio_StartAudio +0x220 为 movs r1,#5，+0x224 调用 pthread_attr_setpriority；ScreenThread 仍为 4 |
| 导航渐进 pacing | AudioOutputThread +0x18..0x3e 判断 type=101、48000Hz、2ch、16bit；启用独立微秒时间线与队列反馈控制 |

pacing 关键控制流已核对：

- +0x49a 起限制控制时间间隔，并以 80ms 参数平滑队列估计。
- +0x4cc 起以 50ms 为调节起点，目标加速上限 50000ppm，变化步长为 dt/10。
- +0x4fc 起实际库存小于 15ms 时取消加速。
- +0x598 起将追赶债务限制为 40ms。
- +0x5c2 起计算标称间隔的 2/3，限制追赶最快为标称包速率的 1.5 倍。
- +0x610 起等待会检查停止标志，并按微秒 deadline 判断；sendto 耗时纳入时间线。
- 包缓冲在 pop 前分配，分配失败等待重试；成功后复用，退出时释放。

因此 v16 确实包含此前交付的三项主要发送策略。旧 50ms 直接取消等待的分支仍供不满足上述导航格式条件的路径使用，不能仅看到常量 50 就判定新 pacing 未生效。

## 版本差异

相对 v15，lib_Accessory2.a 中只有 AirPlayAudio.o 与 AirPlayScreen.o 改变。lib_CarPlay.a 的所有对象内容相同，保留此前 converter/PLC 修正。FDK 库的八个变化对象仅构建日期时间字符串改变，代码及重定位 section 未变；SystemLibEx 的 Log.o 仅版本字符串改变。其他库的对象内容相同，archive 文件本身因重建仍可能 hash 不同。

v16 lib_Accessory2.a SHA256：

```text
af56736d7cd36d865170c035f2e4bbbec0ba8c80923a1259c96b52f51031d9bf
```

## 集成注意

不能把本次三项确认扩展为“所有参考代码均已迁移”：普通画面发送循环仍只检查 remaining，write 返回 0 仍没有独立退出路径。这两项防护尚未体现在该函数中。

新版输出函数布局已变，旧二进制补丁的对象 hash/偏移不再适用。后续集成应关闭已被原生实现替代的发送策略补丁，逐项保留或迁移其他必要功能，避免重复 pacing；不能只放宽旧脚本 hash 后照套。
