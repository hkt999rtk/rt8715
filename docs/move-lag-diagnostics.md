# 触控与画面延迟测试版

## 2026-11-11 发布状态

客户已验证 video 拖动及 audio 恢复正常。默认构建关闭 `MOVE_LAG_PROFILE`、
`SCREEN_SETUP_EVIDENCE`、`AIRPLAY_TIMER_CLOCK_PROFILE`、`AUDIO_TX_EVIDENCE`，
同时关闭本轮自动带出的 TCP/USB/HID 汇总；诊断源码保留，按需显式开启。
`SCREEN_SESSION_FIX=0` 保持不启用实验生命周期修改。

发布版保留 `AIRPLAY_TIMER_MONOTONIC=1`、`AIRPLAY_TIMER_SHARED_ORIGIN=0`、
`SCREEN_TIMER_ORIGIN=1`、`SCREEN_CURRENT_TIMESTAMP=1`：车机 timing 使用单调时钟
并保留原厂启动整数秒起点，video 使用同一起点和当前帧时间戳。音频 RTP 仍按
PCM sample 数递增，不改发送节奏。默认构建不要沿用下文测试命令中的 profile 参数。

### 客户 move-lag11 证据

- 固件 `20261111092608_48D8`，客户回报通过。
- baseline=2 秒，video_align=1，early/underflow=0/0；相对 session 误差通常 -7～-8us，
  全部样本范围 -289～-7us。相对 uptime 的 -2 秒是保留原厂起点后的预期值。
- video 954 帧，header 年龄最大 15.675ms，old20ms/future/back/repeat 全为 0。
- 音乐 stream100/44100Hz/4 bytes per sample frame，7,595 次发送全部被本机 stack 接受；
  seqbad/tsbad/framebad/headerbad/wirebad、发送 error/short 以及 ring 拒收均为 0。
- ring 采样峰值 13,704/100,000 bytes（约 77.7ms）；发送最大间隔 49.990ms、
  sendto 最大耗时 17.304ms。空读累计 1,050 次不能单独判定车机听到 underrun。
- 1 次画面 ChaCha 硬件超时 2.953ms，软件重试 recovered=1/tag_ok=1，下一次硬件成功。
  无 TearDown 或明确 rebuffer/lost-packet 记录。保留错误与恢复日志，关闭高频诊断统计。

这些日志支持本机时间基准与音频输出连续性正常，不证明网络末端零丢包。


### 安静发布版验证

- build：`20261111095042_10B3`，默认 `make -j8 ram_is`（未传诊断开关）。
- SHA256：`a2bae209b4dc5e97d286ec37aa865412b246eecb129884c5f140258ddf83a2d6`。
- 19 项相关测试通过；独立 review 修复了 timestamp 测试依赖旧 build cache 的问题，
  修改后的独立测试再次通过。
- 最终 ELF 确认 timing adapter、video baseline 换算、当前帧采时、4KB write/poll 保留；
  audio/media/move/HTTP/TCP wait 诊断 hook 和对应 log 字符串未进入映像。
- 此安静版完成编译及静态验证；客户已测通过的是前述带观测的 48D8 版本。

## 构建

基于当前主线及客户 v17 library；不是 move-lag1 中 95E7 固件的原样重编。
在 GCC-RELEASE 中按仓库规则先准备 LP，再执行：

```sh
PYTHONPATH=/tmp/rt8715-python-deps make -j8 ram_is MOVE_LAG_PROFILE=1 CROSS_COMPILE=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-
```

开关默认 0。测试开关自动启用 PC_PROFILER（复用其统计线程）、TOUCH_PATH_PROFILE，
以及原有 SCREEN_BLOCK_PROFILE、SCREEN_TCP_ACK_PROFILE、SCREEN_USB_PROBE、TCP_RECOVERY_TRACE。
这些子开关仍可用命令行分别覆盖为 0。
不自动调整 priority、bypass、合并 move、限速、4KB write、250ms poll 或 RTO 策略。
此观测仅支持当前 v17 原生发送路径，不与旧 CVector、direct crypto 等 profiler 混用。
派生 archive 只重定向 ScreenThread 的两个送帧调用和 AirPlayScreen.o 的 free 引用，
原生指令字节保持不变；free 仍调用原 free，不改变 buffer 所有权。

## 输出

每 2 秒由统计线程汇总。热路径不 printf、不分配观测内存。
32 条固定 ring 保存 >=50ms 的慢事件，每轮最多输出 8 条；满载累计 event_lost。
计时和计数仍有少量 CPU 开销，不能宣称零影响。

- `[MOVE_CFG]`：实际 HID bypass、move sampling、周期。build 版本见启动日志。
- `[MOVE]`：本地 at_us，video 待消费记录数/峰值/最老年齡、当前 frame ID/耗时、漏记数。
  pending_full/unmatched/peak/event_lost 是累计值，其余阶段计数按窗口重置。
  unmatched 包括入队后被释放的帧、地址复用及无法关联的消费；不能单独等同于网络丢帧。
- `[MOVE_STAGE]`：阶段次数、平均/最大耗时、>=20/50/100ms 次数；阈值计数互相包含。
- `[MOVE_SLOW]`：阶段、事件或 frame ID、开始时间、耗时、detail、result。
- `[MOVE_HIDQ]`：HTTP queue 年龄、slot_full、触控解析/HID/入队/写完序号。
- `[HIDHTTPBYPASS]`：bypass 排队、完成、读回覆错误、vendor overlap。
- `[MOVE_IO]`：video write 部分写入/EAGAIN/0/错误、poll 可写/超时/错误。

所有 start_us/at_us 均来自同一 hal_read_curtime_us；32 位约 71 分钟回绕，
区间用无符号差值。HID 序号和 video frame ID 是各自独立的编号，不代表因果关联。

| stage | 区间 | detail / result |
|---|---|---|
| hid_drain | 原 transport read 调用 | detail 最高位 1 表示已有解密明文，低31位为明文可读字节；否则为调用前 FIONREAD；result 为 transport 状态 |
| hid_queue | HTTP 入队到首次 write | detail 0=move/其他，1=down，2=up |
| hid_write | 单次 HTTPMessageWriteMessage | detail 同上；result 为状态 |
| dispatch_wait / dispatch_exec | callback 排队 / 执行 | HID 本地序号 |
| video_recv | ScreenReceiver 的 recv 调用 | detail 为本次接收字节；包含等待来源数据，静止画面时长等待正常 |
| video_arrival_gap | 两次 SendVideo 入口间隔 | 包含手机产帧、网络和本机前级处理；不能单独归因于网络 jitter |
| video_handover | SendVideo 调用 | detail 为帧长度；可能等待消费者腾出队列 |
| video_queue | SendVideo 入口到 ScreenThread 消费入口 | detail 为帧长度；包含生产者等待，不是纯 queue 驻留时间 |
| video_frame | 原 SendScreenFrame 调用 | detail 为 write 次数，result 为本机 write 接受的总 bytes（含封装）；不是车机显示完成 |
| video_write | 单次 lwIP write | detail 为请求字节数；result 为原回传值 |
| video_poll | 原 lwIP poll | detail 为第一个 fd 的 revents；result 为原回传值 |

video_recv 和 video_arrival_gap 只汇总，不占用慢事件 ring，避免静止画面挤掉 HID 证据。
HID slot/table 的记录不完整时会走现有处理；本次只观测，不修改该行为。

## 客户采集

保留从启动开始的完整 UART log 和同次测试视频。连接后等待配置行出现，
做一次滑动后停手数秒，再做连续滑动；保留停止操作后至少 10 秒日志。
先看 HID queue/drain/dispatch，再对照同一时间附近的 video_queue/frame/write/poll。
最终显示延迟还包括车机收包、解码和显示，本固件无法直接量到这些边界。

## HTTP request / response 归属观测（2026-11-07）

继续使用 `MOVE_LAG_PROFILE=1`。新增 `http_owner_diag.c/.h`，不改变 HTTP
发送路径、40ms drain、排队策略、回覆或 timeout。固定观测表放在 LPDDR，
复用统计线程输出。原厂已有的 `client->head != NULL` drain guard 保留：
所以 vendor_overlap 只表示两种路径时间上交叠，不直接证明抢读。

### 流程与可信范围

1. HTTPClientSendMessage 入队分配全局 request ID、连接 ID，记录 bypass/原厂、
   caller PC、method/path 分类、CSeq（若有）、body 长度。
2. 对 bplist00 body **只读取顶层 `type` 字段**作为 cmd；有界、无 heap 分配，
   不打印 payload、认证 header 或 URL 参数。不支持的格式打印 unknown。
3. 首次 HTTPMessageWriteMessage 建立发送顺序，记录完成/重试/错误。
4. bypass drain 与原厂 transport read 共用连接级旁观解析器。原厂 callback
   使用同步代理，原函数、context、buffer、返回值保持原样，不额外读取 socket。
5. 解析 Content-Length、chunked、trailer、分段及多回覆合并；1xx 不消耗请求。
   原厂一次预读多个回覆时，完成 FIFO 分别保留，直到原厂 parser 返回成功，
   才记录真正的 deliver。读取某个回覆不等于它已经交付给当前 request。
6. 请求按**首次 writer 调用顺序**对应 response，并在双方有 CSeq 时交叉检查。
   没有 CSeq 时只是 FIFO 推定；部分请求尚未写完就有另一请求开始时，标记
   invalid=23，停止可信配对。不会因为观测而改变业务。
7. 未知 framing（含 close-delimited）、过长 header、表满、混合读取或丢失边界
   都标记 invalid，停止该连接的流推断。不会搜索下一段 HTTP 字样强行恢复。
   HTTPClientInvalidate 时清理该连接；未经过该 hook 的对象生命周期变化会
   降低观测覆盖率，context 改变会标记 invalid。不能把失效观测解释成丢包。

### 输出

- `[HTTP_OWN]`：累计 lost/full/invalid、suspect/mismatch、delivered/guard、captures。
  `suspect` 是 response 被不同于 request 发送路径的 reader 读到；`mismatch`
  包含 CSeq 不符或实际交付 ID 不符，需结合 flags、invalid 和原始事件解释。
- `[HTTP_CONN]`：每连接累计 queued/written/responses、reads/bytes、尚未获得响应
  的 request 数量及 invalid 原因。包含该连接在首次 HID 之前已记录的消息。
- `[HTTP_EVT]`：同一时钟的 t(us)、连接 c、事件、path（1=bypass，2=原厂，
  3=混合）、id、peer、a、b、ret、flags、cmd。

正常事件保留最近 32 条。HID 使用过的 client 又有原厂 request，或者发现
配对异常时，复制历史并捕获后续 64 条。事件 ring 容量 256，每 2 秒最多
输出 64 条。重复历史可能出现，按 t/c/id/event 辨识；lost 非零表示明细不完整。
低频 summary 持续输出。HID 之外的连接默认不输出明细，但仍记录关联状态。

| 事件 | id / peer | a / b / ret |
|---|---|---|
| enqueue | request / caller PC（十进制，可转十六进制查 map） | CSeq / method<<16\|path分类 / body bytes |
| write_begin | request / 0 | CSeq / 发送顺序 / 0 |
| write_end | request / 0 | CSeq / 发送顺序 / writer 返回值（0=完成、11=稍后重试） |
| read | 原厂 reader request（bypass=0） / 0 | 本次明文字节数 / 0 / transport 返回值 |
| response | 按发送顺序对应的 request / 发起 read 的 request | 回覆 CSeq / 请求 CSeq / HTTP status |
| deliver | 原厂实际等待的 request / 流解析对应的 request | 回覆 CSeq / 0 / 0 |
| guard | 0 / 0 | 原厂 request queue 非空，bypass 暂停读 |
| invalid | 0 / 0 | 原因 / parser state / 0 |
| close | 0 / 0 | 累计 response 数 / invalid / 0 |

method：1 POST、2 GET、3 PUT、4 其他；path 分类：1 /command、2 /event、3 其他。
CSeq=4294967295 表示未取得，不能当作实际序号。

flags：0x01 路径不同（嫌疑）、0x02 CSeq 不符、0x04 实际交付 ID 不符、
0x08 混合读取、0x10 观测失去同步、0x20 没有对应请求、0x40 缺 CSeq 按 FIFO 推定。
只有在同步可信且请求覆盖完整时，交付错配才可作为直接证据；普通 overlap
和 read 的 reader ID 不同都不能单独认定业务错误。

### 验证

`src/carbox/tools/test_http_owner_diag.py` 使用 ASan/UBSan 主机 harness：
逐字节分段、合并响应、body 伪 HTTP、chunk extension/trailer、100 continue、
原厂预读与实际交付、would-block 跨调用、混合 reader、CSeq 错配、未知 framing、
过长 header、固定表与 ring 溢出、bplist type/截断/坏 offset、清理及 critical 外打印。
仍需客户实际测试 UART log，主机测试不能证明车机现场没有交付错配。


## move-lag3 后的 TCP / USB 复查

复用现有统计，不开启旧版逐帧 sender patch：

- `[SCREENTX]`：write/EAGAIN、持续阻塞、sndbuf、unsent/unacked、NCM queue/inflight。
- `[SCREENACK]`：ACK 前进、间隔、接收窗口、窗口右沿与协商参数。
- `[SCREENUSB]`：现有轻量 USB bulk OUT submit/error/complete/pending 计数。
- TCP recovery 事件：复用已有 RTO / 重传 / recovery 事件队列与统计线程。

MOVE 与旧 SCREENTX 共用一个 write wrapper，实际 lwip_write 只执行一次；
旧观测负责 TCP/NCM，MOVE 负责帧 ID / poll。MOVE 的 write 耗时包含观测开销，
SCREENTX 的 write_us 只测底层 write；阻塞时 TCP 快照最多约每 250 ms 采一次。
每 2 秒打印汇总，未启用逐包 UART。原有 4KB、250ms poll 和 RTO 参数不变。

SCREENUSB 的 complete 是观察到 URB 非 IDLE，不等于成功送达；该轻量统计汇总
bulk OUT，不能单独证明某个视频包送达或判断 ACK 的 USB IN 状态。
DWT 周期计数器会回绕，长时间卡顿应看 pending 与 TCP/NCM 进度，不能用
complete_us 推算数分钟的等待。详细 USB IN/URB 分类仅在这一版仍不足时再启用。

move-lag3 的 HTTP shadow parser 已发生 invalid=11；其 mismatch=0 不能排除
业务回覆错配。本轮只恢复传输层观测，HTTP 配对失效仍按 invalid 标识处理。

## 车机输入入口观测（CARRX）

MOVE_LAG_PROFILE=1 时同时开启，复用 2 秒统计线程，无逐包打印或 payload 输出。
原 CAREVENTRX 是 HID 匹配后的时序统计，不能当作所有车机消息的接收总数。

新增分层证据：

- `[CARRX] api=read/recv/recvfrom`：覆盖经过 linker wrapper 的 socket 接收调用，
  按 task/fd/API 记录累计 calls/done/data/bytes、zero/again/error、当前阻塞时长、
  最大调用耗时、最后返回值与 errno。data 是正数返回次数，**不是网络 packet 数**。
  不同 TCP read 可能拆分或合并消息，不能直接与 HTTP/HID 消息数相减判断丢包。
- `[CARRX] api=connect/select`：只观察车机 event 的 TCPClient 线程。
  connect 的 zero 表示返回成功；select 的 zero 表示超时、data 表示就绪返回。
  active=1/age_us 持续增加且 read 不增长，说明该线程仍在等调用返回。
  select 只在单个读 fd 时记录该 fd，多 fd 时记 -1，避免猜测。
- `[CARRX] api=http`：所有经过 HTTPMessageReadMessage hook 的调用，按线程统计。
  data 表示完整消息（返回 0），again 表示待续，error 表示其他返回值；
  bytes=0，因为不把 HTTP/transport context 猜作 fd，也不重复统计 raw bytes。
- `[CARRX_TYPE]`：GetInfoType 返回后统计 body bytes 与 hid/other/unknown。
  客户函数成功返回 1，失败返回 0；失败时不读取输出字符串。
  last_type 是最近成功分类的有界类型名；未知分类时清空，不输出消息正文。
- `[CARRX_SUM]`：累计固定表使用量、full、嵌套 recv 去重次数。

read/recv/recvfrom 行也会包含手机、DNS、OTA 等流量，**不能把全部行相加称为车机输入**。
应先用 task 与 local/peer ports 对照 session 日志，再观察 TCPClient 与对应 socket。
ports/af 是报告时快照，addr_ret 非 0 表示查询失败；未连接 UDP 可能没有 peer。
这不是 USB iAP2 全协议抓包；未经过这些 socket/API 的路径不在覆盖范围。

同步 read -> recv -> recvfrom 只统计最外层，避免同一数据重复计数；
MSG_PEEK 如被上层使用，返回的 bytes 代表可见数据，不代表已消费。
固定 96 行放在 LPDDR，累计统计不随报告清零；full 非零说明证据覆盖不足。
同 task/fd/API 的 fd 重用会合并累计数，不用作连接生命周期 ID。
报告跳过无变化的空闲行，但 TCPClient 行保留心跳；等待没有数据并不直接表示故障。

验证：test_car_rx_diag.py 检查 read 返回值/errno、read 嵌套 recv 去重、recvfrom、
HTTP 待续/成功/错误、分类失败不读无效指针、跨报告阻塞状态、connect/select 和满表退化；
test_move_lag_diag.py 检查新增入口与既有 MOVE/TCP observer 组合仍保持 I/O 行为。

## move-lag6 后：TCP 窗口等待证据

仅增加观测，保持 USB screen RTO 50ms 下限、原重传/降窗规则、4KB write、poll、
HID bypass 与线程优先级。`tcp_pcb` 布局不变。

### 观测位置

- `tcp_out.c`：实际窗口不足的两个出口及 Nagle 阻止发送的分支。
- `tcp_recovery.c`：成功提交 segment、有效 ACK 前进、RTO due 事件。
- `tcp.c`：连接释放时结束未完成的观测，记为取消而不是恢复。
- `tcp_output_wait.c`：4 条固定连接记录、16 条慢等待明细，无 heap 分配，
  无逐包打印；`pc_profiler.c` 原统计线程每轮最多输出 8 条明细。
- `tcp_output_wait.h`：仅 `MOVE_LAG_PROFILE && TCP_RECOVERY_TRACE` 打开时编译。
  只跟踪已有 `usb_screen_rto` 标记的连接，不按端口猜测，也不修改 TCP 状态。

### 如何读日志

`TCPWAIT_CFG` 说明枚举和范围。`TCPWAIT` 是按连接累计统计：

- `episodes/sent/cancel`：开始等待、原等待数据成功提交、取消的次数。
- `checks/cwnd/rwnd/nagle`：实际阻塞检查次数；同一等待可多次检查，也可改变原因。
  不是 packet 数；cwnd 与 rwnd 可同时阻挡，同一检查会计入两项。
- `active/age_ms`：仍未恢复的等待及年龄；`max_ms` 是已结束等待的最大值。
- `rto_id`：最近一次 RTO due 的现有 TCPREC ID，不单独证明因果关系。
- `closed`：连接已释放，不是当前仍有活动 socket。

`TCPWAIT_BEGIN/END` 是等待结束后冻结的 >=20ms 明细，用 conn/episode 配对，
用设备 ms 与现有 TCPREC、MOVE 的本地时间对齐（不是 UART 打印时间）。

- BEGIN 的 seq/len、flight、need、cwnd/rwnd/effective、flags 是首次阻塞快照。
  `need` 是下一 segment 末尾相对 lastack 的距离；effective 是实际使用窗口，
  包含 Limited Transmit 临时额度，避免把本可发送的 segment 误记为阻塞。
- reasons 是整个等待的原因并集：1=cwnd，2=rwnd，4=Nagle。
- rto_id 是开始时最近的 RTO due，等待中有新的 RTO 会更新，需查看对应 TCPREC。
- END result：1=成功提交覆盖原等待 seq 的数据，2=队列为空，3=连接释放，
  4=后续发送已跳过原 seq（关联中止）。2/3/4 均不算成功恢复。
- 重传更早的 segment 不结束等待；拆分后成功提交原等待数据的第一部分会结束等待。
- `ack_progress` 仅统计等待期间有效推进 ACK 的次数；first/last_ack_to_end_ms
  是首个/最后一个有效 ACK 到等待结束的时间。4294967295 表示未观察到，0 是有效值。
- result=1 只证明 TCP 成功向下层提交，**不证明 USB 完成、车机 ACK 或画面显示完成**。

`TCPWAIT_QUEUE` 打印累计明细丢弃、连接记录替换次数。表/环固定容量，任何非零
丢弃意味着证据不完整。连接使用现有单调 TCPREC conn ID，无 PCB 指针留给打印线程。
重启后 ID 和计数重新开始，不跨启动比较。

### 客户测试

保留完整开机 log，确认新 build 与 TCPWAIT_CFG。连续滑动、停手几秒、再开始滑动，
录制同次视频。若方便同步采 USB capture，用 TCP seq/ACK 对照原始 OUT 和 ACK IN，
才能进一步区分车机延迟 ACK、传输丢失或本机 ACK 接收处理延迟。
本轮没有新增 USB IN 逐包追踪，不能仅凭 TCPWAIT 判断 ACK 在哪一端延迟。

验证：`tools/test_tcp_output_wait.py` 编译实际 C observer，检查 USB screen 范围、
窗口原因/临时额度、重复检查、重传/部分发送、ACK 时间、时间/序号回绕、取消、
固定表/环溢出、PCB 不被修改，以及关闭配置后宏不求值且不依赖 PCB 布局。
另复查最终 ELF 中的 tcp_output、tcp_recovery_sent/ack、trace_record、tcp_free 调用点。

## move-lag7 后：screen 生命周期修正与初始化证据

测试开关：`MOVE_LAG_PROFILE=1 SCREEN_SESSION_FIX=1`。后者默认关闭，仅对校验过的
v17 resource450 派生库启用；原始客户 archive 不变。实现位于
`src/carbox/screen_session.c`、`screen_setup_evidence.c`，挂接脚本为
`src/carbox/tools/patch_screen_session.py`。

### 修改范围

- 停止标志在每次 screen write 前检查；非空 write 返回 0 按错误结束当前发送。
- sender 改为可 join，旧线程退出后才关闭 socket；停止标志已清除、控制连接已断开时，
  仍执行本地清理。原生 FreeRTOS 任务的 pthread_self 为 NULL 不误判为等待自己。
- start/stop 串行；等待入队的生产者在锁内检查 generation，避免旧会话资料进入新队列。
- 配置帧的 SendScreenData 同样按最多 4KB 提交，EAGAIN 使用 250ms poll。
- 显式停止最多先等 2 秒，未退出则 shutdown，再等 2 秒；仍未退出则拒绝新 session，
  保留旧 fd，不强行 close/reuse。此限制不是 TCP 零窗口自动断开计时器。
- NAL 封装、加密、TCP RTO、HID 重试行为保持原样。看到多 NAL 本身不代表非法；
  此版只确认实际进入原 EncryptData 的数据组成，不据此重封装或丢帧。

### 观测

启动检查 `[SCREEN_FIX] ... packaging_changed=0 info_retry=0`。既有统计线程输出
`[SCREEN_EVIDENCE]`，包含设备 `at_us` 与按标签顺序排列的 `v` 字段：

| 标签 | 含义 |
| --- | --- |
| SESSION | action、generation、fd、error、state |
| INFO_REQUEST | 请求 ID、CSeq、kind、header 长度 |
| INFO_REPLY | 请求 ID、HTTP 状态、响应 CSeq、body 长度、请求 CSeq、match |
| INFO_PLIST | 解析成功、error、根对象 type、body 长度 |
| INFO_HID | key 存在、实际 type、预期 type、设备数量、error、typed lookup 成功 |
| NAL | 既有 frame ID、输入长度、NAL 数、3字节前缀数、前两段长度、type 位图、结构异常数 |

SESSION action：1=start，2=create，3=请求停止，4=清理完成，5=发送错误，
6=线程到达退出入口，7=无法安全完成清理。state 位：1=正在发送，2=持有 sender handle，
4=到达线程退出入口；最终仍由 join 确认退出。

INFO_REPLY match：0=无法判断、1=匹配、2=不匹配。CSeq 的 4294967295 表示未取得；
fd/error 以 uint32 输出，负数需按 int32 解释，例如 fd=4294967295 为 -1。
回复元数据不单独证明传输成功；需与原 HTTP 错误、plist 解析结果一起判断。
不会输出认证 payload 或密钥，不发额外 /info 请求。

`SCREEN_NAL` 为累计统计，扫描耗时 `scan_max_us` 用来检查观测本身开销。
逐帧扫描但不逐帧打印，NAL 明细最多 8 条；事件环固定 24 条，每轮最多输出 12 条，
`lost` 非零表示观测不完整。数据从 length-prefix 转为 Annex B 后，会在发送加密前
再次写入长度字段，因此需观察中间边界，不能仅假设输入输出字节始终相同。

### 验证与客户测试

主机测试覆盖取消、部分写入、write=0、poll 错误、停止后的清理、等待旧线程、
解析元数据且不改变输入；archive 测试检查指令字节保持不变、重定位数量、ABI 和
实际 hook 目标，拒绝重复 patch。硬件上的车机互操作和最终原因仍需测试确认。
请从开机开始保存 log 并录制同次滑动视频，优先检查 /info 的 HID key 是否存在、
类型/数量是否正确，再关联 TCPWAIT、SCREEN_NAL 和 SESSION 的时间。

## 回归反馈：六七次均无法操作（2026-11-07）

用户报告上一测试版完全无法操作。尚未取得该次完整 log，不能确认是 HID 无响应、
画面停止、系统挂起或既有问题；按可能的回归处理。
上一版 SHA256 为 `1f3c8b105851a4790a7ccf8c73a16aa67b0693ba56065ca59778466ebe6faff3`。

本次将行为修改与观测拆开，构建对照版：

```text
MOVE_LAG_PROFILE=1 SCREEN_SESSION_FIX=0 SCREEN_SETUP_EVIDENCE=1
```

此版不编译 screen_session.c，不替换 StartVideo/StopVideo/SendVideo/SendScreenData，
不替换 pthread 创建、detach、exit，不增加 lifecycle mutex/join/取消写入判断。
使用客户原有流程，保留此前 4KB/poll 方案和已有 MOVE/TCP/USB 观测。
新加的 /info 与 NAL 观测继续启用；扫描会增加少量运行开销，不能宣称完全无性能影响。
启动标识为 `[SCREEN_FIX] ... session_fix=0 evidence_only=1 vendor_lifecycle=1`。

派生 archive 测试已确认 AirPlayScreen.o 所有指令字节与基础 resource450 相同，
除 EncryptData 调用重定位到只读观察入口外，所有重定位目标保持一致，生命周期函数
及状态变量符号未替换。对照测试只能隔离行为修改，不能代替实机根因证据。

对照版建置与最终 ELF 检查通过：未链接 carbox_screen_write/poll/create/detach/
thread_exit 及生命周期替换；原始线程和 I/O 路径恢复，/info/NAL 观察入口存在。
flash_is.bin 为 8298496 bytes，SHA256：
`89be56f026370ad9b6715ab609161f466b50f9b385e89881fabb1a53033cfb9f`。
用户补充此前版本似乎不会卡死，进一步支持优先隔离本次行为修改；仍需同条件
A/B 及完整 log 确认。尚未收到本次故障 log，未宣称根因或实机修复已确认。
