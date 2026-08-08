# AirPlay Video Frame Handoff Copy 與 Buffer Ownership 調查

調查日期：2026-08-08

## 1. 結論摘要

目前 `AirPlayScreenReceiver` 收完並解密一張 video frame 後，交給
`ScreenThread` 的過程包含一次完整 frame copy：

```text
AirPlayReceiverSessionScreen_ProcessFrames
  malloc(encrypted body length)                 source buffer
  exact-read TCP body into source
  decrypt/authenticate source in place
  ScreenStreamProcessData(source, frame length)
    DecoderRender(source, frame length)
      application send-video callback
        AirPlayScreen_SendVideo(source, frame length)
          malloc(frame length)                  destination buffer
          memcpy(destination, source, frame length)
          CVector_push_back({destination, frame length})
  free(source)

ScreenThread
  CVector_at(0) -> {destination, frame length}
  CVector_erase(0)
  AirPlayScreen_SendScreenFrame(destination, frame length)
    classify H.264 NAL/config frame
    build 128-byte screen protocol header
    malloc(128 + frame length [+ 16-byte authentication tag])  wire buffer
    memcpy(wire buffer, header, 128)
    memcpy(wire buffer + 128, destination, frame length)
    encrypt payload in place and append authentication tag
    lwip_write(screen socket, wire buffer, wire length)
    free(wire buffer)
  free(destination)
```

因此：

- queue 本身只複製 8-byte `{pointer, length}` metadata。
- enqueue 前仍會複製完整 frame；50 KiB frame 就會搬移約 50 KiB。
- source 在同步 callback 全部返回後立即由 receiver 釋放，不能直接留給
  `ScreenThread` 使用。
- 這個 `memcpy()` 已被 linker wrap；符合條件時由 linked GDMA 搬移，否則走
  M33。這是「以 GDMA 執行 copy」，不是 zero-copy。
- `ScreenThread` 不直接 decode handover buffer；它又把完整 payload 複製到新的
  wire buffer，原地加密後經另一條 screen TCP socket 送出。
- 因此 handover 後還有第二次 full-frame copy，接著 `lwip_write()` 內部另有
  wire buffer 到 TCP TX pbuf 的 segmentation copy。
- ChaCha 與 AES-CTR 的 hardware path 都允許 exact in-place operation；加密前的
  handover-to-wire copy 並不是硬體加密的必要條件，而是現有 sender 的 contiguous
  wire-buffer layout 與 ownership contract 所造成。
- 真正可優先消除的是 receiver-to-handover 與 handover-to-wire 兩次 full-frame
  copy，但必須同時重設 buffer ownership/headroom/tailroom；不能把 generic
  `memcpy()` 改成 no-op。wire-to-TCP-pbuf copy 則涉及 ACK/retransmission lifetime，
  在維持 `NETCONN_COPY` API 時不可直接刪除。

## 2. 分析依據與限制

相關 AirPlay implementation 來自 prebuilt archives，workspace 沒有其 C source：

```text
project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/lib_CarPlay.a
project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/lib_Accessory2.a
```

本文件依據目前完整 link 後的 ELF：

```text
project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/Debug/bin/application_is.axf
```

使用 symbols 與 ARM Thumb-2 disassembly 還原 ownership。下列 address 對應本次
build；重新 link 後 address 可能改變，但 instruction sequence 與 ownership
結論不受影響。

## 3. Source buffer 從哪裡來

`AirPlayReceiverSessionScreen_ProcessFrames()` 位於 `0x700c6a84`。其主要步驟為：

| ELF address | 行為 |
|---|---|
| `0x700c6bec` | 將 frame body length 傳給 `malloc()` |
| `0x700c6bf0` | 呼叫 wrapped `malloc()` |
| `0x700c6bf6` | 將結果保存為 source pointer |
| `0x700c6c18`–`0x700c6c28` | transport exact-read callback 將完整 body 讀入 source |
| `0x700c6c6c`–`0x700c6ca4` | ChaCha20-Poly1305 對 source 原地 decrypt/verify |
| `0x700c6cdc`–`0x700c6cf4` | AES mode 對 source 原地執行 AES-CTR update |
| `0x700c6ea2`–`0x700c6eba` | 呼叫 `ScreenStreamProcessData(stream, source, length, ...)` |
| `0x700c6ecc`–`0x700c6ed0` | `ScreenStreamProcessData()` 返回後 `free(source)` |

這個 source 是每張 encrypted screen record 配置的 heap buffer，不是永久 socket
buffer，也不是交給 consumer 後仍然有效的 shared buffer。其有效期截止於本輪
`ProcessFrames()` callback 返回。

### 3.1 為什麼 GDMA copy 完成後可以立刻 free source

目前 large memcpy GDMA helper 是同步介面：submit 後等待 DMA completion，完成
cache invalidate 與 edge copy 後才從 `__wrap_memcpy()` 返回。故呼叫鏈返回到
`ProcessFrames()` 並執行 `free(source)` 時，DMA 已經不再讀 source。

若未來把 frame copy 改成真正 asynchronous DMA，便不能沿用這個 free 時序；
source ownership 必須延長至 DMA completion callback。

## 4. Thread handoff 中的完整 copy

`AirPlayScreen_SendVideo()` 位於 `0x7015bc58`。反組譯可直接看到：

| ELF address | 行為 |
|---|---|
| `0x7015bc7a`–`0x7015bc80` | `malloc(frameLength)`，建立 destination |
| `0x7015bc82`–`0x7015bc86` | 在 stack 建立 `{destination, frameLength}` |
| `0x7015bc88`–`0x7015bc8c` | `memcpy(destination, source, frameLength)` |
| `0x7015bc90`–`0x7015bc94` | `CVector_push_back(vector, &item)` |

因此 queue handoff 並不是直接把 receiver 的 source pointer 傳給另一個 thread。
它先建立獨立 destination buffer，確保 caller 返回並釋放 source 後，
`ScreenThread` 仍可安全使用 frame。

application profiler 對相同 layout 的定義位於：

```c
typedef struct screenprof_frame_item_s {
    void *data;
    int bytes;
} screenprof_frame_item_t;
```

`screen_queue_profiler.c` 也確認 screen vector 的 `element_size == 8`，並只將
queue element 解讀為 `{frame pointer, frame length}`。這表示 `CVector_push_back()`
只需複製 metadata；完整 payload copy 發生在它之前。

## 5. Destination buffer handover 到 `ScreenThread` 後如何處理

`AirPlayScreen_ScreenThread()` 位於 `0x7015ba0c`：

| ELF address | 行為 |
|---|---|
| `0x7015ba94`–`0x7015baa4` | 取得 queue index 0，讀出 pointer 與 length |
| `0x7015baa6`–`0x7015baaa` | erase queue metadata |
| `0x7015bac2`–`0x7015bac8` | 同步呼叫 `AirPlayScreen_SendScreenFrame(pointer, length)` |
| `0x7015bacc`–`0x7015bad0` | screen socket 傳送路徑返回後 `free(destination)` |

停止 video 時，thread 也會逐項走訪尚未消費的 queue entry 並釋放其中的
destination pointer，避免正常 shutdown 遺漏 queued frame。

### 5.1 Handover buffer 不是直接交給 decoder

`AirPlayScreen_SendScreenFrame()` 位於 `0x7015b9e8`。它先確認 frame pointer、
length 與 screen session 狀態，再檢查：

```c
frame[4] & 0x1f
```

若 NAL type 是 7（SPS），便走 `AirPlayScreen_SendScreenConfigFrame()`；其他 frame
直接走 `AirPlayScreen_SendScreenNormalFrame()`。這裡的目的不是 local H.264
decode，而是把收到的 frame 重新封裝後轉送到另一條 AirPlay screen connection。

configuration path 會用 `GetH264SPSPPSIDRData()` 取得 SPS、PPS 與 IDR 範圍，先
建立並送出 configure record；若同一輸入含 IDR，之後仍會把該 IDR 交給 normal
frame sender。

### 5.2 Normal frame 會再做一次完整 payload copy

`AirPlayScreen_SendScreenNormalFrame()` 位於 `0x7015b850`。反組譯確認其流程：

| ELF address | 行為 |
|---|---|
| `0x7015b878`–`0x7015b8c4` | 在 stack 建立 128-byte screen header、timestamp 與 length 欄位 |
| `0x7015b8c4` | 計算 wire length；加密模式包含額外 16-byte authentication tag |
| `0x7015b8cc`–`0x7015b8d2` | `malloc(wireLength)` |
| `0x7015b8d4`–`0x7015b8d8` | 複製 128-byte header 到 wire buffer |
| `0x7015b8dc`–`0x7015b8e6` | `memcpy(wireBuffer + 128, handoverBuffer, frameLength)` |
| `0x7015b8ea`–`0x7015b8f0` | 對 wire buffer 中的 payload 原地加密/產生 tag |
| `0x7015b8fe`–`0x7015b90c` | 呼叫 wrapped `lwip_write()` 傳送剩餘資料 |
| `0x7015b93e`–`0x7015b946` | 傳送完成或出錯後 `free(wireBuffer)` |

這表示先前的 handover copy 並不是此資料路徑最後一次 payload copy。以 50 KiB
frame 為例，`ScreenThread` 還會再讀取約 50 KiB handover buffer、寫入約 50 KiB
wire buffer，之後才對 wire buffer 原地加密。

第二次 full-frame `memcpy()` 同樣會被 `-Wl,-wrap,memcpy` 攔截，因此符合
alignment、size 與 channel availability 條件時也由 linked GDMA 執行；128-byte
header copy 則低於 threshold，使用 M33。

### 5.3 `screen block` 的確切位置與 buffer lifetime

normal sender 會持續呼叫 `lwip_write()`，直到整個 wire buffer 都被 TCP stack
接受。若 `lwip_write()` 回傳 `-1` 且 `errno == EAGAIN/EWOULDBLOCK`，路徑會：

```text
print "screen block"
vTaskDelay(10 ms)
retry the same unsent wire-buffer range
```

因此出現 `screen block` 時：

- handover buffer 尚未釋放；
- wire buffer 也尚未釋放；
- `ScreenThread` 被同步卡在 outbound TCP backpressure retry；
- queue 中後續 frame 會繼續等待，直到本 frame 傳完或 socket error。

wire buffer 全部送入 lwIP 後才會先被釋放；`AirPlayScreen_SendScreenFrame()` 返回
到 `ScreenThread` 後，handover buffer 才會接著被釋放。

### 5.4 `ScreenThread` 如何把一張 frame 寫進 lwIP

是的，normal video path 會由 `ScreenThread` 同步呼叫
`AirPlayScreen_SendScreenFrame()`；後者建立並加密完整 wire record，然後以
`lwip_write(screenSocket, wireBuffer + offset, remaining)` loop 將這張 frame 寫入
screen TCP socket。一張 application frame 通常是一筆約 50 KiB 的 logical write，
但 `lwip_write()` 可能只接受其中一部分，所以 sender 會依回傳長度前進 offset，
直到整個 wire buffer 都被接受。

這裡必須區分四種邊界：

```text
AirPlay frame boundary       approximately 50 KiB
socket API write boundary    one or more lwip_write() calls
tcp_write boundary           limited by tcp_sndbuf() and 16-bit length
TCP segment boundary         normally approximately one MSS, about 1460 bytes
```

它們並不保證一一對應。一張 AirPlay frame 可以被多次 `lwip_write()` 接受，也一定會
被拆成許多 TCP segments；TCP stream 本身不保存 application frame boundary。

### 5.5 `lwip_write()` 到 `tcp_write()` 的呼叫路徑

socket API 的實際路徑是：

```text
lwip_write()
  -> lwip_send(flags = 0)
  -> netconn_write_partly(..., NETCONN_COPY, ...)
  -> TCP_IP task / lwip_netconn_do_writemore()
  -> tcp_write()
  -> tcp_output()
```

`lwip_write()` 本身只轉呼叫 `lwip_send()`。對 TCP socket，`lwip_send()` 固定加入
`NETCONN_COPY`，因此此路徑不是把 wire buffer pointer 直接交給 TCP/IP driver：

```c
write_flags = NETCONN_COPY | optional_flags;
netconn_write_partly(conn, data, size, write_flags, &written);
```

`lwip_netconn_do_writemore()` 再決定每次可交給 `tcp_write()` 的長度：

1. `tcp_write()` 的 length 是 16-bit，單次最多 `0xffff` bytes；
2. 若 `tcp_sndbuf(pcb)` 可用空間更小，先縮到目前可用 send-buffer bytes；
3. blocking write 在 send buffer 不足時等待 ACK/poll 釋放空間後繼續；
4. non-blocking write 可能 partial return，或在完全沒有空間時回
   `EAGAIN/EWOULDBLOCK`；此時 application sender 印出 `screen block` 並重試。

當本次 `tcp_write()` 成功，netconn layer 會呼叫 `tcp_output()`。因此
`lwip_write()` 成功只代表資料已被 lwIP 接受，並不代表 NIC/USB 已送完，也不代表
對端已 ACK。

### 5.6 `tcp_write()` 如何依 MSS 建立 fragments

`tcp_write()` 先決定此連線的 local segment limit：

```c
mss_local = MIN(pcb->mss, pcb->snd_wnd_max / 2);
if (mss_local == 0) {
    mss_local = pcb->mss;
}
```

一般 Ethernet MTU 1500、IPv4 header 20 bytes、TCP header 20 bytes 時，協商後的
`pcb->mss` 通常約 1460 bytes。若啟用了 TCP options，單段可放的 application data
為 `mss_local - optlen`，所以可能略小於 1460 bytes。

segmentation 有三個 phase：

1. 若上一個 `pcb->unsent` segment 的 oversized allocation 還有尾端空間，先填入；
2. 若允許 chained TX pbuf，嘗試把資料接到最後一個尚未送出的 segment；
3. 剩餘資料用下列 loop 建立新的 `tcp_seg` 與 `PBUF_RAM`：

```c
while (pos < len) {
    left = len - pos;
    max_len = mss_local - optlen;
    seglen = MIN(left, max_len);
    p = tcp_pbuf_prealloc(PBUF_TRANSPORT, seglen + optlen, ...);
    copy(p->payload + optlen, arg + pos, seglen);
    pos += seglen;
}
```

所以約 50 KiB frame 加上 128-byte screen header 與可能的 16-byte authentication
tag，通常會形成約 35 到 36 個 MSS-sized TCP segments/pbuf。實際數量會受 frame
length、negotiated MSS、TCP options，以及舊 `unsent` segment 是否仍有空間影響。

此處所說的是 **TCP segmentation**，不是 IP fragmentation。正常情況下每個 TCP
packet 都不超過 MTU，所以不需要再由 IP layer fragmentation。

### 5.7 wire buffer 到 TCP pbuf 是另一個已確認的 full-payload copy

`NETCONN_COPY` 最終成為 `TCP_WRITE_FLAG_COPY`。因此 `tcp_write()` 配置 stack-owned
`PBUF_RAM`，並把 encrypted wire bytes 複製進約 35 到 36 個不連續的 TX pbuf：

```text
one contiguous encrypted wire buffer
  -> many MSS-sized TCP TX pbuf payload regions
```

這個 copy 是目前 `[TCP_PERF] TX ... copy=... dma=...` 與 `[TXGDMA]` 所量測的
另一層，和 `AirPlayScreen_SendScreenNormalFrame()` 的 handover-to-wire copy 不同。

在目前 ownership contract 下，它也是 wire buffer 能在 `lwip_write()` 全部成功後
立刻釋放的原因。TCP stack 必須繼續保留自己的 pbuf，以支援：

- 尚未送出的 `pcb->unsent` queue；
- 已送但尚未 ACK 的 `pcb->unacked` queue；
- packet loss 時的 retransmission；
- remote cumulative ACK 到達後才釋放 segment/pbuf 並歸還 send-buffer accounting。

若改成 no-copy TCP write，就不能在 `lwip_write()` 返回後釋放 wire buffer；必須讓
整張 wire buffer 至少存活到所有對應 TCP bytes 都被 ACK，並新增 refcount/release
callback、partial ACK、retransmission、socket close/error 與 shutdown cleanup 管理。

### 5.8 TX linked GDMA 如何執行 segmentation copy

目前的 TX linked-GDMA path 在配置每個 TCP pbuf 時先記錄：

```c
block->src = wireBuffer + pos;
block->dst = pbuf->payload + optlen;
block->len = seglen;
```

所有 pbuf 配置成功後，再呼叫一次 logical copyv：

```c
carbox_linked_gdma_copyv_try(blocks, block_count, &result);
```

硬體一條 linked list 最多 16 個 descriptors，因此約 35 到 36 個 TCP pbuf 通常會
被 helper 拆成三筆 hardware transactions，例如：

```text
logical TX copyv for one approximately 50 KiB write
  hardware batch 1: 16 descriptors
  hardware batch 2: 16 descriptors
  hardware batch 3: 3 to 4 descriptors
```

每個 pbuf 中符合 address/length/cache-line 條件的 body 由 GDMA 搬移，無法納入
DMA body 的 prefix/tail edge bytes 由 M33 補齊。若 channel unavailable、program/
completion failure 或 validation failure，`tcp_write()` 仍保留完整 M33 fallback。

所以這裡是「一個 application frame、一次 logical scatter copy、數次 hardware
linked-list transactions」，不能把 application `lwip_write()` 次數、TCP segment
數量與 GDMA start 次數視為同一個數字。

### 5.9 Crypto in-place 能力與前置 copy 的真正原因

目前 ChaCha20-Poly1305 與 AES-CTR 路徑都能讓 input/output 指向同一塊 buffer：

| Crypto path | exact in-place 能力 | 目前 screen path |
|---|---|---|
| ChaCha20-Poly1305 | raw、chunked 與 combined backend 均有 in-place self-test；目前 board validation 也涵蓋多種 cache-line offset 與大於 64 KiB chunked input | 對 wire payload 原地 encrypt，RX 對 receiver source 原地 decrypt/verify |
| AES-CTR | `AES_CTR_Update()` 明確允許 source/destination 相同；底層 API 也接受分離的 source/destination | legacy screen path 對 frame buffer 原地 update |

因此目前 TX 的 50 KiB copy 不是為了讓硬體能夠 in-place，而是 closed sender 先建立
一筆可獨立持有、可修改且連續的 wire record：

```text
handover plaintext buffer
  -> malloc([128-byte header][payload][16-byte tag])
  -> copy plaintext into wire payload
  -> encrypt wire payload in place
  -> lwip_write the contiguous wire record
```

正常 exact-alias ChaCha path 不會在 crypto wrapper 內再複製相同 payload；其
`chacha_copy_bytes()` 在 source 與 destination 相同時直接返回。但也不能只把現有
caller 改成 `source != destination` 就宣稱消除了 copy：目前 ChaCha Mode 2 會先把
source staging/copy 到 destination，再對 destination 原地執行 hardware crypto。
既有 one-shot wrapper 也沿用這個 streaming staging 行為，尚不是直接由 hardware
完成 source-to-destination encryption 的真正 out-of-place one-shot API。

要移除 TX 這次 copy，有兩種語意完整的設計：

1. 新增真正 direct out-of-place crypto path：header 寫入 wire buffer，hardware
   直接由 handover source 讀取 plaintext、將 ciphertext 寫到 `wire + 128`，最後寫
   tag。ChaCha wrapper 必須確認沒有先做 software staging；AES-CTR API 本身已能接受
   分離的 source/destination。
2. 更完整的 ownership/layout 設計：producer 一開始就配置
   `[128-byte headroom][payload][16-byte tailroom]`，TCP receive/decrypt 直接落在
   payload，queue transfer 整筆 allocation；`ScreenThread` 只補 header、原地加密
   並附加 tag，不再配置或複製第二個 wire buffer。

RX 端同理：decrypt source 後的 source-to-handover copy 可以由「轉移已驗證 buffer
ownership」消除，或讓 out-of-place decrypt 直接寫進 final handover allocation。
無論採哪一種方式，都必須等 authentication tag 驗證成功後才能把 plaintext 發布
給 consumer。現有 in-place ChaCha path 一旦 hardware submit 後改寫 input，runtime
failure 時也不能用原 ciphertext 做 software retry，這是 ownership/API 設計必須
明確保留的錯誤語意。

這些改動都跨越 closed AirPlay library 的 allocation、crypto、queue 與 free 時序。
把某次 generic `memcpy()` 偽裝成成功但實際不搬資料，會讓後續 crypto/lwIP 讀到
未初始化的 wire payload，並不是保留語意的優化。沒有 upstream source 時，安全
選項只有維持 copy 並以 GDMA offload，或在明確 ABI 邊界完整替換 sender/ownership
API；不建議用彼此相依的 malloc/memcpy/crypto/free stateful wrappers 猜 call flow。

所以完整 buffer pipeline 是：

```text
Wi-Fi RX/TCP pbuf fragments
  -> receiver source buffer
  -> ScreenThread handover buffer
  -> encrypted wire buffer
  -> lwIP TCP TX pbuf/segments
  -> WLAN/NCM forwarding path
```

其中有四個已確認的 payload 搬移點：

1. fragmented TCP RX pbuf → contiguous receiver source；
2. receiver source → cross-thread handover buffer；
3. handover buffer → encrypted wire buffer；
4. `NETCONN_COPY` 路徑由 wire buffer → TCP TX pbuf。

完整 ownership 因此是：

```text
receiver owns source
  -> AirPlayScreen_SendVideo owns temporary destination allocation
  -> successful queue insertion transfers destination to ScreenThread
  -> ScreenThread reads destination to construct/encrypt/send a wire buffer
  -> screen sender frees wire buffer after lwIP accepts it
  -> ScreenThread frees destination after the synchronous sender returns
  -> receiver callback returns and frees source independently
```

## 6. 目前這個 copy 是否真的走 GDMA

linker 使用 `-Wl,-wrap,memcpy`，所以 closed library 中的 `memcpy()` 也會進入
`__wrap_memcpy()`。`AirPlayScreen_SendVideo()` 的 call site 在最終 ELF 中已指向
wrapped memcpy veneer，而不是繞過 wrapper 的 private implementation。

目前 `__wrap_memcpy()` 先呼叫：

```c
carbox_large_memcpy_gdma_try(destination, source, length)
```

必要條件包括：

- `length > 4096` bytes；
- task context，且 scheduler 已啟動；
- source/destination 的 address modulo 4 相同；
- 四個受管理 context（GDMA0/1 的 channel 4/5）至少有一個可用且正常。

成功時完整 cache-line body 由 GDMA 搬移，小量 prefix/tail 由 M33 處理；helper
同步等待完成後返回。若 alignment 不合、context unavailable 或 runtime failure，
則由 `__wrap_memcpy()` 完成整筆 M33 copy。

所以目前可準確描述為：

```text
logical full-frame copy: always present while queue is active
physical copy engine: GDMA when eligible, otherwise M33
```

不能因為已使用 GDMA 就把此路徑稱為 zero-copy。destination allocation、DRAM
read/write traffic、cache maintenance 與同步等待仍然存在。

### 6.1 50 KiB copy 的 linked-list 形態

contiguous memcpy 使用 four-byte transfer width。硬體單一 descriptor 的
`BLOCK_TS` 上限是 4095 transfers，因此每個 descriptor 最多搬：

```text
4095 * 4 = 16,380 bytes
```

一條 linked list 最多 16 個 descriptors，所以目前 helper 的單次 transaction
上限是：

```text
16,380 * 16 = 262,080 bytes
```

50 KiB frame 約需 4 個 descriptors，但仍是一次 transaction：

```text
build approximately four LLIs
  -> one GDMA start
  -> hardware walks all LLIs
  -> one completion IRQ
  -> one semaphore completion/wait
```

不是由 CPU 對每個 descriptor 分別 submit。只有單筆 DMA body 超過 262,080 bytes
時，helper 外層 loop 才會拆成多次 transaction，各自 start 並等待 completion。

整條 video forwarding pipeline 有兩次不同的 full-frame memcpy，所以一張 50 KiB
frame 正常會產生兩筆獨立 transaction，各約 4 個 descriptors：

```text
receiver source -> handover buffer
handover buffer -> encrypted wire buffer
```

兩筆 copy 發生在不同呼叫點與時間，可能取得不同的受管理 GDMA context。

### 6.2 已 GDMA 化後的 CPU 影響

只要 GDMA success rate 高，這兩次 copy 不會讓 M33 執行逐 byte/word 的 50 KiB
搬移，因此通常不再是主要 CPU hotspot。每筆 copy 的 CPU 工作主要剩下：

- claim/release 一個 persistent GDMA context；
- 建立/更新約 4 個 descriptors 與 channel registers；
- source/destination cache clean；
- 啟動一次 GDMA；
- task 以 semaphore 阻塞等待，而不是 busy-spin；
- 處理一次 completion IRQ；
- destination cache invalidate；
- 搬移不足完整 cache line 的 prefix/tail；
- 原有 `malloc/free` 與 queue mutex 操作。

以 30 fps、每幀兩次 full-frame GDMA 粗估，約為每秒 60 次 GDMA start 與 60 次
completion IRQ，數量不高。task 等待 DMA 時不消耗 M33 執行週期，scheduler 可讓
其他 ready task 執行；PC sampling 也不應把這段等待算成該 task 的 CPU hotspot。

但 logical copy 仍不是完全沒有成本：

1. **Pipeline latency**：receiver 或 `ScreenThread` 必須等同步 DMA completion 才能
   繼續；它不吃 CPU，但會延後下一階段與增加 queue age。
2. **Cache maintenance**：CPU 仍需處理大量 cache-line clean/invalidate，並非只有
   descriptor setup。
3. **DRAM bandwidth**：兩次 50 KiB copy、30 fps 等於約 3 MB/s payload copy，按
   每次 read+write 計算約 6 MB/s DRAM traffic，尚未包含 lwIP TX copy。
4. **Allocator/working-set**：source、handover 與 wire buffer 同時存在的期間會增加
   heap pressure、cache footprint 與 peak memory。
5. **Fallback risk**：alignment 不合、所有 context 忙碌、channel unavailable 或
   runtime failure 時，完整 copy 會回到 M33，CPU cost 便重新出現。

因此目前的判斷是：這些 copy 在 GDMA 正常命中時，對 CPU utilization 的直接影響
應該很小；剩餘影響主要是 wall-clock latency、cache maintenance、DRAM bandwidth
與 allocator pressure。除非 10 秒統計顯示大量 fallback、長 DMA wait 或 queue
backlog，移除它們不應高於 `TCP_IP`、WLAN RX、USB ISR/main task 與 outbound
`screen block` 的優化優先級。

## 7. `ScreenStreamProcessData()` 內的另一個條件式 copy

`ScreenStreamProcessData()` 位於 `0x700e20e0`。依 stream format，它可能直接將
source 交給 `DecoderRender()`，也可能配置 temporary repack buffer、逐個 H.264
NAL 呼叫 wrapped `memcpy()`，最後在函式返回前釋放 temporary buffer。

反組譯證據：

| ELF address | 行為 |
|---|---|
| `0x700e216c`–`0x700e2174` | conditionally `malloc(inputLength * 4)` |
| `0x700e2186`–`0x700e21f6` | parse NAL units 並將 NAL payload copy 至 temporary buffer |
| `0x700e21ae`–`0x700e21b0` | 釋放 temporary buffer |

這條 path 是否為目前 iPhone screen stream 的常態，僅靠單次靜態分支不能確定；
需用 caller-address/format profile 確認。它與本文件已確定的
`AirPlayScreen_SendVideo()` full-frame ownership copy 是兩個不同位置。

## 8. 哪些 copy 可以消除，哪些只能 offload

目前四個已確認搬移點可分類如下：

| 搬移點 | 現在是否必要 | 可行的無 copy 條件 | 現階段建議 |
|---|---|---|---|
| TCP RX pbuf fragments → contiguous receiver source | 對現有 socket `recv` 與需要 contiguous crypto/parser 的 ABI 而言必要 | 改為 scatter-aware parser/crypto，或讓 network stack 直接填入最終 frame allocation | 先保留；linked GDMA gather offload |
| receiver source → ScreenThread handover | copy 本身不具資料轉換功能，只用來延長跨 thread lifetime | queue transfer source ownership，並提供 release callback、enqueue failure 與 shutdown cleanup | 最值得由 library 正式 ownership API 消除 |
| handover → encrypted wire buffer | copy 本身不具資料轉換功能，只為建立 header/payload/tag contiguous layout 與可修改 ownership | frame allocation 預留 headroom/tailroom，或提供不先 staging 的 direct out-of-place crypto | 可消除，但需改 closed sender/layout，不能讓 memcpy no-op |
| encrypted wire buffer → TCP TX pbuf fragments | `NETCONN_COPY` ownership 下必要；TCP 必須持有資料直到 ACK/retransmission 完成 | 使用 no-copy TCP API，讓原 buffer refcount 維持到所有 bytes ACK/abort，並處理 partial write/close/error | 風險最高；目前用 linked GDMA scatter offload |

此外，`ScreenStreamProcessData()` 的條件式 H.264 repack copy 只有在 input format 需要
重排 NAL representation 時才有資料轉換價值。如果目前 stream format 已符合下游
要求，這條 temporary repack path 也可能不需要；必須先用 format/caller profile
確認實際命中率，不能只依反組譯靜態刪除。

### 8.1 為什麼 TCP fragment 尾端不應強求 zero-copy 或全長 GDMA

一般 IPv4/Ethernet MTU 1500 的 TCP payload 上限約 1460 bytes；IPv6 通常約
1440 bytes。實際 segment 也會受 negotiated MSS、TCP options、send-buffer 空間與
既有 `unsent` segment 尾端空間影響，所以 fragment size 不是絕對固定。

即使 destination payload 是 32-byte aligned，1460 也不是 cache-line 倍數：

```text
1460 = 1440-byte aligned body + 20-byte edge
```

安全實作應讓完整 cache-line body 由 GDMA 搬移，prefix/tail edge 由 M33 補齊。
TCP/IP header 是在 pbuf headroom 內 prepend，不要求 header 本身為 32/64-byte
aligned；真正限制是每個 descriptor 的 source/destination transfer-width alignment
及 cache maintenance 不得覆蓋相鄰 pbuf metadata。若 source/destination modulo 4
不同，則不能靠跳過相同 prefix 同時對齊，應整段使用 byte-width DMA 或 M33。

## 9. 若要真正 zero-copy，需要改什麼

不能只刪除 `AirPlayScreen_SendVideo()` 的 `memcpy()`，因為 receiver 會在 callback
返回後釋放 source，`ScreenThread` 將得到 dangling pointer。

安全的 zero-copy contract 至少要具備：

1. receiver 配置的 source 可被 detach，不再於 `ProcessFrames()` 尾端無條件 free；
2. send-video callback 可明確回報 ownership 是否已被 consumer 接受；
3. enqueue 成功時由 `ScreenThread` 最終釋放原 source；
4. enqueue 失敗、video stopped 或 parse/decrypt error 時仍由 producer 釋放；
5. shutdown 必須 drain queue 並釋放所有 transferred buffers；
6. 若 source 來自固定 pool，release callback 必須歸還原 pool，而不是直接 free。

例如可擴充為：

```c
typedef void (*video_buffer_release_f)(void *context, void *buffer);

bool send_video_take_ownership(
    void *buffer,
    size_t length,
    video_buffer_release_f release,
    void *release_context);
```

返回 `true` 表示 consumer 已接管，producer 不再 free；返回 `false` 表示 producer
仍持有並負責釋放。legacy `send_video(const void *, size_t)` 則繼續維持 copy
語意，以保持 ABI/API 相容。

目前 receiver、`ScreenStreamProcessData()` 與 `AirPlayScreen_SendVideo()` 都在
prebuilt library 中，沒有 upstream source 時，僅靠 application wrapper 強行偷取
source ownership 風險很高：下游 free 時序、error path 與 shutdown path 都會改變。
現階段保留原 ownership copy、讓 global wrapper 以 GDMA 加速，是較安全的部署；
真正 zero-copy 應由 library 提供正式 ownership-transfer API。

## 10. 後續量測建議

若要量化此 copy 還值不值得改，建議只加入低干擾的 10 秒彙總：

- `AirPlayScreen_SendVideo` calls/bytes，frame length avg/max；
- ownership copy 的 GDMA success、alignment fallback、busy/unavailable、runtime fail；
- GDMA wait total/avg/max；
- queue depth current/max；
- enqueue-to-dequeue age avg/P95/P99/max；
- `ScreenStreamProcessData` direct/repack calls 與 repack bytes。

這些數據能分辨剩餘成本主要是 DRAM copy、GDMA wait、NAL repack，還是 consumer
queue backlog；不需要逐 frame UART log。
