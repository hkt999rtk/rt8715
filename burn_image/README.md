# RTL8195B burn image 調查報告

本目錄整理 2026-07-31 對目前 workspace 的燒錄映像、bootloader 與
`ImageTool.exe` UART 協議所做的靜態分析。

## 報告索引

- [01_flash_image_layout.md](01_flash_image_layout.md)
  - `flash_is_ota1.bin`、`firmware_is_ota1.bin`、`ota_app.bin` 的用途
  - flash 分區與 bootloader 在完整映像中的位置
- [02_bootloader_reverse_engineering.md](02_bootloader_reverse_engineering.md)
  - `bootloader.axf` 的 SRAM 配置、啟動流程及主要功能
  - 正常開機、錯誤 shell、映像驗證與跳轉流程
- [03_uart_imagetool_protocol.md](03_uart_imagetool_protocol.md)
  - `ImageTool.exe` 的 UART command 與 XMODEM 呼叫順序
  - 「是否先下載 RAM loader，再下載 flash image」的結論
- [04_immutable_recovery_ota_design.md](04_immutable_recovery_ota_design.md)
  - 6 MiB firmware 空間下的 immutable Recovery + upgradeable Main 架構
  - 非對稱 FW1/FW2 分區、失敗恢復、容量風險與驗證計畫
- [05_runtime_cpu_hotspots.md](05_runtime_cpu_hotspots.md)
  - `AirPlayScreenReceiver`、`rtw_recv_tasklet`、`TCP_IP`、`ScreenThread` 工作內容
  - CarPlay 畫面資料路徑、CPU 成本來源、量測與後續優化優先順序
- [16_wlan_rx_ring_zero_copy_requirements.md](16_wlan_rx_ring_zero_copy_requirements.md)
  - RTL8195B RX descriptor ring、靜態 `rx_ring_pool` 與 pending index 調查
  - RX buffer detach/rearm ownership API、release callback與 profiling 決策條件
- [17_realtek_wlan_rx_zero_copy_compatible_plan.md](17_realtek_wlan_rx_zero_copy_compatible_plan.md)
  - 提供 Realtek RD 的向下相容 WLAN RX ring zero-copy 修改計畫
  - legacy API/ABI、transparent skb backing、fallback、cache、shutdown與測試要求
- [18_airplay_screen_rx_select_read_split_plan.md](18_airplay_screen_rx_select_read_split_plan.md)
  - 調查 `AirPlayScreenReceiver` 現行 select/read/decrypt/callback 同步 loop
  - 256 KiB TCP receive window、producer/consumer 分拆設計、prebuilt library
    可行方案、風險、量測門檻與驗證計畫
- [19_airplay_video_frame_handoff_copy_ownership.md](19_airplay_video_frame_handoff_copy_ownership.md)
  - 還原 video RX source buffer 的 allocate/read/decrypt/callback/free 生命週期
  - 確認交給 `ScreenThread` 前的 full-frame copy、queue 8-byte metadata、GDMA
    wrapper 行為，以及真正 zero-copy 所需的 ownership contract
- [20_airplay_video_handover_pointer_swap_plan.md](20_airplay_video_handover_pointer_swap_plan.md)
  - 精準消除 receiver source 到 `ScreenThread` handover 的 ownership-only copy
  - archive/object relocation、pointer swap transaction、雙 ref lifetime、fallback、
    10 秒 profile、shadow/active rollout 與測試門檻

## 最重要的結論

1. 完整燒錄檔是：

   `project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is_ota1.bin`

2. 此檔包含 partition table、bootloader、FW1 與實際 FATFS 內容。
   bootloader 位於 flash offset `0x00009000`。

3. `bootloader.axf` 是供 linker/debug/ELF2BIN 使用的 ELF，不是直接送入
   flash 的最終格式。ELF2BIN 產生的
   `application_is/boot.bin` 才是完整映像中實際打包的 bootloader payload。

4. 現有 `ImageTool.exe` 的正常單次操作流程是：

   `ping` → `ucfg` → PG command → XMODEM 傳送所選檔案 → `OK` →
   optional hash verify → `disc`

5. 在 `ImageTool.exe` 中沒有發現正常按一次 Download 時，自動先傳一份
   隱藏 RAM loader、執行後再做第二次 XMODEM 傳 image 的流程。它也沒有
   內嵌或隨附另一份 loader binary。

6. UART command/XMODEM 接收能力主要由晶片 ROM command table 提供；
   flash bootloader 在載入失敗時也會把 shell 接到同一份 ROM command
   table。板子在燒錄前仍必須先進入 UART boot/download shell。

## 分析範圍

本次為靜態分析，沒有實體板可做 UART trace。若要確認 boot strap、reset
時序及量產治具是否另外執行兩階段腳本，仍需在實機擷取從 reset 開始的
完整 UART TX/RX。
