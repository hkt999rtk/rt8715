# `ImageTool.exe` UART/XMODEM 協議分析

分析對象：

`tools/AmebaPro/Image_Tool/ImageTool.exe`

檔案是 .NET Framework 4.5 WPF 程式，版本資訊為
`FirmwareDownloader 1.0.0.0`。本報告由 .NET metadata、BAML 字串與 CIL
method body 靜態反組譯得到。

## UI mode 與 command

目前 UI 的四個 mode 依序為：

| UI index | UI 顯示 | 送到 target 的 command |
|---:|---|---|
| 0 | `1. Program flash` | `fwd <flash_io> <flash_pin> <offset>\n` |
| 1 | `2. Program bootloader` | `otu1 <flash_io> <flash_pin>\n` |
| 2 | `3. Update Firmware` | `otu2 <flash_io> <flash_pin> <fw_idx+1>\n` |
| 3 | `4. Update Firmware (auto select)` | `otu2x <flash_io> <flash_pin>\n` |

`HandlePGcommand()` 另保留兩個 switch case：

- index 4：`fwdram <address>\n`
- index 5：`uboot\n`

但這兩個項目沒有出現在目前 BAML 的 mode 下拉選單中，應視為保留或舊版
功能，不能當成目前正常 UI 燒錄流程的一部分。

## `download_start()` 流程

```text
開啟 UART：115200, 8N1
        |
        v
送 "ping\n"
讀回 4 bytes，必須匹配 "ping"
        |
        v
送 "ucfg <baud> <parity-index> <flow-index>\n"
        |
        v
關閉 UART，以新設定重新開啟
讀回 "OK"
        |
        v
啟動 BackgroundWorker
        |
        v
bw_DoWork()
```

任一 UART open、ping、config 或 ACK 失敗都會中止。

## `PG_progress()` 流程

```text
HandlePGcommand()
    |
    +-- 先送 fwd / otu1 / otu2 / otu2x / fwdram / uboot
    |
    v
讀取使用者選擇的檔案
    |
    +-- optional Skip(offset)
    +-- optional Take(length)
    |
    v
XmodemTransmit(payload)
    |
    v
等待 target 回覆 "OK"
```

XMODEM 支援：

- target 送 `C` 時使用 CRC16
- target 送 `NAK` 時使用 checksum
- SOH 128-byte frame
- STX 1024-byte frame
- ACK/NAK/CAN/EOT
- 最多 16 次 retry

## Hash verify

勾選 verify 時，工具在 XMODEM 與 flash write `OK` 之後送：

```text
hashq <length> <flash_io> <flash_pin>\n
```

接收 38 bytes response，取其中 32 bytes remote hash，與本機 SHA-256 比較。
最後送：

```text
disc\n
```

`fwdram` 保留模式的 `disc` 還可附帶 HS/LS RAM address。

## Skip Wi-Fi calibration 的特殊行為

只有 `Program flash`、有指定 offset、並勾選 skip Wi-Fi calibration 時，
同一次工作會做兩次 `PG_progress()`：

1. 傳來源檔案 `[0x0000, 0x2000)`。
2. 將 flash offset 改成 `0x8000`，傳來源檔案 `[0x8000, EOF)`。

所以它跳過 `0x2000` 到 `0x7FFF`。這兩次 XMODEM 都是同一份所選 flash
image 的不同區段，不是「loader + firmware」兩種 payload。

## 是否先傳 RAM loader，再傳 image？

對目前這份 `ImageTool.exe`，答案是：**正常 UI 燒錄流程沒有這樣做。**

靜態證據：

1. `download_start()` 在 UART 設定完成後只啟動一次 `bw_DoWork()`。
2. 每個 `PG_progress()` 都是先送 command，接著立刻把使用者選擇的檔案
   交給 `XmodemTransmit()`。
3. 工具目錄只有 `ImageTool.exe` 與 `Newtonsoft.Json.dll`，沒有另一份
   loader binary。
4. PE resources 沒有足以容納此平台 bootloader 的隱藏大型 binary。
5. 正常非 skip-calibration 路徑只有一次 XMODEM。
6. target 端 SDK 明確提供 ROM `otu_fw_download()` 與 ROM command table，
   不需要 PC 先下傳一個 flash writer 才能接收 `fwd`。

因此正常 `Program flash` 應理解為：

```text
板子先進入 ROM/UART download shell
        |
        v
PC 送 fwd command
        |
        v
ROM 端以 XMODEM 接收所選 flash image
        |
        v
ROM 端寫入 flash
```

`Program bootloader` 則是以 `otu1` command，把使用者選擇的 bootloader
檔案本身作為該次唯一的 XMODEM payload。它不是在同一次點擊中先把這個
bootloader 放到 RAM 執行，再自動傳第二份 full image。

## 仍可能存在的「兩階段」情境

下列情境仍可能由其他治具或人工步驟組成兩階段流程：

- 先用 `fwdram` 或 `uboot` 保留命令載入 RAM image，再另外執行一次
  `Program flash`。
- 量產治具在啟動 `ImageTool.exe` 之前，另外送一份 loader。
- 使用不同版本的 ImageTool 或 command-line downloader。

但這些都不是目前這份 `ImageTool.exe` 正常四個 UI mode 自動執行的流程。

## 實機確認建議

從 reset 前開始擷取 UART TX/RX，確認：

1. boot strap/reset 後第一個 target prompt 或 response。
2. 第一個 host command 是否是 `ping`。
3. `ucfg` 後的實際 baud rate。
4. XMODEM 開始前的 PG command 是 `fwd`、`otu1`、`otu2` 或 `otu2x`。
5. 一次點擊中出現幾次 `C`/SOH/STX/EOT sequence。
6. 是否存在 ImageTool process 以外的治具程式先傳資料。

若 trace 是 `ping → ucfg → fwd → XMODEM`，即可直接確認是 ROM downloader
接收 full image，而不是 hidden RAM loader 兩階段流程。
