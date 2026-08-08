# CarPlay Video AES / ChaCha 加解密邊界與時序調查

## 1. 調查目的

本報告確認目前 CarPlay video bridge 在接收與送出畫面時，AES / ChaCha
究竟以 TCP segment、H.264 NAL，或完整 video frame 為處理單位，並整理這個
設計對 latency 與 jitter 的影響。

本報告所稱「一次」分為兩個層級：

- **Library/API 層級**：CarPlay library 呼叫一次 encrypt/decrypt API 的資料範圍。
- **Hardware/HAL 層級**：一次 API 呼叫內，實際提交幾次 crypto engine transaction。

兩者不可混為一談。完整 frame 可以是一個 AEAD record，但因 HAL 長度限制而
拆成多個 hardware transaction。

## 2. 調查來源

主要根據下列實際 linked object、source 與 runtime log：

- `carplay_app/lib_Accessory2.a(AirPlayScreen.o)`
- `carplay_app/chacha_m33/build/lib_CarPlay_chacha_m33.a`
  - `AirPlayReceiverSessionScreen.o`
  - `ChaCha20Poly1305.o`
- `carplay_app/chacha_m33/ChaCha20Poly1305.c`
- Runtime `[CHACHA][STATS]`、`[FRAMEPROF]` 與 `[SCREENQ]`

封閉 library 的行為以 linked object 反組譯確認；硬體 backend 的分流與限制則可
直接由本地 ChaCha source 確認。

## 3. 接收方向：iPhone 到 box

實際資料流程為：

```text
Wi-Fi / TCP byte stream
        |
        v
讀取固定 128-byte screen header
        |
        v
依 header 中的長度收齊完整 encrypted frame body
        |
        v
ChaCha20-Poly1305 decrypt，或 legacy AES-CTR update
        |
        v
ChaCha 路徑驗證 16-byte Poly1305 tag
        |
        v
AirPlayScreen_SendVideo(完整明文 H.264 frame)
```

### 3.1 ChaCha 接收路徑

`AirPlayReceiverSessionScreen_ProcessFrames()` 的實際呼叫順序為：

1. `chacha20_poly1305_init_64x64()`
2. `chacha20_poly1305_add_aad(..., 128)`
3. `chacha20_poly1305_decrypt(..., encrypted_length - 16, ...)`
4. `chacha20_poly1305_verify(..., tag, ...)`

`decrypt()` 的 length 是完整 encrypted frame body 扣除 16-byte authentication
tag，不是單一 TCP segment，也不是單一 H.264 NAL。

因此在 library/API 層級，一個 video frame 是一個完整的 ChaCha20-Poly1305
AEAD record，使用一個 tag 保護 header AAD 與整個 frame payload。

### 3.2 AES 接收路徑

Legacy screen cipher 路徑將完整 frame buffer 和完整 frame length 一次交給：

```text
AES_CTR_Update(context, frame_buffer, frame_length, frame_buffer)
```

因此 AES 在 screen library 的 API 邊界同樣是以完整 frame 處理。AES-CTR API
本身可以 streaming，但目前這個 screen path 並沒有在每次 TCP recv 後立即將
該小段交給下一層；它先組成完整 frame，再進行解密與 callback。

## 4. 送出方向：box 到 USB/NCM 另一端

實際資料流程為：

```text
AirPlayScreen_SendVideo(完整明文 frame)
        |
        v
完整 frame 放入 Screen queue
        |
        v
ScreenThread dequeue
        |
        v
建立 128-byte outbound screen header
        |
        v
對完整 frame 做 ChaCha20-Poly1305 或 AES-CTR
        |
        v
lwip_write() 將 encrypted record 分段交給 TCP
        |
        v
CDC-NCM / USB
```

### 4.1 ChaCha 送出路徑

`AirPlayScreen_EncryptData()` 的反組譯顯示：

1. `chacha20_poly1305_init_64x64()`
2. `chacha20_poly1305_add_aad(header, 128)`
3. `chacha20_poly1305_encrypt(frame_buffer, frame_length, frame_buffer)`
4. `chacha20_poly1305_final(..., tag)`

它以完整 frame length 呼叫一次 encrypt，並在 frame 後方產生 16-byte tag。
目前實作為 in-place encryption。

### 4.2 AES 送出路徑

如果 session 選用 legacy AES，`AirPlayScreen_EncryptData()` 將同一個完整 frame
buffer 和 length 交給 `AES_CTR_Update()`。也就是說 cipher 選擇不同，但完整
frame buffering 的架構相同。

## 5. Hardware backend 的實際分段

完整 frame 是 library/API 的 record 邊界，不代表只提交一次 HAL。

目前 ChaCha hardware backend 的主要限制如下：

- Hardware route threshold：`CHACHA_HW_MIN_LEN=4096`
- 單次 RTL8195B HAL message 上限：64 KiB
- 部分 HAL 路徑要求 length 為 16-byte multiple
- ChaCha counter 以 64-byte block 前進

因此：

| Frame / record 狀況 | Library API | Hardware 執行 |
|---|---|---|
| 小於 4 KiB | 一次完整 record | software |
| 不超過 64 KiB、符合 combined 條件 | 一次完整 record | 通常一次 combined HAL |
| 不超過 64 KiB、其他 layout | 一次完整 record | standalone ChaCha，Poly1305 依 backend 處理 |
| 大於 64 KiB | 一次完整 record | ChaCha 拆成最多 64 KiB chunks；整個 record 共用同一個 tag |

大於 64 KiB 的 `chunked-chacha+software-poly1305` 仍是同一個 AEAD record；只是
ChaCha payload 因 HAL 限制分段提交，Poly1305 在 software 對完整 ciphertext
計算。

## 6. Runtime 統計的交叉驗證

實機常見的 5 秒統計約為：

- 大型 hardware ChaCha operations：約 108 至 142 次
- Hardware ChaCha bytes：約 5 至 6.6 MB
- Video frame rate：約 23 至 30 fps，即每 5 秒約 115 至 150 frames
- 同期間 video bytes：約 5 至 6 MB

大型 hardware operation 數量及 bytes 都接近 video frame 數量及 video bytes，
符合「一個 video frame 對應一個大型 logical ChaCha operation」。

另外出現的約 200 至 280 個 software operations 多為較小的 audio、control 或
其他 protocol records；不能把它們解讀成同一張 video frame 被拆成大量 software
加密呼叫。

`[CHACHA][STATS] chunked=N/bytes` 計數的是 logical record/backend route 次數，
不是底層 64 KiB HAL transaction 的總數。

## 7. 對 latency 與 jitter 的意義

### 7.1 接收端無法直接把未驗證 plaintext 向下游交付

ChaCha20-Poly1305 在計算上可以 incremental decrypt，但 authentication tag 位於
record 尾端。在 tag 驗證完成以前，plaintext 尚未被認證。若直接把這些資料送給
decoder 或另一端，tag 最後失敗時已無法收回已使用的資料。

目前 library 選擇先收齊完整 frame、完成 decrypt 和 tag verify，再呼叫
`AirPlayScreen_SendVideo()`，在安全性與 API 語意上是合理的。

### 7.2 完整 frame buffering 會形成 burst boundary

目前 bridge 不是：

```text
收到一個 TCP segment -> 解密一段 -> 立刻轉送一段
```

而是：

```text
收齊完整 frame -> 解密/驗證 -> queue -> 再加密完整 frame -> TCP/USB 送出
```

因此每個 frame 至少存在下列等待：

1. 收齊完整 encrypted frame。
2. 完整 frame decrypt / authentication。
3. Screen queue scheduling。
4. Outbound buffer 準備與完整 frame encryption。
5. TCP 接受並逐步送出該 record。

這種架構會保留並可能放大 Wi-Fi/TCP 的 burst arrival 特性。它不代表 queue 一定
會持續累積，但會使每幀 latency 和輸出 cadence 更容易受到 frame arrival、task
scheduling 和 socket handoff 波動影響。

### 7.3 與 timestamp 問題的關係

若 outbound 使用 iPhone 原始 presentation timestamp，下游 jitter buffer 理論上
可以吸收部分 arrival burst。若原始 timestamp 在
`AirPlayScreen_SendVideo(data, bytes)` 邊界遺失，並由 outbound 端依本地收到或
送出時間重新產生，完整 frame buffering 所造成的 burst 便可能直接反映成播放
cadence jitter。

因此 crypto full-frame boundary 本身不一定是 bug，但它使 timestamp 保留與
retiming 是否正確變得非常重要。

## 8. 尚未由現有統計單獨量出的項目

目前 `[FRAMEPROF] prepare_us` 包含：

- outbound header 準備
- allocation / memory copy
- ChaCha 或 AES encryption
- 第一次 `lwip_write()` 之前的其他處理

它不是純 crypto execution time。現有 `[CHACHA][STATS]` 提供 operations 與 bytes，
但沒有逐 backend 的 elapsed time。因此本報告可確定資料邊界與呼叫次數，不能
僅靠現有 log 把 `prepare_us` 全部歸因於 ChaCha/AES。

如果後續需要量化純 crypto latency，應在下列 API 外圍只做 timer/counter 累積，
每 10 秒彙總，而不是逐幀印 log：

- `chacha20_poly1305_decrypt()` / `verify()`
- `chacha20_poly1305_encrypt()` / `final()`
- `AES_CTR_Update()`

並依 encrypt/decrypt、cipher、payload size bucket 與 hardware backend 分開統計。

## 9. 結論

1. **Video 接收與送出在 library/API 層級都是完整 frame 加解密。**
2. **TCP segmentation 不等於 crypto record segmentation。** TCP 只負責將完整
   encrypted record 以 byte stream 分段傳輸。
3. **ChaCha 大於 64 KiB 時會在 HAL 層分塊，但仍屬同一個 frame、同一個 AEAD
   record 和同一個 Poly1305 tag。**
4. **AES legacy screen path 同樣先組完整 frame，再以完整 length 呼叫
   `AES_CTR_Update()`。**
5. **Audio/control 另有自己的小型 records。** `[CHACHA][STATS]` 中大量 software
   operations 並不代表 video frame 被拆成許多 software calls。
6. 完整 frame buffering 是目前 latency pipeline 的真實邊界；它與原始 PTS 是否
   被保留共同決定 arrival burst 是否會轉化為可見播放 jitter。
