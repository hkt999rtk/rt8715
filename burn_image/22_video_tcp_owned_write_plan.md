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
