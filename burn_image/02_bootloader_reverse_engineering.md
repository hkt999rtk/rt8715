# `bootloader.axf` 反組譯報告

分析對象：

`component/soc/realtek/8195b/misc/bsp/image/bootloader.axf`

## 檔案性質

- ELF32 little-endian ARM executable
- ARM EABI5、soft-float
- AXF 檔案大小：1,172,320 bytes
- 大部分檔案大小來自 DWARF debug information
- 真正會進入 boot image 的 code/rodata/data 約 20 KB
- BSS 是配置需求，不會以全零資料完整存入 flash image

主要 section：

| Section | 位址 | 大小 | 用途 |
|---|---:|---:|---|
| `.ram.vector` | `0x20000000` | 192 | vector table |
| `.boot_temp_use.bss` | `0x20000400` | 9,920 | boot 暫存 BSS |
| `.ram.bss` | `0x20172000` | 4,456 | bootloader BSS |
| `.ram.data` | `0x20174000` | 164 | initialized data |
| `.ram.code_text` | `0x201740A4` | 16,072 | bootloader code |
| `.ram.code_rodata` | `0x20177F80` | 3,928 | constant/string data |
| `.ram.func.table` | `0x20179D00` | 96 | RAM image startup table |

ELF header 的 entry 顯示 `0xC0`，但此平台的 boot image 使用自訂 RAM startup
table。實際啟動函式是 Thumb address `boot_start | 1`，亦即
`0x201755ED`。

## 啟動主流程

`boot_start()` 位於 `0x201755EC`，流程如下：

```text
ROM/boot stage 載入 RAM image
        |
        v
boot_start
        |
        +-- 清除 bootloader BSS / secure BSS
        +-- system、MPU、vector table 初始化
        +-- timer、GDMA、pinmux 初始化
        +-- debug UART 初始化（115200）
        |
        v
boot_load
        |
        +-- 成功：驗證 RAM image signature
        |          清除 exported partition pointer
        |          deinit UART/cache
        |          erase bootloader SRAM footprint
        |          跳到 application entry
        |
        +-- 失敗：印出錯誤
                   進入 "$boot>" shell
```

## `boot_load()` 做的事

`boot_load()` 位於 `0x20175BA0`，由反組譯、symbol 與 debug line information
可確認它負責：

1. 初始化 SPIC/flash。
2. 讀取並驗證 partition table。
3. 根據 partition 狀態選擇 FW1 或 FW2。
4. 取得對應 firmware key table。
5. 初始化 crypto 與 GDMA。
6. 逐一解析 sub-image header 與 section header。
7. 驗證 image/section signature、長度及目的位址。
8. 支援 SHA-256、MD5、HMAC-MD5、HMAC-SHA256 驗證。
9. 支援 AES-CBC、AES-ECB 解密。
10. 設定 XIP SCE remap/decryption。
11. 初始化 LPDDR，並設定 DDR SCE。
12. 將非 XIP section 搬到 SRAM/ITCM/DTCM/DDR 等目的位址。
13. 載入 LS/LP firmware，透過 ICC/GDMA 完成跨 core 啟動資料傳遞。
14. 回傳 application startup table/entry 給 `boot_start()`。

## 正常成功後為何要「擦掉自己」

`boot_start()` 成功載入 application 後，會呼叫 ROM
`hal_flash_boot_stubs.erase_boot_loader()`。

它的目的不是擦 flash 中 `0x9000` 的 bootloader partition，而是清理目前
佔用的 bootloader SRAM 範圍，讓這塊 RAM 能交還 application 使用，然後
轉移控制權到 application。

## UART shell 與 XMODEM

此 AXF 本身沒有連入一份獨立命名的 `XModemTransmit`/`XModemReceive`
實作，但這不代表晶片沒有 UART download：

- `shell_cmd_init()` 會把 command table 設為
  `cmd_shell_stubs.rom_cmd_table`。
- `cmd_shell_stubs` 位於晶片 ROM stub table。
- SDK 的 `fw_img_update_over_uart()` 會轉呼叫 ROM
  `hal_flash_boot_stubs.otu_fw_download()`。
- `fw_uart_boot.h` 定義了 ROM UART boot/XMODEM frame handler 的狀態，
  包含 image header、section header、image data、disconnect 及 RAM image
  load 狀態。

因此 UART download command parser 與主要 frame 處理邏輯是在晶片 ROM，
不是以一般 symbol 的形式編入這個 `bootloader.axf`。

## 錯誤路徑

以下狀況會落入 `$boot>` shell，而不跳 application：

- SPIC 初始化失敗
- partition table 無效
- 找不到可載入 FW
- image/section header 或 signature 無效
- hash/authentication 失敗
- AES/SCE/DDR 配置錯誤
- application startup signature 無效

這個 shell 使用 ROM command table，因此也能成為 ImageTool UART command
的目標端。實際量產時，晶片也可能由 boot strap/ROM startup 直接進入 UART
download mode；這個進入條件需由板級 boot pin 與實機 UART trace 最終確認。

## 與燒錄檔的關係

```text
bootloader.axf
    |
    | ELF2BIN convert (amebapro_bootloader.json)
    v
application_is/boot.bin
    |
    | ELF2BIN combine, BOOT partition @ 0x9000
    v
application_is/flash_is_ota1.bin
```

所以：

- AXF 用於保留 symbol/debug 資訊及提供 ELF2BIN section。
- `application_is/boot.bin` 是真正的 BOOT partition payload。
- `flash_is_ota1.bin` 是可直接燒整片 flash 的完整組合映像。
