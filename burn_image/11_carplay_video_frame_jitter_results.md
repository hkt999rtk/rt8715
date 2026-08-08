# CarPlay Video Frame Jitter 實機量測結果

## 1. 目的

本報告整理 2026-08-06 實機測試中的 video frame jitter 統計，判斷目前系統是否有 frame 持續堆積，以及延遲主要出現在哪一段。

本次 profiler 不逐幀輸出 UART log；每一幀只更新 RAM 內的統計值，並每 10 秒輸出彙總結果及最嚴重的 8 個異常 frame。

## 2. 時間戳與量測邊界

時間戳來自 `hal_read_curtime_us()` 讀取的 32-bit System GTimer，屬於本機單調微秒時間，沒有經過 NTP 或 AirPlay timing clock 校正。

這適合量測單一裝置內的間隔與階段耗時，因為不會受到 wall clock 校時、step 或 slew 影響。32-bit counter 約每 71.6 分鐘回繞；程式使用 unsigned subtraction，對本報告中的短時間差可正確跨越回繞。

目前量測點如下：

1. `arrival`：完整 video frame 進入 `AirPlayScreen_SendVideo()`。
2. `queue`：frame 從進入 queue 到 `ScreenThread` dequeue。
3. `prepare`：dequeue 後到第一次 `lwip_write()`，包含 frame header、buffer 準備、memory copy 與加密等工作。
4. `socket`：第一次 `lwip_write()` 到該 frame 的資料全部被 lwIP socket 接受。
5. `socket_handoff`：從 `AirPlayScreen_SendVideo()` 到 frame 全部被 lwIP socket 接受。

`socket_handoff` 不代表資料已經由 USB wire 傳輸完成；目前可靠的結束邊界是 `complete-frame-to-lwip-write-accepted`。

## 3. 實機數據

10 秒視窗的主要結果：

| 項目 | 結果 |
|---|---:|
| Frames | 298 |
| Frame rate | 29.80 fps |
| Frame bytes | 11,774,944 bytes |
| Queue 最大深度 | 3 frames |
| 視窗結束 queue depth | 0 |
| Overflow / desync | 0 / 0 |
| Frame without write | 0 |

### 3.1 完整 frame 抵達間隔

| 統計 | Arrival delta |
|---|---:|
| Average | 33.306 ms |
| Sigma | 15.377 ms |
| Median | 33.156 ms |
| P95 | 57.046 ms |
| P99 | 76.324 ms |
| Maximum | 95.474 ms |

以 33.156 ms median 為基準的 jitter：

| 統計 | 結果 |
|---|---:|
| Mean absolute deviation | 12.357 ms |
| MAD | 10.195 ms |
| P95 absolute deviation | 29.228 ms |
| P99 absolute deviation | 43.168 ms |
| Maximum absolute deviation | 62.318 ms |
| Delta > 40 ms | 106 / 298 |
| Delta > 50 ms | 39 / 298 |
| Delta > 100 ms | 0 / 298 |

平均值及 median 接近 30 fps 所需的 33.3 ms，但 tail 很長，代表完整 frame 並非以穩定的 33.3 ms 間隔交給下游。

### 3.2 本機處理階段

| 階段 | Average | Sigma | P50 | P95 | P99 | Maximum |
|---|---:|---:|---:|---:|---:|---:|
| Queue wait | 9.405 ms | 8.069 ms | 6.753 ms | 25.033 ms | 37.436 ms | 42.743 ms |
| Prepare | 3.098 ms | 4.130 ms | 2.005 ms | 11.989 ms | 25.973 ms | 26.654 ms |
| Socket write | 7.496 ms | 6.048 ms | 6.560 ms | 22.059 ms | 26.030 ms | 27.049 ms |
| Socket handoff | 19.999 ms | 11.252 ms | 16.828 ms | 41.376 ms | 56.021 ms | 57.709 ms |

平均 socket handoff 時間等於 queue、prepare 與 socket 三段平均值之和。各 percentile 不能直接相加，因為三個階段的最慢 frame 不一定相同。

## 4. 結論

### 4.1 存在明顯 jitter

完整 frame 的平均間隔正常，但 P95、P99 與 maximum 分別到達 57.0、76.3 與 95.5 ms。主要 jitter 在完整 frame 進入 `AirPlayScreen_SendVideo()` 時就已經可以觀察到。

因此目前不能把全部 jitter 歸因於 `ScreenThread`。它可能來自 iPhone frame pacing、網路/TCP/NCM 傳輸、上游收包與組幀、解密，或 receiver task scheduling。

### 4.2 本機 queue 也增加了 tail latency

本機三個階段中，queue wait 的平均耗時與最大耗時最高：平均 9.4 ms、P95 25.0 ms、最大 42.7 ms。Prepare 與 socket write 也各自有約 26 至 27 ms 的 tail。

所以系統內部確實會進一步放大部分 frame 的 latency，但它不是目前唯一的 jitter 來源。

### 4.3 沒有持續消化不良

雖然 queue 最大深度曾到 3，但視窗結束時 depth 為 0，且沒有 overflow、desync、post failure 或 frame-without-write。

目前行為較像 frame 不均勻或成批抵達後，系統短暫排隊但仍能追上；沒有證據顯示 queue 長期累積。

`frames=298`、`completed=299` 及 `deq=299` 是 10 秒統計視窗邊界造成：前一視窗進入的 frame 可以在本視窗完成，不代表 frame 被重複處理。

## 5. 目前不能回答的問題

目前只有本機完整 frame arrival time，沒有同時保存 iPhone 的 media timestamp，因此無法把 jitter 精確拆成：

- iPhone 產生 frame 的 pacing jitter；
- iPhone 到 box 的 transport jitter；
- box 內收包、組幀及解密 jitter；
- `ScreenThread` 到 socket handoff jitter；
- lwIP/TCP 到 CDC-NCM USB completion jitter。

另外，`lwip_write()` 成功只表示 lwIP 接受資料，不代表 USB transfer 已完成。

## 6. 建議的下一階段量測

若需要找出 jitter 的真正來源，建議依序增加以下時點，並繼續採用 RAM 統計加每 10 秒彙總，避免逐幀 UART log 影響結果：

1. 在 AirPlay frame/header 解析點保存 sender media timestamp。
2. 保存第一個 TCP byte 與完整 frame 組裝完成的本機時間。
3. 將 sender timestamp 透過 AirPlay timing mapping 轉成本機時間軸，計算 transit-time variation。
4. 對每個 frame 建立 sequence/byte-range，追蹤到 lwIP TCP output、CDC-NCM NTB submit 與 USB bulk completion。
5. 分別輸出 source pacing、network/assembly、local queue、encryption、socket、NCM/USB 的 P50/P95/P99 與異常事件。

完成以上量測後，才可以可靠判斷 jitter 是由來源端、傳輸端或本機處理造成。
