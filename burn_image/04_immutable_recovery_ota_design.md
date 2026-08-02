# 6 MiB Firmware 區域的 Immutable Recovery OTA 架構調查

調查日期：2026-08-02

## 目的

目前 NOR flash 留給 firmware 的空間只有 6 MiB，無法同時容納兩份約
5.3 MiB 的完整 CarPlay firmware，因此傳統的對稱 A/B OTA 不可行。

本報告評估以下非對稱配置是否可行：

- 第一份 firmware 是固定不再升級的 Recovery/OTA firmware。
- Recovery 只提供網路連線、下載、驗證及燒寫新版 main firmware。
- 第二份 firmware 是正常運作的 Main firmware，可持續 OTA 升級。
- Main 升級中斷或內容損壞時，仍可啟動 Recovery 重新升級。

本次只做靜態調查與架構分析，尚未修改 partition、linker script 或 source
code，也尚未使用實體板驗證。

## 結論

此架構可行，適合稱為：

> Immutable Recovery Firmware + Single Upgradeable Main Firmware

它不是兩份等大的 A/B application。Recovery 必須縮成一個很小、固定不變的
firmware，把絕大多數 flash 空間留給 Main。

RTL8195B 現有格式與 SDK 已具備這個方案所需的大部分基礎能力：

1. Partition table 原生支援 `FW1` 與 `FW2`。
2. Bootloader 會判定兩份 firmware 的 OTA signature 是否有效。
3. 只有一份有效時，bootloader 會選擇有效的 firmware。
4. 兩份都有效時，bootloader 會選擇 serial number 較新的 firmware。
5. OTA 寫入時會先保留 image signature，完成全部寫入後才寫回 signature。
6. 因此下載中斷、斷電或只寫入部分 image 時，新 Main 不會被視為有效。
7. Partition table 還定義了 `ota_trap` GPIO，可在開機時要求載入 OTA
   firmware；其實際 slot 對應及板級使用方式仍需實機確認。

## 現有 Flash 配置

目前 partition 設定：

| Offset | Length | Partition |
|---:|---:|---|
| `0x00009000` | `0x00010000` | Bootloader |
| `0x00040000` | `0x00600000` | FW1 |
| `0x00640000` | `0x00100000` | FATFS |
| `0x00740000` | `0x00080000` | LittleFS |

Firmware 區域是：

```text
0x00040000 .. 0x0063FFFF = 0x600000 bytes = 6 MiB
```

調查時的 build 產物大小：

| File | Size |
|---|---:|
| `firmware_is_ota1.bin` | 5,580,164 bytes (`0x552584`) |
| `ota_app.bin` | 5,580,196 bytes |

`ota_app.bin` 比 firmware 多 32 bytes OTA wrapper；partition 容量主要應以
實際寫入 flash 的 firmware payload 與 OTA parser 行為共同檢查。

## 建議的非對稱分區

建議先以下列配置做容量 PoC：

| Partition | Start | Length | 用途 |
|---|---:|---:|---|
| FW1 Recovery | `0x00040000` | `0x00080000`（512 KiB） | 固定、不升級，只負責 OTA |
| FW2 Main | `0x000C0000` | `0x00580000`（5.5 MiB） | 正常 CarPlay firmware |

區域關係：

```text
0x00040000  +------------------------------+
            | FW1 Recovery                 | 512 KiB
0x000C0000  +------------------------------+
            | FW2 Main                     | 5.5 MiB
            |                              |
0x00640000  +------------------------------+
            | FATFS                        |
```

選擇 512 KiB 的原因：

- FW1 與 FW2 的起始位址皆保持 256 KiB alignment。
- 現有 linker script 明確註明 XIP image start 必須 256 KiB aligned。
- Main 可用空間為 `0x580000`，即 5,767,168 bytes。
- 目前 Main firmware 為 5,580,164 bytes，尚餘 187,004 bytes。

因此目前 Main 可以放入 5.5 MiB，但剩餘空間只有約 182.6 KiB，後續新增
功能或 toolchain 尺寸變動都可能超出，必須在 build 時加入 hard size check。

若 Recovery 超過 512 KiB，下一個方便的 256 KiB alignment 邊界通常會使
Recovery 需要保留 768 KiB；此時 Main 只剩 5.25 MiB，現有 Main 已放不下，
必須再縮減 Main 或從其他 partition 取得空間。

## Bootloader 選擇行為

Bootloader AXF 反組譯中的 `boot_get_load_fw_idx()` 顯示：

- FW1 valid、FW2 invalid：選 FW1。
- FW1 invalid、FW2 valid：選 FW2。
- FW1/FW2 都 valid：比較 `fw1_sn` 與 `fw2_sn`，選 serial number 較新者；
  相同時目前實作傾向 FW2。
- 兩者都 invalid：沒有可正常載入的 firmware，進入 boot error/shell 路徑。

SDK image header 也把 `serial_no` 註解為「數字越大代表越新」。

因此建議：

- Recovery 固定為 FW1，serial number 固定且低。
- Main 固定為 FW2，每次 release 使用遞增 serial number。
- Recovery 永遠保持 valid。
- Main 只有在完整寫入後才恢復 valid signature。

## 建議 OTA 狀態流程

### 正常開機

```text
FW1 Recovery valid, serial 固定且低
FW2 Main     valid, serial 較高
                    |
                    v
Bootloader 選 FW2 Main
```

### Main 要求升級

Main 不應直接下載並寫入另一個 slot，因為現有對稱 A/B API 會把「另一個
firmware」視為 OTA target，可能破壞 Recovery。

建議流程：

```text
FW2 Main 收到升級要求
        |
        +-- 記錄 upgrade request、URL/version 等最小資訊
        +-- 將 FW2 OTA signature 標成 invalid，或使用經實機驗證的
            recovery boot trigger
        |
        v
Reboot
        |
        v
FW1 Recovery 啟動
        |
        +-- 下載新版 Main
        +-- 只 erase/program FW2 partition
        +-- 驗證接收長度與 partition boundary
        +-- 完整寫完後才寫 FW2 signature
        |
        v
Reboot
        |
        v
FW2 valid 且 serial 較新，Bootloader 選 FW2
```

SDK 的 `sys_clear_ota_signature()` 已提供將較新 firmware signature 改成
invalid、讓 bootloader 回到 default image 的機制。此函式會 read-modify-erase-
write firmware 第一個 sector，使用前仍要做斷電測試，並確認該函式及其
依賴在操作 active XIP image 第一個 sector 時均能安全執行。

更穩健的方案是使用 bootloader 原生 `ota_trap` GPIO，或新增獨立且具冗餘的
boot request flag；但是 `ota_trap` 實際會強制選擇哪個 slot、板子是否有可用
GPIO，都必須先在實體板驗證。

## 為何 OTA 中斷後仍可恢復

現有 `ota_8195b.c` 的主要順序是：

1. 根據目前執行的 firmware 選擇另一個 slot。
2. Erase target firmware 所需 sectors。
3. 收到 image 前 32 bytes 時，先備份 OTA signature。
4. 將 signature 位置暫時寫成 `0xFF`。
5. 串流接收並寫入其餘 firmware。
6. 全部完成後，最後才呼叫 `update_ota_signature()` 寫回 signature。
7. Reboot 後由 bootloader 驗證 image/hash。

因此下列失敗不會破壞 Recovery：

| 失敗情境 | 結果 |
|---|---|
| 下載前失敗 | Main 保持 invalid，Recovery 繼續執行 |
| Erase 後斷電 | Main invalid，重新啟動 Recovery |
| 寫到一半斷網 | Main invalid，Recovery 可重試 |
| 寫到一半斷電 | Main invalid，重新啟動 Recovery |
| Image hash/header 錯誤 | Bootloader 拒絕 Main，回到 Recovery |
| 完整成功 | 最後寫入 signature，重開後選新版 Main |

## 現有程式不可直接照用的部分

### 1. 對稱 A/B target selection

`update_ota_prepare_addr()` 現在的行為是：

```text
currently FW1 -> write FW2
currently FW2 -> write FW1
```

在非對稱架構中，只允許：

```text
Recovery FW1 -> write Main FW2
```

Main FW2 不得呼叫現有 OTA writer，否則會 erase immutable FW1 Recovery。
Main 只保留「請求進入 Recovery」的薄介面。

### 2. 缺少 target partition boundary check

目前 `update_ota_erase_upg_region()` 根據遠端提供的 image length 計算 sector
數量後直接 erase，沒有確認：

```text
NewFWAddr + image_length <= FW2 partition end
```

這是正式導入前必須修正的高風險問題。否則錯誤或惡意的 length 可能越過
FW2，覆蓋 FATFS、LittleFS 或其他資料。

必須在任何 erase/write 前檢查：

- Target 必須恆等於 FW2 start。
- Image length 必須大於最小合法 firmware header。
- Image length 不得超過 FW2 length。
- 每次 write 的 address + length 不得超過 FW2 end。
- OTA wrapper 32 bytes 是否計入傳輸長度，需用實際 OTA package 再確認。

### 3. Recovery 寫保護

至少要有軟體層保護：

- Recovery build 中的 OTA target 固定為 FW2。
- Main build 不連入 flash OTA writer，或 writer 對 FW1 一律拒絕。
- 日常 OTA package 不得包含 bootloader、partition table 或 FW1。
- Full flash factory image 與 OTA application image 必須是不同產物。

如果硬體 flash controller 支援適用的 region protection，可再研究是否能在
量產後保護 bootloader、partition table 與 Recovery；但必須確認 protection
不會阻止正常啟動或量產維修。

## 可恢復與不可自動恢復的失敗

### 可由上述架構處理

- OTA server 斷線。
- Wi-Fi 中斷。
- 傳輸未完成。
- Flash erase/program 中途斷電。
- Main image header、signature 或 hash 無效。
- Main partition 完全空白。

### 需要額外機制

如果 Main image 在密碼學及格式上完全有效，但開機後因程式 bug 立即 crash，
bootloader 仍會認為 FW2 valid，並在每次 reset 再次選擇 FW2。單靠 signature-
last 無法處理這種「valid but unbootable」情況。

若要涵蓋此情況，需要至少一項：

1. Boot-attempt counter + boot-success flag。
2. Main 啟動後必須在期限內標記 boot success。
3. Watchdog/reset reason 累積達門檻後切回 Recovery。
4. 實體按鍵或外部 MCU 控制 recovery GPIO。
5. 經實機確認可用的 `ota_trap` GPIO。

由於目前 bootloader source 未包含在 workspace，只有 AXF，若要在 bootloader
中增加自動 trial boot/rollback，可能需要 Realtek 提供 source 或新版 binary。
外部 GPIO recovery 是侵入性最低的替代方案。

## 512 KiB Recovery 的容量風險

512 KiB 是否足夠仍未證實。若要透過 Wi-Fi 做 OTA，Recovery 至少可能需要：

- 必要的 boot/runtime section。
- FreeRTOS kernel。
- Wi-Fi driver 與 WLAN firmware。
- lwIP、DHCP、DNS。
- TCP socket。
- HTTP 或 HTTPS client。
- Flash erase/write driver。
- OTA package parser。
- Firmware signature/hash 驗證相關支援。
- 最小 log、retry、watchdog 與狀態管理。

目前開機 log 顯示下載至 WLAN processor 的 firmware 本身約 115 KiB，因此
512 KiB 對「Wi-Fi + HTTPS + OTA」而言偏緊，但仍需由 minimal build 的實際
section size 判斷，不能只用 library archive 大小估算。

如果改用 UART、USB 或有線網路 Recovery，容量通常較容易控制；但是否符合
客戶現場升級流程需要另外評估。

## Immutable Recovery 的長期維護風險

Recovery 一旦量產後永不升級，必須避免依賴容易過期或改變的外部條件：

- OTA server domain、port 與 URL 格式需長期相容。
- TLS root CA、憑證期限與 cipher suite 要有長期策略。
- DNS、DHCP、Wi-Fi authentication 版本不可只支援短期配置。
- OTA package 格式需要 versioning 與 backward compatibility。
- Recovery 必須能拒絕超大、錯平台、降版或未授權 image。
- 即使傳輸使用 HTTP，也必須以可信任的 firmware signature 驗證 image；
  是否允許非 TLS 傳輸需另行做安全評估。

建議把 Recovery protocol 做得非常簡單，並讓 server 永遠保留相容 endpoint。

## 建議開發與驗證順序

### Phase 1：容量 PoC

1. 建立獨立 Recovery build target。
2. 只連入必要的 RTOS、Wi-Fi/lwIP、下載及 flash 功能。
3. 不連入 CarPlay、影音、AAC/H.264、UI、USB composite 等功能。
4. 產生 map/size 報告。
5. 確認完整 Recovery firmware 小於 512 KiB，並保留合理 margin。

此階段只驗證容量，不先修改量產 partition。

### Phase 2：雙 partition boot PoC

1. 建立 FW1 512 KiB、FW2 5.5 MiB 的測試 partition table。
2. Recovery 以 FW1 link target build。
3. Main 以 FW2 link target build。
4. 驗證兩份都 valid 時選 serial 較新的 Main。
5. 破壞 Main signature/hash，確認自動回到 Recovery。
6. 驗證 `ota_trap` GPIO 的實際選擇行為。

### Phase 3：Recovery OTA

1. Recovery writer 固定只能寫 FW2。
2. 加入完整 boundary checks。
3. Signature 維持最後寫入。
4. 驗證 image/hash、version 與平台識別。
5. 加入下載 retry、timeout、錯誤 log 與 watchdog。

### Phase 4：故障注入

在以下時點反覆斷電：

- Erase 前。
- Erase 中。
- 寫入 1%、50%、99% 時。
- Signature 寫入前。
- Signature 寫入兩段之間。
- Signature 完成後、reboot 前。

每一次都必須確認：Recovery 仍能開機，且可重新進行 OTA。

### Phase 5：Main boot failure recovery

決定是否需要處理「有效但無法正常啟動」的 Main：

- 若需要，評估 GPIO recovery、外部 MCU 或 boot-success protocol。
- 若不需要自動 rollback，至少保留客戶可操作的實體 Recovery 入口。

## 驗收條件

此方案在滿足以下條件後才適合量產：

- Recovery image 加 margin 後仍小於 512 KiB。
- Main release 在 5.5 MiB hard limit 內。
- Main 無法 erase/write FW1 Recovery。
- 所有 OTA erase/write 都有 FW2 boundary check。
- 任何未完成 OTA 都不會使 FW2 valid。
- FW2 無效時 bootloader 100% 回到 FW1。
- 斷電測試涵蓋 erase、program、signature commit 各階段。
- 至少有一種不依賴 Main 正常執行的 Recovery 進入方式。
- Factory full image 與日常 OTA package 清楚分離。

## 相關 Source 與證據

- `project/realtek_amebapro_v0_example/GCC-RELEASE/partition.json`
  - 目前只有 FW1 6 MiB，後方緊接 FATFS。
- `project/realtek_amebapro_v0_example/EWARM-RELEASE/partition.json`
  - SDK 原始範例包含 FW1/FW2 兩個 partition。
- `project/realtek_amebapro_v0_example/GCC-RELEASE/rtl8195bhp_ram_is.ld`
  - XIP image start 需要 256 KiB alignment。
- `component/soc/realtek/8195b/cmsis/rtl8195b-hp/lib/include/fw_img.h`
  - 定義 FW1/FW2、serial number、valid 狀態及 `ota_trap`。
- `component/soc/realtek/8195b/misc/platform/ota_8195b.c`
  - 現有 OTA target selection、erase/write 與 signature-last 流程。
- `component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/sys_api.c`
  - OTA signature clear/recover 與 alternative firmware selection API。
- `component/soc/realtek/8195b/misc/bsp/image/bootloader.axf`
  - 反組譯確認 firmware validity 與 serial selection 行為。
- `burn_image/02_bootloader_reverse_engineering.md`
  - 既有 bootloader 靜態分析報告。

## 最終判斷

在目前 6 MiB firmware 空間內，保留兩份完整 Main firmware 不可行；保留一份
512 KiB immutable Recovery，加上一份 5.5 MiB upgradeable Main，從 boot 與
OTA 機制上可行，現有 Main 大小也勉強容納得下。

真正的 go/no-go 條件是：最小 Wi-Fi OTA Recovery 能否在保留安全 margin 的
情況下放入 512 KiB。

## 2026-08-02 實作／容量 PoC 結果

已在 `feature/immutable-recovery-ota` 建立第一版實作：

- FW1 Recovery 固定為 `0x40000 + 0x80000`。
- FW2 Main 固定為 `0xC0000 + 0x580000`。
- Main 預設以 FW2 link target build。
- Recovery OTA 的每一次 erase/write 都會檢查 FW2 邊界，從 FW2 執行 writer
  也會被拒絕。
- `make recovery_image` 與 Main packaging 都會在 checksum 後執行 hard size
  check；超界時不會產生 factory image。
- `make factory_image` 只有在 FW1、FW2 都通過容量檢查後才會 combine。

實際 build 結果：

| Image | Signed/checksummed size | Partition | 結果 |
|---|---:|---:|---|
| Main FW2 | 5,565,124 bytes | 5,767,168 bytes | PASS，餘 202,044 bytes |
| Recovery FW1 | 1,315,076 bytes | 524,288 bytes | FAIL，超出 790,788 bytes |

Recovery profile 已移除 ISP、WoWLAN 與 FWLS image，並把 skb/lwIP pool 縮到
只支援 DHCP 與單一 OTA TCP connection 的規模。剩餘差距主要來自封閉的
`lib_wlan.a` Wi-Fi driver/PHY code、driver 內建 WLAN firmware，以及必須同時
支援 B/C cut 的兩份外部 WLAN firmware。這不是小幅裁 command 或調 compiler
flag 就能縮進 512 KiB 的差距。

因此目前的精確結論是：

- 非對稱 FW1/FW2、FW2-only OTA 與 Main 5.5 MiB 都已驗證可 build。
- 以現有 WLAN binary 組合，`512 KiB network Recovery` 容量 PoC **不通過**。
- size gate 正確阻止產生會覆蓋 FW2 的錯誤 factory image。
- 下一個架構決策必須在「取得更小的 Realtek Recovery WLAN library」、
  「縮小 Main 後擴大 Recovery」，或「Recovery 改用 UART/USB 而非 Wi-Fi」之間
  選擇；在決策前不可把目前 partition 當成可量產配置。
