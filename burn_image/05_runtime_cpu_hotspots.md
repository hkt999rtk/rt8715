# CarPlay 畫面資料路徑 CPU 熱點調查

調查日期：2026-08-04

## 目的

分析目前 CPU 使用率最高的四個 task：

- `AirPlayScreenReceiver`
- `rtw_recv_tasklet`
- `TCP_IP`
- `ScreenThread`

本報告依據目前 workspace 的 source、link map、靜態 library symbol 與
反組譯結果整理。這四個 task 不是四個完全獨立的瓶頸，而是同一條
CarPlay 畫面收發路徑上的連續階段。

## 整體資料路徑

```text
Wi-Fi RX ring
    |
    v
rtw_recv_tasklet
    |  RX descriptor、802.11 validation/reorder、ring -> skb
    v
TCP_IP
    |  Ethernet/IP/TCP processing、socket receive queue
    v
AirPlayScreenReceiver
    |  lwip_recv、解密驗證、H.264 NAL parsing
    v
Video callback / frame queue
    |
    v
ScreenThread
       frame queue、重新加密、lwip_write
```

因此畫面流量增加時，四個 task 的 CPU 使用率同時上升是合理現象。真正
要找的是資料在哪一階段被重複處理、複製，或產生過多 per-packet / per-frame
成本。

## 1. AirPlayScreenReceiver

### 已確認的工作

`AirPlayScreenReceiver` 對應 CarPlay library 的 `_ScreenThread()`，主要會
進入 `AirPlayReceiverSessionScreen_ProcessFrames()`。反組譯確認包含：

1. `select()` / `lwip_recv()` 接收 screen stream。
2. 視 frame 大小配置接收 buffer。
3. ChaCha20-Poly1305 或 AES 解密及驗證。
4. 呼叫 `ScreenStreamProcessData()`。
5. H.264 NAL unit parsing。
6. 必要時把 NAL 資料重組為 Annex-B 格式。
7. 呼叫上層 decoder/render callback。
8. 釋放暫存 buffer。

這裡沒有 H.264 software decoding；`DecoderRender` 最後只是把 encoded H.264
資料交給已註冊的 callback。

### 可能的 CPU 成本

- socket pbuf 到 caller buffer 的 copy。
- ChaCha/Poly1305 或 AES。
- NAL scanning 與非 4-byte length 格式的重新包裝。
- per-frame `malloc()` / `free()`。
- video callback 將 frame 放入下一級 queue 時的完整 payload copy。

ChaCha 已使用 hardware mode 2，因此不能再直接假設 ChaCha 是目前最大的
成本。大於 64 KiB 的 chunked backend 仍可能使用 software Poly1305，但必須
以實際 frame 分布及分段計時確認占比。

### 建議量測

為以下階段各自累積 calls、bytes、total/min/max time，每 5 秒統一輸出：

```text
socket_recv
decrypt_auth
nal_parse_repack
video_callback
allocation
```

先量測再修改，才能分辨主要成本是 network copy、crypto、NAL parsing，還是
callback 後的 queue copy。

## 2. rtw_recv_tasklet

### 已確認的工作

`rtw_recv_tasklet` 並不是單純的 memcpy task。從 `lib_wlan.a` 的 symbol 與
反組譯可確認包含：

- RX descriptor/ring 處理。
- 配置 skb 並做 ring 到 skb 的 frame copy。
- 802.11 data/control/management frame validation。
- decrypt、MIC、PN/replay 檢查。
- fragmentation/reassembly。
- AMPDU reorder。
- A-MSDU 拆成 MSDU。
- 802.11 header 轉換為 Ethernet header。
- queue、drop 與 RX 統計管理。

### 現況

- `rtw_recv_tasklet` 熱點 code 已放入 ITCM。
- 大型 skb 到 lwIP pbuf 已有 RX zero-copy 路徑。
- `PBUF_POOL_BUFSIZE` 為 1600 bytes，正常 Ethernet frame 通常不需 pbuf chain。
- network GDMA copy 預設關閉，使用 CPU copy。
- 目前 build 使用原始 `lib_wlan.a`，不需要 WLAN archive patch。

因此現有 CPU 使用率不應全部歸因於 copy。Wi-Fi protocol validation、reorder
及每個 MPDU 的固定成本可能更重要，CPU 使用率很可能與 PPS、AMPDU 組成和
重傳狀況高度相關。

### 建議量測

- MPDU packets/s 與 bytes/s。
- AMPDU reorder 次數及平均聚合量。
- A-MSDU frame/subframe 數量。
- ring 到 skb 的 copy bytes。
- duplicate、retry、drop、reorder timeout。

driver 沒有 source，因此在取得上述統計前，不建議對封閉 object 再做高風險
redirect 或 replacement。

## 3. TCP_IP

### mailbox 實作

Wi-Fi RX 呼叫 `tcpip_input()` 時，因目前沒有啟用
`LWIP_TCPIP_CORE_LOCKING_INPUT`，流程會：

1. 配置一個小型 `tcpip_msg`。
2. 把 pbuf、netif 和 input callback pointer 填入 message。
3. 透過 FreeRTOS queue 把 message pointer 送給 TCP_IP task。
4. TCP_IP task 收到 pointer 後執行 Ethernet/IP/TCP processing。

mailbox 不會複製 packet payload；queue 中主要傳遞的是 message pointer。

### 已完成的優化

- checksum 使用 M33 inline assembly。
- 大型 Wi-Fi RX 使用 zero-copy pbuf。
- pbuf buffer size 為 1600 bytes。
- NAT 已關閉。
- lwIP 熱點 code 已放 ITCM。
- TCP SACK output 已關閉。

最近低流量統計例子：

```text
RX ops=18 bytes=522 total_us=1297 checksum_us=27
```

換算約為每個小封包 72 us wall time，其中 checksum 約 1.5 us，只占約 2%。
checksum 已不是首要瓶頸。

### 可能的 CPU 成本

- 大量 TCP ACK/小封包造成的 per-packet overhead。
- pbuf、TCP segment、tcpip message 的 allocation/free。
- TCP PCB、window、ACK、timer processing。
- FreeRTOS queue wakeup/context switch。
- Wi-Fi/USB ISR 時間被歸到當時遭中斷的 task。
- `LWIP_TCPIP_CORE_LOCKING=1` 使部分 TCP work 在 caller task 內執行，造成
  task CPU 歸屬與直觀資料路徑不完全一致。

`TCP_PERF total_us` 是 wall-clock 時間，會包含函式執行期間被其他 task 或
ISR 搶占的時間；它不能直接當成純 CPU cycles。現階段沒有看到 TCP_IP 內
另一個明確、低風險且能大幅下降 CPU 的單點。

## 4. ScreenThread

### 已確認的工作

`ScreenThread` 對應封閉 library 裡的 `AirPlayScreen_ScreenThread()`。反組譯
確認其 loop 會：

1. lock frame queue mutex。
2. 從 `CVector` 取第一個 frame，執行 `CVector_erase(0)`。
3. 配置新的 output buffer。
4. copy 128-byte protocol header。
5. copy 完整 H.264 payload。
6. 執行 ChaCha/AES encryption。
7. 以 `lwip_write()` loop 送入 TCP socket。
8. free output/frame buffer。
9. 每輪 `vTaskDelay(10)`。

上游的 `AirPlayScreen_SendVideo()` 也會配置新 buffer、copy 完整 payload，
再 `CVector_push_back()`。這一段若由 AirPlay receiver callback 呼叫，其 CPU
時間會計入 `AirPlayScreenReceiver`，不一定計入 `ScreenThread`。

### 明確可改善的架構

- 使用 semaphore/queue event 取代固定 10 ms polling。
- 一次喚醒後 drain queue，而不是每次只處理一個 frame。
- 使用固定 frame pool，減少 per-frame malloc/free。
- 使用 ring queue，避免 `CVector_erase(0)` 移動後續 entry。
- 重新定義 buffer ownership，減少 frame queue、encryption output 與 TCP
  pbuf 之間的完整 payload copy。

其中 event wakeup、ring queue 和 buffer pool 的風險較低；跨 library 的真正
zero-copy 需要確認 callback、encryption 及 socket 對 buffer lifetime 的要求。

## Copy benchmark 的解讀限制

先前 benchmark 顯示 1 至 1.5 KiB aligned CPU memcpy 約只需 2 至 3 us，這能
證明正常小型 aligned copy 不值得換成 semaphore/IRQ GDMA，但不能直接證明
整條 screen pipeline 的 copy 可以忽略，原因包括：

- video frame 往往遠大於 1.5 KiB。
- TCP payload 起點可能因 Ethernet/IP/TCP header 而不對齊。
- 同一個 frame 可能跨多個階段重複搬移。
- benchmark 的 offset-2 CPU copy 明顯比 aligned copy 慢。

因此應以各階段累積 bytes 與 cycles 判斷，而不是只看單次 memcpy latency。

## 優先順序建議

1. 分段量測 `AirPlayScreenReceiver`：recv、crypto、NAL、callback。
2. 分段量測 `ScreenThread`：queue、allocation/copy、crypto、`lwip_write`。
3. 加入 Wi-Fi RX 的 PPS、reorder/A-MSDU 與 ring-copy 統計。
4. 將 TCP_IP task cycles、wall time 和 ISR time 分開觀察。
5. 根據結果決定是否修改 frame queue/buffer ownership。

最可能產生實質收益的位置是 `AirPlayScreenReceiver` 到 `ScreenThread` 的
per-frame allocation、queue 與多次 payload 搬移。TCP_IP 已經完成多項直接
優化，目前不是第一個應繼續修改的部分；`rtw_recv_tasklet` 則必須先取得
packet/reorder 統計，才能在封閉 driver 的限制下做可靠判斷。

## 分析限制

- `lib_CarPlay_chacha_m33.a`、`lib_Accessory2.a` 和 `lib_wlan.a` 的部分程式沒有
  source，本報告對這些區段是依 ELF symbol 與 ARM 反組譯分析。
- task 百分比可能來自不同 firmware revision；應使用目前 DWT/tickless 版本，
  在固定畫面內容與穩定 Wi-Fi 狀態下重新擷取同步統計。
- 沒有 phase timing 前，不把任何單一函式直接判定為最終瓶頸。
