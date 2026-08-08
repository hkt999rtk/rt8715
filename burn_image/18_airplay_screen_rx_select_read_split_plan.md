# AirPlay Screen RX `select/read` 分拆調查與修改計畫

調查日期：2026-08-08

## 1. 結論摘要

目前 iPhone video 的接收者不是名為 `TCPClient` 的 task，而是
`AirPlayScreenReceiver`。封閉 library 中的
`AirPlayReceiverSessionScreen_ProcessFrames()` 以單一 loop 依序執行：

```text
select(video socket + control socket)
  -> 收滿 128-byte header
  -> malloc frame body
  -> 收滿完整 encrypted frame body
  -> decrypt/authenticate
  -> H.264 NAL parse/repack
  -> DecoderRender callback
  -> free
  -> 下一次 select
```

因此只有上述同步工作全部返回後，才會再次進入 `select()`。

建議的分拆方式不是讓兩個 task 分別操作同一個 socket，而是：

```text
Socket ingest producer                 Frame processing consumer
----------------------                 -------------------------
select(video + control)                dequeue completed record
read exact 128-byte header      ->     decrypt/authenticate in FIFO order
read exact encrypted body              H.264 parse/repack
enqueue owned frame slot                DecoderRender callback
immediately return to select            release frame slot
```

`select()` 和 socket `read()` 必須由同一個 producer 擁有。這樣才不會發生
兩個 reader 競爭 TCP byte stream、header/body 邊界錯位或 shutdown race。

不過，目前 lwIP 已經可以在 application 處理上一幀時預收約 256 KiB。分拆
不會讓 TCP 第一次具備預收能力；它的價值是更早把資料從 socket mailbox 取走、
更早呼叫 `tcp_recved()` 重開 receive window，並把下一幀收包與上一幀的解密、
解析及 callback 重疊。是否值得實作，應先用現有 profiler 確認 receive window
是否真的經常被吃滿。

## 2. 已確認的目前實作

### 2.1 Thread 與 library 位置

目前 screen receiver 位於：

```text
project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/lib_CarPlay.a
```

相關 archive members：

- `AirPlayReceiverSession.o`
  - `_ScreenThread()` 將 pthread 命名為 `AirPlayScreenReceiver`。
  - 建立/接受 screen data socket 後呼叫
    `AirPlayReceiverSessionScreen_ProcessFrames()`。
- `AirPlayReceiverSessionScreen.o`
  - 定義 `AirPlayReceiverSessionScreen_ProcessFrames()`。
- `ScreenUtilsStub.o`
  - 定義 `ScreenStreamProcessData()`，解析 H.264 NAL，必要時重新包裝。
- `AppleCarPlay_AppStub.o`
  - `DecoderRender()` 同步呼叫已註冊的 application callback；本身不做
    software H.264 decode。

名為 `TCPClient` 的 task 是另一條 AirPlay event/control connection，不是 video
data socket 的接收 thread。

### 2.2 `ProcessFrames()` 的實際 loop

反組譯確認 `ProcessFrames()` 會：

1. 從 `NetSocket` 取得 native lwIP descriptor。
2. 用 `lwip_select()` 同時監控 video socket 及內部 control/wakeup socket。
3. video socket ready 時，先透過 transport read callback 精確讀滿 128-byte
   screen header。
4. 從 header 取得 encrypted body length，配置完整 body buffer。
5. 再精確讀滿整個 body。TCP 是 byte stream，單次 `select()` ready 不代表
   header 或完整 frame 已全部抵達，因此 exact-read loop 不能省略。
6. 依 session cipher 執行：
   - ChaCha20-Poly1305：128-byte header 作 AAD，decrypt body，驗證尾端 tag。
   - AES：對完整 frame buffer 執行 AES-CTR update。
7. 呼叫 `ScreenStreamProcessData()`，再同步進入 `DecoderRender()` callback。
8. 釋放 body buffer，回到下一輪 `select()`。
9. control socket ready 時，另以最多 64 bytes 的 `lwip_recv()` 處理 stop/wakeup。

這證明目前接收、完整 frame 組裝、驗證及 callback 是同一個同步 pipeline。

### 2.3 `screen block` 與本題無關

UART 的 `screen block` 出自 outbound `AirPlayScreen_SendScreenNormalFrame()`：
當 `lwip_write()` 回傳 `-1` 且 `errno == EAGAIN/EWOULDBLOCK` 時，延遲後重試。
它不是 inbound receiver queue full，也不是 `select()` 尚未回去的直接證據。

## 3. 目前 TCP 可以預收多少資料

目前 video build 由
`component/common/api/network/include/lwipopts.h` 的
`CONFIG_VIDEO_APPLICATION` 區段設定：

| 項目 | 目前值 | 意義 |
|---|---:|---|
| `TCP_MSS` | 1460 bytes | 一般 Ethernet TCP payload 上限 |
| `TCP_WND` | `65535 * 4` = 262,140 bytes | 每條 TCP connection 的 receive window 上限 |
| `LWIP_WND_SCALE` | 1 | 支援 TCP window scaling |
| `TCP_RCV_SCALE` | 2 | advertised window 左移 2 bits |
| `DEFAULT_TCP_RECVMBOX_SIZE` | 600 entries | TCP netconn receive mailbox 深度 |
| `LWIP_SO_RCVBUF` | 1 | 支援 `SO_RCVBUF` |
| `RECV_BUFSIZE_DEFAULT` | 未覆寫，故為 `INT_MAX` | netconn receive byte accounting 預設上限 |

若 iPhone 在 SYN negotiation 接受 window scaling，最大 receive window 是
262,140 bytes，約 256 KiB；若對端不接受 scaling，lwIP 會限制在 65,535 bytes。

這裡必須區分「編譯設定」與「該 connection 的實際協商結果」：

- 編譯設定可由 source 確定為 262,140 bytes、local receive scale 2。
- `tcp_parseopt()` 只有在 iPhone 的 SYN 也帶 Window Scale option 時才設定
  `TF_WND_SCALE`，並把該 PCB 的 `rcv_wnd` 擴成完整 `TCP_WND`。
- 現有 `lwip_diag_tcp_buffer_state()` 回報的 `rx_window_capacity` 使用
  `TCP_WND_MAX(pcb)`，因此 `[BUFPROF] ... cap=262140B` 代表協商成功，
  `cap=65535B` 則代表未協商。這是確認當次實機連線的準確方式。

iPhone 在 SYN 宣告的 `snd_scale` 是另一個方向：它決定 box 傳往 iPhone 時可用
的 iPhone receive window；不要與 box 接收 iPhone video 所使用的 local
`rcv_scale` 混為一談。

以 50 KiB frame 粗估：

```text
262,140 / 51,200 = 5.12 frames
```

也就是 application 暫時不讀時，TCP 最多仍可先收約五幀，約等於 30 fps 下
170 ms 的資料。先前 10 秒樣本為 11,774,944 bytes / 298 frames，平均約
39.5 KiB/frame；以該平均值則約能容納 6.5 frames。

`SO_RCVBUF=INT_MAX` 不代表能無限預收。lwIP 的 TCP receive callback 不會因
`recv_bufsize` 拒絕已經 ACK 的 TCP data；實際正常背壓仍由 `pcb->rcv_wnd`、
可用 pbuf/memory 及 mailbox 決定。以目前數值看，256 KiB TCP window 會比
600-entry mailbox 更早成為主要限制。

application 每次從 socket 取走資料後，lwIP 才會透過 `tcp_recved()` 恢復可廣告
window。因此現行 loop 若長時間停在 decrypt、NAL parse 或 callback，資料雖會
先堆在 lwIP，但約 256 KiB 後 iPhone 仍會被 zero-window/backpressure 擋住。

## 4. 分拆真正能改善什麼

### 4.1 可能改善

- producer 在完整 encrypted record 收齊後立即回到 `select()`，不等待上一幀
  decrypt、NAL parse 或 callback。
- socket data 更早離開 lwIP receive mailbox，receive window 更早恢復。
- 下一幀的 Wi-Fi/TCP arrival 可以和上一幀的 hardware crypto、NAL parsing、
  outbound queue handoff 重疊。
- frame buffer 可改成固定 ownership pool，避免每幀 `malloc/free`。
- producer/consumer 各自的 latency 與 backlog 可以明確量測。

### 4.2 不會自動改善

- 不會消除 Wi-Fi driver、TCP checksum 或 socket-to-application copy。
- 不會改變一個 ChaCha frame 必須在 tag 驗證成功後才能交給 decoder 的安全
  邊界。
- 不會改善 iPhone 本身的 frame pacing jitter。
- 如果目前 256 KiB window 從未接近用滿，而且 processing 遠快於 frame arrival，
  增加 thread/queue 只會增加複雜度與少量 latency。
- 若 callback 內部本來就快速 enqueue 後返回，能重疊的只剩 decrypt/NAL parse，
  實際收益可能有限。

先前實機資料顯示 298 frames/10s、平均 arrival interval 33.306 ms，但 P95/P99
已達 57.046/76.324 ms。crypto engine 總 busy 約 7.9%，平均 lock wait 37 us，
並非長期飽和；不過少數 AES hold 可達 27.687 ms。這表示分拆可能降低部分
本機 tail latency，但不能僅靠靜態分析認定它會消除主要 jitter。

## 5. 建議的 target architecture

### 5.1 Ownership 模型

建議以兩個 bounded pointer queues 管理固定 frame slots：

```text
free_slots queue --(producer take)--> socket ingest
                                          |
                                          v
                                    ready_frames queue
                                          |
                                          v
                                  processing consumer
                                          |
                                          +---- release --> free_slots
```

每個 slot 至少包含：

```c
struct screen_rx_slot {
    uint8_t  header[128];
    uint8_t *body;
    uint32_t body_len;
    uint32_t body_capacity;
    uint64_t source_timestamp;
    uint32_t sequence;
    uint32_t first_byte_us;
    uint32_t complete_us;
};
```

以上只是 interface sketch，不是本輪要加入的程式碼。

所有權規則應固定為：

1. `free_slots` 中的 slot 只屬於 pool。
2. producer 取出後，只有 producer 能寫 header/body。
3. enqueue `ready_frames` 後 ownership 一次性交給 consumer。
4. consumer 驗證、callback 完成後才把 slot 放回 `free_slots`。
5. 不以裸 pointer 同時讓 producer、consumer 或 callback 持有。

### 5.2 Producer 工作

producer 是 video socket 唯一 reader：

1. 取得一個 free slot；沒有 slot 時以 queue/semaphore 阻塞，不 busy-loop、
   不 `vTaskDelay()` polling。
2. `select()` 同時監控 video FD 與原有 control/wakeup FD。
3. 收滿 128-byte header，驗證 type 與 body length。
4. 確保 slot capacity 足夠，將 encrypted body 直接讀進該 slot。
5. 保存原始 header、timestamp、sequence 與收包時間。
6. enqueue 到 `ready_frames` 後立即回到 free-slot/select 流程。

不要設計成 task A 只做 `select()`，task B 收到通知後才 `recv()`。在 task B
排程前，FD 狀態可能已改變；更重要的是 shutdown、partial header、partial body
及 errno 狀態會分散到兩個 owner，難以維持 TCP record 邊界。

### 5.3 Consumer 工作

只使用一個 ordered consumer：

1. FIFO dequeue 一個完整 encrypted record。
2. 依原 session 順序更新 AES counter 或 ChaCha nonce/sequence。
3. ChaCha 必須完成 tag verification，成功後才能把 plaintext 交給下游。
4. 呼叫既有 H.264 NAL parsing/repack 與 `DecoderRender()` callback。
5. callback 完全返回後釋放 slot。

不能用多個 consumer 平行解密 frame，除非 cipher record sequence、callback
ordering 與 presentation ordering 都有獨立且經驗證的重排設計；第一版不應做。

### 5.4 Queue 深度

第一版建議 3 slots，必要時再比較 2/3/4 slots：

- 1 個由 producer 填充。
- 1 個由 consumer 處理。
- 1 個吸收短暫 scheduling/crypto jitter。

4 slots 可多吸收一幀，但會增加 memory 與最大排隊 latency。queue 必須同時設
frame count 與 total bytes 上限，不能只靠 frame count。

目前 profiler 對 header length 的 4 MiB 判斷只是避免把普通 128-byte read 誤判
成 header，不能直接當成協議保證的最大 frame。實作前應先統計 P50/P95/P99/max
body length，再決定：

- 固定最大容量；或
- slot 首次遇到較大 frame 時 grow，之後保留 capacity；或
- small/large slab classes。

不建議一開始配置 `4 slots * 4 MiB`。

### 5.5 Alignment、cache 與 hardware

- body 起點至少 32-byte aligned。
- slot 必須位於 GDMA/crypto engine 可存取的 SRAM/DRAM，不能放在只有 M33
  可見的 DTCM。
- 若 body 經 DMA/crypto engine 寫入，consumer 讀取前依平台規則 invalidate。
- 若 CPU 建 descriptor/header 交給 DMA，啟動前 clean descriptor 與 source。
- metadata 與 DMA payload 不要共用 cache line，避免對 payload 做 cache maintenance
  時破壞 metadata。
- head/tail 不符合 hardware alignment 時可由 CPU 處理，中間 aligned body 再交
  hardware；所有 failure 必須保留安全的同步 fallback 或明確停止 session。

## 6. Backpressure、stop 與 error semantics

### 6.1 Pool full

pool full 時 producer 應阻塞等待 free slot。這不是丟 frame；TCP data 會繼續留在
lwIP，receive window 縮小後自然對 iPhone 施加 backpressure。禁止覆寫尚未處理
slot，也禁止為保持讀取速度而無界 malloc。

FreeRTOS queue/semaphore overhead 只發生在每個 frame 的 ownership handoff，約
30 次/秒，不是每個 1.4 KiB TCP pbuf 一次，成本相對很小。只有沒有 free slot
或 ready frame 時 task 才真正 block。

### 6.2 Stop/reconnect

現行 `select()` 同時監控 control socket，分拆後必須保留此能力：

- stop 要同時喚醒可能停在 `select()`、free-slot wait、ready-frame wait 的 task。
- producer 停止且不再擁有 socket 後，才能由 session close socket。
- consumer 需明確 drain 或 discard queued slots，再全部歸還 pool。
- reconnect 必須重設 frame sequence、crypto record state 及 queue generation。
- queue item 應包含 session generation，避免上一條 connection 的殘留 frame 被
  新 session 使用。

### 6.3 Error handling

必須逐一保持既有語意：

- `select/recv` 的 `EINTR`、`EAGAIN`、EOF 及 fatal error。
- partial header/body exact-read。
- body length/type validation。
- pool grow/allocation failure。
- ChaCha tag failure或 AES/crypto runtime failure。
- callback failure與 session teardown。

retry 不得讓 AES counter、ChaCha nonce或 frame sequence 前進兩次。auth failure
後的 plaintext 不得進入 decoder/callback。

## 7. 在目前 prebuilt library 下的可行方式

### 7.1 方案 A：修改 library source（建議的 production 方案）

直接修改 `AirPlayReceiverSessionScreen_ProcessFrames()`，將 socket ingest 與
processing 拆成兩個 internal workers。優點是：

- 可沿用真實 private structs、crypto state、control socket 與 error path。
- producer 可直接把 socket data 收進最終 frame slot，沒有額外 full-frame copy。
- ABI 與 session lifecycle 最容易維持。

這是應要求 CarPlay library/vendor 實作的目標方案。

### 7.2 方案 B：linker `--wrap` 完整替換 `ProcessFrames()`

目前 archive 中 `AirPlayReceiverSession.o` 對
`AirPlayReceiverSessionScreen_ProcessFrames` 是 undefined reference，而
`AirPlayReceiverSessionScreen.o` 提供定義，因此 linker `--wrap` 技術上可以攔截。

但 wrapper 若要真正分拆，必須重做整個 private function，包含 opaque object
layout、transport delegate、crypto state、time synchronizer、control socket、
callback 與所有 error/shutdown semantics。呼叫 `__real_...()` 無法分拆，因為
real function 仍是原本的同步 loop。

此方案只能在以下條件成立時考慮：

- library archive hash/version 固定。
- private layout 有完整反組譯與 runtime assertion。
- 有 reconnect、auth failure、partial frame、socket close 的 fault injection。
- 任何 layout/version 不符都 fail closed 回原路徑，而不是繼續猜 offset。

可行但維護及安全風險高，不建議直接作為第一個 production 版本。

### 7.3 方案 C：`lwip_select/lwip_recv` read-ahead shim

現有 `screen_queue_profiler.c` 已證明可用 linker wrapper 只攔截
`AirPlayScreenReceiver` 的 `lwip_select()`/`lwip_recv()`。理論上可讓另一個
producer 讀 real socket，再讓封閉 library 從 virtual staging FIFO 讀資料。

缺點是：

- 原 library 仍會 malloc 自己的 body，staging FIFO 到 library body 通常多一次
  full-frame CPU copy。
- 必須精確模擬 `select` readiness、partial read、errno、close、control FD 與
  socket reuse。
- 全域 socket API wrapper 的錯誤可能影響其他 connections。

它適合做效益 proof-of-concept，不適合作為最終低風險架構。

### 7.4 方案 D：只在 `DecoderRender` callback 後 enqueue

這是最安全的拆法，但只把下游工作移出 receiver；它無法讓下一幀 socket read
與 inbound decrypt/NAL parse 重疊。現有 bridge 已經有 outbound frame queue，
所以對本題的額外收益有限。

## 8. 實作前的量測與 go/no-go 條件

目前測試 build 已開啟完整 screen queue profiler，以同一個 10 秒窗口關聯
frame cadence、select/recv backlog 與 TCP window：

```text
SCREEN_TCP_BUFFER_PROFILE=1
SCREEN_QUEUE_PROFILE=1
SCREEN_FRAME_FORMAT_PROFILE=0
SCREEN_TIMESTAMP_PROFILE=0
```

它不枚舉所有 socket，也不依賴可能重用的固定 fd。wrapper 只在 task 名稱是
`AirPlayScreenReceiver`，且第一次 `lwip_recv()` request 大於 40 KiB 時，將該
`(task, fd)` 綁定為 video RX socket。綁定後在每次 128-byte screen header read
前取一次 PCB snapshot，因此小於 40 KiB 的後續 frames 也會被統計。reconnect
若換 task 或 fd，會在下一次大 frame 自動重新綁定。

每 10 秒只印四行 `[VIDRXWND]`：

- 綁定來源、task、fd、generation、sample/error 與 window scaling 狀態。
- pending bytes 與 receive-window available/used/capacity 的 last/min/avg/max。
- 下一個 ACK 將公告給 iPhone 的 receive window last/min/avg/max 與 zero 比例。
- window used >=75%、>=90%、full 的次數/比例及 profiler 自身耗時。

若只需要低負擔的 video RX window profiler，可改回：

```text
SCREEN_QUEUE_PROFILE=0
```

完整模式的既有 wrapper 能辨識：

- `select` wait/immediate/streak。
- <=64-byte control read。
- 128-byte header read。
- >128-byte payload read。
- recv calls/bytes/time。
- socket pending bytes、receive-window used/capacity。
- 完整 frame arrival、queue、prepare、socket handoff timing。

下一次實機測試應先收至少正常播放、快速畫面變化、reconnect 各 5 分鐘，10 秒
彙總以下資料：

| 類別 | 必要欄位 |
|---|---|
| TCP/window | negotiated scale、window capacity、used min/avg/max、zero/full count |
| Socket | pending bytes avg/max、select immediate ratio、header/body recv wait P95/P99 |
| Frame assembly | first-byte 到完整 body 的 P50/P95/P99/max |
| Processing | decrypt/auth、NAL parse/repack、callback 的 P50/P95/P99/max |
| Pipeline | next-select gap、frames/bytes、queue depth/bytes、oldest age |
| Failure | EOF、partial、auth fail、pool wait、drop、reconnect cleanup |

初始 go/no-go 判斷：

- **適合分拆**：receive window used 經常高於 75%，曾接近/full；或 processing
  期間 socket pending 持續累積，且 processing tail 與 window shrink 同時發生。
- **優先不拆**：window usage 長期很低、body recv 多數在等待網路，且 callback
  已快速返回。此時瓶頸較可能是 iPhone pacing、Wi-Fi transport 或下游 TX，增加
  producer/consumer 不會改善來源 arrival jitter。

75% 只是第一輪 decision threshold，不是協議限制；應以多個實機 scenario 的
分布調整。

## 9. 建議 rollout

1. **Measurement only**
   - 先收集預設的 `[VIDRXWND]`；需要時間關聯時再開啟完整 frame timeline。
   - 不改 socket ownership。
2. **Vendor/source prototype**
   - 先用 2 slots 驗證 lifecycle、ordering、auth 與畫面正確性。
3. **Bounded pipeline**
   - 比較 2/3/4 slots，選擇 latency 與 burst absorption 的平衡。
   - 導入 capacity-retaining pool，移除 per-frame malloc/free。
4. **Hardware/cache validation**
   - 驗證 aligned/unaligned、cacheable/non-cacheable、large I-frame、runtime
     crypto failure。
5. **Stress**
   - 反覆 connect/disconnect、network loss、partial frame、auth failure、stop
     while producer/consumer blocked。
6. **Production cleanup**
   - 保留低成本 10 秒 error/high-water counters，關閉逐筆 debug log。

## 10. Acceptance criteria

- 只有 producer 讀 video socket，沒有雙 reader。
- frame sequence、timestamp、cipher counter/nonce 及 callback order 全部保持。
- ChaCha auth failure 為 0；故障資料不交付下游。
- 沒有新增 full-frame CPU copy（方案 A 的要求）。
- queue/pool memory 有明確上限，steady state 不持續累積。
- stop/reconnect 不 deadlock、不 use-after-free、不殘留上一 session frame。
- 相同 traffic 下 receive-window full/near-full 次數下降。
- frame first-byte-to-callback 的 P95/P99 改善，且 presentation cadence 不退步。
- CPU、context switch、heap high-water 與 end-to-end latency沒有 regression。

## 11. 最終建議

先不要直接以 wrapper 重寫封閉 `ProcessFrames()`。目前 receive window 已有約
256 KiB，靜態分析只能證明「可以重疊」，尚不能證明 transport 正被同步 loop
卡住。

下一步應先收集 `[VIDRXWND]`，以 receive-window used/full 判斷是否確有
backpressure；若需要證明與 processing tail 的時間相關性，再開啟完整 profiler。
若數據確認 window 被同步 processing
耗盡，production 方案應由可取得 private source/layout 的 library owner 按本文件
的 single-socket-producer、ordered-consumer、bounded-slot-pool 架構實作；本地
linker read-ahead shim 僅適合作為短期 proof-of-concept。
