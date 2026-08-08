# Video TCP owned-write zero-copy 計劃與 bring-up

## 目的

ScreenThread 目前將完整 encrypted wire record 傳給 `lwip_write()`，socket layer 設定
`NETCONN_COPY`，`tcp_write()` 再按 MSS 將同一筆 payload 複製進多個 TCP pbuf。這層
copy 在最近一次五秒 profile 中搬移約 5.95 MiB、耗時約 292 ms。

本變更只移除已被 `VIDEOHOF` 與 `TXCRYPTO_DIRECT` 精準辨識之 video wire buffer 的
TCP segmentation copy。一般 socket、控制訊息、audio、WLAN 與所有無法證明 ownership
的 buffer 仍使用 legacy COPY API。

## Ownership contract

`lwip_write_owned()` 建立一個 shared owner。每個 MSS payload 使用 custom reference
pbuf，並各持有一個 owner reference；TCP header 仍由 lwIP 配置。application write
完成後只放掉 API reference，buffer 必須保留到下列任一事件清除最後的 pbuf：

- remote ACK；
- retransmission 完成後 ACK；
- socket abort/close；
- enqueue transaction rollback。

closed ScreenThread 原本的 `free()` 由 object-local hook 攔截。如果 TCP 尚持有
reference，只記錄 consumer release；最後一個 TCP pbuf 釋放時才真正 free。若 TCP
在 application free 前已完成，原本的 free 路徑照常執行。

## Compatibility

- `lwip_write()` 與 `lwip_send()` ABI/行為不變。
- `LWIP_NETIF_TX_SINGLE_PBUF` 對 legacy traffic 仍強制 COPY。
- 只有顯式 `tcp_write_owned()` 可建立 header + referenced payload chain。
- allocation、predicate 或 transaction 不符合時走原本 COPY path。

## 第一版測試

1. 確認正常出圖、audio/video 同時播放。
2. 至少連續播放 30 分鐘，包含重新連線與拔插。
3. 製造 Wi-Fi packet loss，確認 TCP retransmission 後畫面仍正確。
4. 每 10 秒檢查：

```text
[TCPOWN] create/fail pbuf/fail bytes release live/max
[TXCRYPTO_DIRECT] tcp_owned begin/defer/complete/final
```

正常穩態允許少量 `live`（尚未 ACK 的 frame），但不能持續單調增加。`pbuf/fail` 應為
零；任何 assert、heap corruption、畫面破損或 reconnect 後 live 不下降都視為失敗。

## 預期 profile 變化

- `[TCP_PERF] TX ... copy=...B copy_us=...` 的 video payload bytes 應大幅下降。
- TCP segment、USB packet 與 NCM NTB 數量不應因此改變。
- memory 使用量會從「TCP-owned copied pbuf payload」轉為「ACK 前保留原 wire
  buffer」；峰值取決於 congestion window 與 RTT。

## 2026-08-08 TCP backlog 與 iPhone 限速調查

### 新增觀測項目

`[TCPOWN]` 原本只顯示 create、release 與當下 live 數量，無法區分正常的
in-flight buffer 和長時間排隊。為此加入 `[TCPOWNAGE]`：在建立 shared owner 時保存
時間，並在最後一個 TCP pbuf 釋放時統計整個 owned frame 的 lifetime。

```text
[TCPOWNAGE] samples=... age_ms_avg/max=...
bins <10/10-25/25-50/50-100/100-200/200-500/>=500=...
live_now/max=...
```

這個 lifetime 的起點是 `tcp_write_owned()` 建立 owner，終點是最後一個引用該
payload 的 pbuf 被釋放。正常傳輸時終點通常由 remote TCP ACK 觸發；因此數值同時
包含：

1. segment 尚未送出的本機排隊時間；
2. segment 已交給 netif、但仍等待 cumulative ACK 的時間；
3. 發生 loss 時的 retransmission lifetime。

它不是純粹的 `tcp_write()` 執行時間，也不能代表 remote 已經解碼或顯示 frame。

### 實測結果

兩個連續穩態窗口的 owned-buffer 統計如下：

```text
window 4: create=243 release=242 live_now/max=2/3
          age avg/max=49/105 ms
          bins=8/16/98/116/4/0/0

window 5: create=233 release=234 live_now/max=1/3
          age avg/max=49/105 ms
          bins=1/13/115/104/1/0/0
```

兩個窗口合計 `create=476`、`release=476`，live 從 2 降至 1，峰值皆為 3。沒有
buffer 落在 `200–500 ms` 或 `>=500 ms`。以實際約 24 fps、每幀約 41.7 ms 計算，
49 ms 約等於 1.18 個 frame interval，與平常 1–2 個、短暫最多 3 個 owned frame
一致。

因此 49 ms 是值得注意的 TX in-flight latency，但目前不是 queue 持續累積的證據。
若是累積，預期會同時看到：

- 多個窗口持續 `create > release`；
- `live_now` 單調增加；
- age 分布逐步移向 `100–200 ms`、`200–500 ms` 或更高；
- TX send buffer 長時間接近容量，並可能出現 partial write/error。

本次實測沒有上述趨勢。

### TCP RX/TX window 狀況

video RX socket 使用 negotiated window scaling。穩態窗口觀察到：

```text
RX window cap                 約 262140 B
minimum available/advertised 約 181–186 KiB
RX ge75/ge90/full             0/0/0
advertised zero window        0

TX buffer cap                 約 262140 B
TX maximum used              約 156–163 KiB
TX ge75/ge90/full             0/0/0
```

所以沒有證據顯示本機 video RX window 關閉或 TCP TX send buffer 長期滿載，亦沒有
看到 window pressure 迫使 iPhone 限速。

### 本機 video pipeline backlog

`SCREENQ` 的 frame age 平均約 6.5–6.9 ms、最大約 26–28 ms，queue depth 最大為 2。
`lwip_write()` 沒有 partial/error。`select()` ready 後立即返回的比例只有約 4–5%，
大部分時間 task 是阻塞等待新資料，而不是 socket 中已有大量 frame 尚未讀取。

這表示從 video socket 到 ScreenThread handoff，再到 `lwip_write()` 接受資料的本機
路徑沒有形成秒級 backlog。

### iPhone frame cadence

穩態的 iPhone NTP delta 主要落在 33.333 ms 與 66.666 ms：

```text
p50 = 33.333 ms
p95 = 66.666 ms
effective rate 約 23.5–24.1 fps
```

協商的 30 fps 是能力上限／時間基準，不保證 iPhone 每個 33.333 ms slot 都產生一個
frame。iPhone 經常跨過一個 slot，因此實際輸出低於 30 fps。

另一次窗口同時出現約 8.62 秒的 arrival gap、8.63 秒的 iPhone NTP delta，以及
8.62 秒的 wire delta。三者同步跨越，表示這不是 8.6 秒 frame 卡在本機 queue；較像
source 暫停產生 frame、stream 狀態切換，或測試期間長時間無畫面更新。這種窗口不應
用來計算穩態 FPS。

### 目前結論與限制

在暫不分析下游解碼／顯示的前提下：

- `tcp_write` 路徑存在約 49 ms 的正常 outstanding lifetime；
- 目前沒有 TCP owned queue 持續或秒級累積；
- RX advertised window、TX send buffer、SCREENQ 與 socket read backlog 均不是已觀察
  到的限速點；
- frame 到達 cadence 主要由 socket 上游決定，iPhone 常以 33.333/66.666 ms 間隔送出；
- 現有 iPhone NTP 是 media timestamp，不能單獨證明它等於相機 capture 時刻，也不能
  直接量出 iPhone 內部 capture/encode queue 的絕對延遲。

若後續需要釐清 49 ms 的組成，應將 owner lifetime 再拆為：

```text
owner create -> frame 最後一個 segment 交給 netif
frame 最後一個 segment 交給 netif -> final ACK/release
```

第一段才是本機 unsent queue；第二段主要是傳輸中與等待 ACK。只有第一段長時間上升，
才能直接判定 `tcp_write` 後方正在本機累積。
