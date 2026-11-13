# 媒体时钟审计与第一阶段交付（2026-11-11）

## 本阶段边界

实现纯时钟控制器、只读源桥接和长时间样本计数观测，构建 shadow 测试版。
`MEDIA_CLOCK_DISCIPLINE=1` 在构建时拒绝，防止误认为仅改 timing 就完成音讯补偿。
正式音讯／video／timing 的时钟和节奏保持客户 v18 行为。不是宣称已修复长时间破音。

原因：move-lag12 显示 HAL 与 RTOS 约 2918ppm 相对差异；同时 v18 已有音讯反馈调速。
先用同一采样点证明各时钟关系，再迁移生产／发送两个控制点，不能重复叠加 ppm。

## 实际时间路径

| 路径 | 核实到的实现 | 影响 |
| --- | --- | --- |
| `hal_read_curtime_us` | HP `CONFIG_TIMER_SCLK_FREQ=2000000`，直接返回 timer 计数，非 32K 左移分支 | 尚未量测物理速率，不能断定它比 RTOS 准确 |
| FreeRTOS tick | `ARM_RTL8195B/port.c`，SysTick reload 根据 SystemCoreClock/1000 设置，tickless 开启 | 需要观察 reload、实际 elapsed；尚不能排除漏 tick／tickless 误差 |
| `GetTickCount` | `lib_SystemLib.a(Window.o)` → `clock_gettime(1)` 即 CLOCK_MONOTONIC → RTOS 64-bit tick → 最终返回 32-bit ms | 不依赖日期更新；返回值约49.7天回绕 |
| `UpTicks` | `lib_CarPlay.a(TickUtils.o)` → GetTickCount，r1 清零 | 接收 clock 参考 RTOS 毫秒，非 HAL 微秒 |
| iPhone timing | `_TimingReceiveResponse+0x336` 调用 `AirTunesClock_Adjust`，参数 offset 为 ns | 被接收同步算法接受的调整；并非每个收到的 UDP 都有效 |
| 原厂同步时钟 | Adjust 对小误差进行渐进校正；reset 或超过约100ms的误差执行跳变，返回1 | 返回0不代表失败；不能把所有非零返回当成一般错误 |
| NearUpTicks getter | r0=clock, r1=16-byte time输出, r2:r3=UpTicks，内部 mutex | time.seconds +0，64-bit fraction +8；不解析 session 私有布局 |
| 车机 timing/video | v18 `AirPlayMediaClock`，HAL 微秒展开，统一整数秒 baseline | 已有共同起点，但尚未对齐 iPhone 的长期速率 |
| PCM producer | `AirPlayAudio_SendAudio`，GetTickCount，`ctx->deltaTime` 控制读取／生产节奏 | 原厂 feedback 会改变此值 |
| PCM sender | 输出线程普通路径 GetTickCount，导航专用 pacer 使用 HAL | 两个时间域需要一起审核，不应只替换 sender 的 sleep |

证据对象 hash 在 `src/carbox/media_clock/vendor_v18.json`。构建启用观测时检查五个对象，未知版本拒绝。ARM 原始反汇编保存在本次工作目录外的 `/tmp/clock-drift-audit`，关键指令与解释在此归档。

## 原有 feedback 不能忽略

v18 `AirPlayAudio.o` hash `acfb4ed06564cc3443dd7453101eb59a2550ebd9cbb45644a09a75207b67d43e`。
`AirPlayAudio_FeedBack`：

1. `+0x22..0x36`：累计 totalCount，前100次返回。
2. `+0x38..0x60`：再收集100次 rateAvgBox / rateAvgCar。
3. `+0x72..0xc6`：用均值与 clockSkew 计算新的 deltaTime。
4. `+0x108`：设置 lockFreq=1；后续 `+0x30` 直接返回，不持续更新。

所以长时差异可能还涉及“只校正一次后锁住”的频率关系，但这仍不是本次破音的已证明根因。
profile 读取的原生字段：deltaTime +688（每毫秒1000000单位，即每微秒1000单位），clockSkew +696，lockFreq +700，count +704，totalCount +708，rateAvgBox/Car +712/+716。
字段和统计快照在同一个短临界区取得；仅读取，不修改 feedback。

## 源采样的生命周期

- 包装 Adjust，原函数只调用一次，原返回值原样返回。
- 在 Adjust 调用者原有生命周期内，以 `UpTicks` 配对 HAL/RTOS，再调用 NearUpTicks；原厂 getter 的锁在临界区外。
- Adjust 事件最多每2秒记录一次，phase-step 不节流。原厂只接受不劣于之前8次 RTT 的样本，实际 Adjust 间隔可能超过20秒；不能把它称为固定2秒源采样，也不能把 getter 调用当成新的对时回复。超过10秒保守进入 HOLDOVER，source 斜率可能暂时不可用。
- 不保存给异步线程解引用的 clock/session 指针。owner 只用于身份比较。
- Finalize 先取消该 owner 的队列；同一地址复用生成新 generation，旧样本拒绝。
- 固定16条队列；满时记录 lost，清窗，重新获取有效窗口；不会跨过丢失的 step 事件继续拟合。
- 本机 HAL/RTOS 在既有 PC profiler 线程每2秒独立配对，iPhone 断线／RTT筛选不影响本机估算。完整统计仍每10秒处理有界队列并打印；无新增线程、无每包 printf。

## 连续模型与质量门槛

纯模块 `AirPlayMediaClockDiscipline.c`：LOCAL / ACQUIRE / TRACK / HOLDOVER；30～60秒窗口，至少10点；使用 residual 的最小二乘斜率，整数 ppb；±200ppm 试验范围；每实际秒最多2ppm；10秒过期冻结。
输出在改速率前以旧模型重锚定，保留不足1us的小数；不复制源绝对时间。
phase-step、明显纠相、旧 generation、过期／倒退／超范围样本拒绝；重新取得完整窗口后才能 TRACK。

±200ppm 只是控制器测试初值。本次实机 shadow 输出仍为0ppm；超范围估计会明确记录，不能把限幅当成修复。
固件尚未使用此模块替换客户公共时钟或 PCM 排程，因此不会对原有反馈形成第二个调速闭环。

## 用户补充：累计 sample 目标

共同媒体时钟可定义：`target_frames = anchor_frames + (media_now-anchor_media)*sample_rate/1000000`。
第一阶段记录生产、消费、sendto接受的64-bit frames，并从每个 stream/generation 的首个非空快照计算 expected HAL frames、accepted delta 和误差。导航／音乐分别记录；超过100ms发送间隔重新分段，日志给出 segment。该段误差不是车机已播放位置，暂停／欠载也不能当作晶振漂移。

后续正式闭环应把缓慢修正反馈到 PCM 读取期限，保持 sample count 与 RTP timestamp 语义。需要明确地替换／协调原生 FeedBack，而不是叠加；短暂落后不得突发补读或丢包追赶。车机播放进度仍需其 feedback 与 buffer 证据。

## 测试版日志

- `[MEDIA_DRIFT]`：shadow、实际 applied=0、source generation/owner、状态、窗口点数、source/HAL估计ppb、目标、拒绝／超范围、源年龄、step、队列丢失、getter最大耗时。
- `[MEDIA_PAIR]`：同一源样本的 HAL/RTOS/UpTicks/source、adjust_ns、flags、跨度、独立周期 HAL/RTOS 配对及其斜率；SysTick reload、core/tick/timer配置。
- `[AUDIO_SAMPLE_CLOCK]`：64-bit produced/consumed/accepted、分段期望与实际sample差。
- `[AUDIO_CLOCK_FEEDBACK]`：原生锁定状态、次数、deltaTime与三个float的原始bit pattern（离线解码，避免固件浮点格式开销）。
- 原有 `[AUDIO_TX] / [AUDIO_TX_NET] / [AUDIO_TX_RING]` 保留，累计byte扩成64-bit。
- 接收 jitter buffer／PLC 和车机实际播放位置未接入，明确 unavailable；不打印伪零。

## 交付与后续门槛

先测正常启动至破音，建议24小时并标注听到异常的时间。分析源质量、相对斜率、feedback锁定时刻与每段sample误差；确认真实参考及现有反馈作用后再交付 active。
不要为覆盖2918ppm而直接放大控制器限幅，也不要全局替换 GetTickCount、SysTick 或日期时钟。

## 本次构建验证

- build：`20261111211730_DBCE`；工作树 `/home/kevin/work/rt8715-clock-drift`。
- 参数：`MEDIA_CLOCK_DRIFT_PROFILE=1 PC_PROFILER=1 AUDIO_TX_EVIDENCE=1`；active 禁用。
- 镜像：`project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`，8298496 bytes。
- SHA256：`283df9262c2a3af4a05a5883c407349f42a978c557991c6d34835ddaabc9c2c6`。
- clean → 单线程强制 LP → IS 顺序完成；首次镜像打包因缺少 Python FATFS 依赖失败，补齐隔离依赖后重新构建成功。
- 最终 ELF 自动检查 receiver→observer→原函数、Finalize 生命周期和独立周期采样调用；避免仅有符号定义却没有实际调用。
- 已通过48小时正负漂移仿真、队列生命周期、生产桥接／稀疏 Adjust 回归、音讯计数回绕及原生 v18 ARM 时钟测试。
- 尚未在硬件长测；本版用于证据采集，不宣称破音已修复。

## move-lag13 后的 producer 修正（2026-11-12）

用户已授权先修等待溢出与原厂 feedback 突变。本次启用 `AUDIO_PACING_SLEW_FIX=1`。
全局时钟控制器仍为 shadow；不能把 `MEDIA_DRIFT applied_ppb=0` 解读为音讯 producer 完全没有改动。

### 证据与改动边界

- v18 `AirPlayAudio_SendAudio+0x5e..0x62` 是 unsigned 差值与39比较。
  目标周期40148072时，累积lastTime可以领先当前tick；相减下溢会绕过等待。
- 只替换 StartAudio（原函数一次转送并在新流开始前失效上一代）和 SendAudio。
- 原厂 `AirPlayAudio_FeedBack`、100次预热+100次平均、目标计算与一次锁定均保留。
  `ctx->deltaTime` 保留原厂目标；独立 pacer 保存实际生效周期，避免先突变再回写的竞争。
- 音讯使用原先 RTOS 时间域；HAL/RTOS相对速度仍观测，未直接应用约0.285%的全局补偿。
- decoder、PCM内容、RTP样本递增、sender与导航输出pacer都保留。

### 渐进规则

- 目标验证范围为原厂40ms周期的±1%，本次40148072在范围内；越界记录rejected并保留上个有效目标。
- 实际周期每秒最多改变20ppm（每40ms基准周期最多800ns/秒）。本次目标约需185秒渐进到位。
  这是已有PCM反馈目标的执行斜率，不是尚未启用的全局时钟2ppm/秒补偿。
- 每次根据真实PCM frame数安排期限，保留除法余数；mono/stereo不改变frame时间含义。
- 期限使用64位展开tick；未来期限正常等待，32位自然回绕不跳过等待。
- 过期超过一个块的期限重新锚定，不补发整段停顿欠账；暂停不积攒调速额度。
- 每次sleep最多5ms，检查stop与generation；不会忙等或用已退出流的期限继续等待。
- 没有改变原厂一次反馈锁定机制。需要持续更新目标与统一共同时间线时，另做闭环迁移，不能叠加第二个调速器。

### 新证据

`[AUDIO_PACING]` 每10秒报告目标/实际周期、20ppm/s斜率、frame总量、generation、重锚定、越界、等待和退出。
与 `[AUDIO_SAMPLE_CLOCK]`、`[AUDIO_CLOCK_FEEDBACK]` 一起判断目标变化后实际节奏是否渐进、是否增加空读。
最关键的复测窗口为原厂第一次feedback调速前后，至少保留调速后5分钟，再做长测。

### 客户 C 代码迁移

可复用 `src/carbox/audio_pacing/AudioSamplePacer.c/.h`（不依赖SDK）。
`audio_pacing_fix.c` 是当前精确v18 ABI下的试验适配；客户正式实现应放进 `AirPlayAudio.c`：
在新流Start清状态，在SendAudio使用原生字段与pacer安排等待；保留FeedBack计算目标，移除原有unsigned等待循环。
不可把偏移读取桥接当成跨版本公共API。更换客户库后先重新核对hash/调用链，再移除临时wrapper。

### 本次构建与验证

- 版本：`20261112034539_2BA3`。
- 镜像：`/home/kevin/work/rt8715-clock-drift/project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`，8298496 bytes。
- SHA256：`ad05361871db5184ca1c740b25de30fc16770ac53a9ed0a92dbe0b3ab8a23922`。
- 在已完成顺序 clean/LP 构建的工作树中增量构建 IS，退出码0。
- 通过纯 pacer（含48小时正负目标切换）、生产桥接 UBSan、时钟桥接、音讯统计及原生 v18 ARM 回归测试。
- 最终 ELF 检查确认调用进入新 producer、保留原厂 feedback、Start转送及统计报告；原厂 producer 未留在最终镜像。
- 独立 review 未发现需要修正的重要问题。硬件调速过程与长时间缓冲状态尚待客户验证。
- 未 commit/push。
