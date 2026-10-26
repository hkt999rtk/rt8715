# Wi-Fi 事件相容性驗證

參考檔案：`/home/kevin/work/carbox_smart.orig/component/wifi/api/wifi_ind.c`。

這次對齊的是 RT8715 既有連線事件的處理行為，保留 RT8715 driver、
客戶靜態庫使用的事件編號和參數格式。不是把另一個 SDK 的 `wifi_ind.c`
直接覆蓋過來。

## 行為對照

| 參考平台的行為 | RT8715 實作 |
| --- | --- |
| 先處理內部 join 狀態，再通知應用 callback | `wifi_indication()` 先呼叫 `wifi_event_sta_internal_hdl()`，再分派應用事件；內部 handler 不再占 callback table |
| join 成功後 link-up、喚醒等待者 | OPEN/WEP 在 CONNECT 完成；WPA 在 FOURWAY_HANDSHAKE_DONE 完成。先更新 link，再 give semaphore |
| join 失敗後喚醒等待者 | NO_NETWORK 記錄失敗並喚醒；斷線沿用失敗原因判定。NO_NETWORK 不會因暫時仍顯示 driver ready 而回傳成功 |
| 斷線停止 DHCP 並 link-down | 停止 STA 的 DHCPv4/v6，清除 link 和連線成功狀態；應用 callback 看到的是更新後狀態 |
| AP client 加入／離開通知 | 繼續走 STA_ASSOC／STA_DISASSOC 的應用 callback；不會執行 STA 的 DHCP／link 清理 |
| callback 表滿的診斷 | 增加 `WifiEvtReg fail`；重複註冊不會被誤報成表滿；保留原本三個應用 slot |
| 直接分派 callback 的函式 | 對外宣告 `rtw_indicate_event_handle()`，只分派 callback；driver 應繼續呼叫 `wifi_indication()` |
| 保存快速連線資訊、釋放 join password | 沿用 `wifi_connect()`／`wifi_connect_bssid()` 的完成路徑，避免在 callback 再保存或再釋放一次 |
| 自動重連 | 沿用 RT8715 `wifi_config_autoreconnect()`、driver 的 `p_wlan_autoreconnect_hdl` 及 `wifi_autoreconnect_thread()`；未增加第二套 retry timer |
| EAP 斷線處理 | 沿用 EAP 啟用時註冊的 `eap_disconnected_hdl`；目前建置未啟用 EAP |

內部 handler 現在會持續存在，因此同步等待結束後，先解除 `join_user_data`，
再釋放 semaphore/password/result，避免後續事件取得已釋放的 join 資料。

## SDK 邊界

- 參考平台的 `WIFI_EVENT_JOIN_STATUS`、中間 join 狀態、WPA-lite 和
  `rtw_psk_deauth_info_flash()` 屬於另一套 driver/API；本次沒有建立這些
  原始 API，也沒有宣稱其全部狀態機或二進位介面相容。
- RT8715 的 `rtw_join_status` 是 bitmask，參考平台使用 enum，不能直接互換。
- 參考平台有專用的 Flash deauth 記錄；本版不具備相同資料格式與擁有者，
  因此沒有將其清零寫入邏輯套到 RT8715 的 Flash。
- 本次保留既有連線 API；外部 semaphore 的非同步連線模式不在本次測試範圍。

## 執行

```sh
python3 project/realtek_amebapro_v0_example/src/carbox/tests/wifi_ind/test_events.py
```

測試讀取並編譯實際的 dispatcher、STA handlers 與兩個公開連線函式，
以 host stub 取代 radio、lwIP netif 與 RTOS semaphore，搭配
AddressSanitizer／UndefinedBehaviorSanitizer。涵蓋 OPEN/WEP/WPA 成功、
逾時、找不到 AP、rekey、遲到的 handshake、STA 斷線、AP client 離開、
callback 表滿、無效事件、短／未對齊 reason buffer，以及 join 釋放順序。

這些測試不能證明真實無線事件時序、RF 行為或 driver 自動重連的成功率。
實機需測：STA 成功／錯誤密碼／找不到 AP／AP 關閉後重連，以及 STA+AP
模式下 iPhone 離開是否仍保留 AP 服務。
