# Realtek WLAN RX Buffer-Swap Reference

此目錄是 `../17_realtek_wlan_rx_zero_copy_compatible_plan.md` 的簡化參考程式，不編入
目前 firmware。

- `rtw_rx_zc_reference.h`：private swap callback、fixed-buffer ownership 與 statistics。
- `rtw_rx_zc_reference.c`：`callback != NULL` 時 swap，否則執行原本
  `rtw_memcpy()` 的最小 driver skeleton。

設計重點：

1. function pointer 預設 `NULL`，因此舊客戶行為不變；
2. completed RX buffer 與剛取得的 skb backing buffer 直接互換；
3. backing buffers 由 fixed pool 管理，不做 per-packet heap malloc/free；
4. callback 失敗必須保證所有狀態不變，caller 才能安全 fallback；
5. `rtw_port_*` 是 Realtek RD 必須對接 private descriptor/skb/cache primitives 的
   placeholder，不是公開 API。
