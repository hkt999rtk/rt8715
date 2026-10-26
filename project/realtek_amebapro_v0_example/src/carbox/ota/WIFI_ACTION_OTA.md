# RTL8715 Action broadcast OTA

## 依據與目標

參照 `../carbox_smart.orig/component/wifi/api/wifi_init.c` 的
`wifi_action_user_callback`、`action_data_process`、`action_version_report`、
`action_ota_upgrade`，以及 `wifi_ota.h/.c` 的標記與 TLV 定義。

對正常的 host 廣播保留同樣的操作順序、版本判斷、回報協定與重試規則。
8715 使用自身的 driver API 與 Flash layout；不能把 Smart 的事件編號、
RTOS 型別或 OTA 寫入地址直接複製過來。

## 實作入口

- `component/common/api/wifi/wifi_conf.c`：`wifi_on()` 成功後啟動接收。
- `wifi_action_ota.c`：註冊 `WIFI_EVENT_RX_MGNT`、啟用
  `wifi_set_indicate_mgnt(1)`、queue 和背景 task、版本回報與更新流程。
- `wifi_action_protocol.c/.h`：Action header 與 TLV 解析、HTTP URL 解析。
- `carplay_ota_http.c/.h`：TCP/HTTP GET，分段接收後交給 package writer。
- `carplay_ota_compat.c/.h`：沿用已有 Smart OTA package 格式、checksum、
  alternate FW slot、FATFS 寫入及最後的 signature commit。
- `GCC-RELEASE/application.is.mk`：編入新 C 檔，啟用
  `CONFIG_CARBOX_ACTION_OTA=1`。不需要新增 binary patch 或 linker wrapper。

## 封包格式

24-byte 802.11 management header 後：

```
C8 4F 54 41 DD 09 6C 69 74 69 61 6E 74 61 69
|  O  T  A       l  i  t  i  a  n  t  a  i
```

後面是 `type(1 byte), length(1 byte), value(length bytes)`：

| type | 欄位 | 格式 |
| --- | --- | --- |
| 0x10 | SSID | NUL 結尾字串，length 包含 NUL |
| 0x11 | password | 緊接對應 SSID；有此欄位使用 WPA2 AES PSK |
| 0x12 | URL | `http://host[:port]/resource` |
| 0x13 | version | 用字串完全相同判斷，不比較版本新舊 |

可包含多組 AP；沒有 password 的 AP 使用 OPEN。尾部零 padding 可接受。
完整 frame 上限 2304 bytes，AP 清單按封包長度配置，SSID 上限 32 bytes、PSK 上限
64 bytes（均不含 NUL）。無效長度／未終止字串會拒絕。
不支援 HTTPS 或 HTTP chunked；參考平台此 HTTP 流程也依賴 Content-Length。

## 與參考平台相同的流程

1. RX callback 僅複製封包並排入 queue；處理中略過新封包。
   queue 深度 16；worker priority 1，stack 8192 bytes。
2. 版本相同且已成功回報時，通知 `usb_ota_update_status(2,0)`，不重連。
   每收到第 10 次相同版本的通知，清掉已回報標記，允許再次回報。
3. 沒有 AP 不處理；`get_aac_cp_aa_state()!=0` 表示使用中，不啟動更新。
4. 多 AP 隨機挑選，保留 Smart list 前插的索引順序。
   Wi-Fi connect 失敗等待 100 ms；連續 5 次失敗，等待 1 秒重啟。
5. 連上後 DHCP。DHCP 失敗則 disconnect、等待 100 ms，再重連；
   跟原版一樣，此分支本身不扣 Wi-Fi connect 的重試次數。
6. 解析 URL，再檢查一次 CarPlay/AA 狀態。
7. 連到 URL host 的 TCP port **58616**，送出 `MAC,local_version`，
   沒有換行或 NUL。保留原版格式 `%02X%02X%02X%2X%02X%02X,%s`，
   包含第 4 個 MAC octet 的空白補齊行為，避免改變 host 的既有協定。
   回報失敗不阻擋後續 OTA。
8. 版本相同只回報更新完成狀態，不下載；版本不同就 HTTP GET。
9. `usb_ota_update_status(1,0)` 後開始更新。成功等待 20 ms 重啟；
   下載／寫入失敗等待 1 秒重啟。初始化失敗直接結束該次處理。

## 平台適配與安全邊界

- Smart 的 `WIFI_EVENT_USER_RX_ACTION` 已由其 driver 過濾；8715 沒有這個
  event ID，因此在原始管理封包 callback 辨識同一個 marker。
  已檢查目前 ELF 的 `_mgt_dispatcher`：啟用管理封包 indication 後，
  會先上報 raw frame，再分派 Action category，不必修改 `OnAction`。
- 8715 同時有 STA、AP、USB netif；取得 STA DHCP 後指定 STA 為 default route。
  Action 流程開始時停用 STA auto reconnect，避免與 OTA 主動連線競爭。
- Smart 的 `stop_bt_task()` 只設定內建 `bt_proc_task` 的停止重連 flag。
  8715 目前沒有此 task／symbol，使用外部 MCU 藍牙。因此保留 weak callback，
  平台若提供該 task 就呼叫；目前映像不會向外部 MCU 發送額外 BT 指令。
  **這項平台差異不應描述成已驗證外部 MCU 的重連行為相同。**
- 修正原流程的 queue 失敗釋放順序、socket 未關閉、TLV 邊界問題；
  不複製 use-after-free 或越界存取。log 不印 password。
- Smart package header 先 memcpy 到對齊的結構再讀欄位，避免 byte buffer
  直接轉型造成未對齊存取（主機 UBSan 測試有重現舊寫法的問題）。
- HTTP 與原版一樣最多跟隨一次 302 的絕對 HTTP Location，之後須回覆 200。
  檢查 Content-Length；拒絕壓縮／chunked body、重複
  或無效長度。HTTP 長度必須等於 package 宣告的總長度。
- 資料經現有 `download_fw_program()` 驗證及寫入。
  HTTP 失敗會標記失敗，deinit 不會補寫可開機 signature。
  保留既有 Web POST OTA 的 ABI 和寫入流程。
- FATFS 仍是既有的單一共用分區；失敗時不 commit 新 FW，並不代表
  中途已寫入的 FATFS 會自動回復。此處未新增 FATFS 雙分區機制。

## 測試與實機驗證

主機執行：

```
python3 project/realtek_amebapro_v0_example/src/carbox/tests/wifi_action_ota/run.py
python3 project/realtek_amebapro_v0_example/src/carbox/tests/wifi_ind/test_events.py
```

- 截圖 payload、TLV padding／截斷／錯誤長度、多 AP 順序、URL。
- 使用中跳過、前後兩次狀態檢查、同版本每 10 次回報、回報失敗繼續 OTA、
  connect 失敗 5 次、DHCP 重試、queue 滿時的記憶體釋放。
- localhost TCP server 測 HTTP header/body 分段、404、302 轉址／連續轉址拒絕、長度錯誤、短收、
  callback 拒絕；無外部下載。
- 模擬 Flash 執行真實 package writer：FW-only、FW+FATFS、長度不符、
  FW/FATFS checksum 錯誤、下載不完整；失敗不得 commit FW signature。
- C 測試使用 AddressSanitizer / UndefinedBehaviorSanitizer。

這些不是實機空中收包、Flash 或重啟測試。燒錄後應先看到：

```
[ACTION_OTA] enabled build=... marker=C8/OTA/litiantai queue=16 priority=1 stats_ms=5000
[ACTION_OTA][RX] ms=... channel=... mgmt=... action=... ota=... match=...
[ACTION_OTA] process begin ms=... len=... flags=... src=...
[ACTION_OTA] connect ...
[ACTION_OTA] DHCP assigned
[ACTION_OTA] version report port=58616 result=1
[ACTION_OTA] HTTP status=200 content_length=...
[carplay_ota] package finalize done ...
[ACTION_OTA] OTA result=0; reboot
```

Host 廣播時需與 8715 接收端位於相同可接收的無線頻道；本次沒有加入
channel hopping，也不代表其他頻道的廣播一定會被收到。
URL 必須提供 **8715 這個專案產生的 `ota_all.bin` 或 `ota_app.bin`**，
不能提供 Smart/8730 的 firmware payload 或完整 `flash_is.bin`。

## 實機 log 判讀（診斷版）

待機時每 5 秒印一次 `[ACTION_OTA][RX]`。計數都是本次開機累計，
RX callback 只記錄計數，不逐筆 printf。

| 觀察 | 意義／下一步 |
| --- | --- |
| 沒有 `enabled` | 先檢查 build version、Wi-Fi init、queue/task create failed |
| `mgmt` 一直 0 | 沒有管理封包進入這個 callback；檢查頻道、driver indication。不能只靠此值斷言空中沒有封包 |
| `mgmt` 增加但 `action` 不增 | 有管理封包，沒有 Action subtype |
| `action` 增加但 `ota` 不增 | 未收到 `C8 OTA`；參考 `last_category` |
| `ota` 增加但 `match` 不增 | `DD 09 litiantai` 或封包長度不符 |
| `busy_drop` 增加 | 前一包正在連線／回報／更新，依原流程略過新廣播 |
| `nomem` / `full` 增加 | RX 配置記憶體失敗／16 格 queue 已滿 |
| `parse failed` | 看 TLV 的 offset/type/len，log 不含 password value |
| `pre_connect cp_aa_state` 非 0 | 依原版規則，CarPlay/AA 使用中跳過 |
| `connect result` 非 0 | 同時看 `wifi_error` 與耗時；最多 5 次後重啟 |
| DHCP start 沒有 result | 停在 DHCP 等待；成功時會印 STA IP、gateway、channel、RSSI |
| `[TCP]` resolve/connect 失敗 | 指定 host/port 無法連線；區分回報 58616 與 OTA URL port |
| `[HTTP] result=-1` | 看 phase、io_result、errno、received/expected；io_result=0 常是 EOF，負值是 socket read error |
| `phase=header_validation` | HTTP status／Content-Length／傳輸編碼不符合 |
| `phase=package_begin` | 宣告的檔案長度超過本機 FW+FATFS 容量 |
| `phase=package_write` | 看前面的 `[carplay_ota]`，區分 package header、checksum、範圍、Flash readback 錯誤 |
| `package finalize done` + `OTA result=0` | 新 FW signature 已提交，接著 20 ms 後重啟 |

HTTP 每約 10% 印一次收取 bytes 與 elapsed_ms；各階段也有起訖 log。
`errno` 是 Realtek lwIP 的 `lwip_err` 快照，不是原生 newlib errno；
該值是 SDK 共用狀態，應搭配 phase/ret/長度看，不能單獨當作根因。
完整測試 log 請從開機 `main build_version` 一路保留到更新完成或失敗重啟。
