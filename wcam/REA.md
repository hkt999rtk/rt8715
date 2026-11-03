# REA 分析紀錄

日期：2026-11-03。依使用者要求使用 reverse-engineer-anything。
本 session 未提供 REA MCP tool，改用已安裝的 REA 6.7.0 公開 CLI；Ghidra 12.1.4 與 Realtek objdump 作為 backend，未修改全域 MCP 設定。

## USB SDK archive 來源

- 原始位置：`/home/kevin/work/8715_bk.dev.20260710/component/soc/realtek/8195b/misc/bsp/lib/common/GCC/lib_usbd.a`
- 本專案：`component/soc/realtek/8195b/misc/bsp/lib/common/GCC/lib_usbd.a`
- 大小：1,793,356 bytes
- SHA-256：`544c6b919206b176b812120500c00489c5f925e1ea15c9c30fccf41df08363c2`
- archive members 日期為 2021-07-16；DWARF 指向 `/cygdrive/d/amebaPro_git/20210716_pro_release/sdk/...`，編譯器為 Realtek GCC 6.4.1 ASDK Build 2778。
- ARMv8-M / softfp ABI、UVC 結構大小與現有 SDK 一致，完成實際連結。符號未見 Carbox、CarPlay、AirPlay、USB Smart。

這些是本機來源與 SDK 相容性的證據，未做供應商數位簽章驗證。使用原 archive，沒有修改其 ELF section 或 relocation。

## 發現與處理

| 觀察 | 證據與實作處理 |
| --- | --- |
| 原生 GET_MAX 使用負索引 | `usbd_event.o` 的 `uvc_events_process` 在 +0x10c 設定 -1，+0x11c 讀取 `uvc_formats[-1]`。以 C 實作取代該 archive member 的唯一 global function，固定單一模式並檢查控制請求。 |
| callback 初始化時序 | `usbd_uvc_init` 清零整個 568-byte context；其後的 callback 呼叫沒有 null guard。以 linker wrapper 在 registration 前設定 callback／probe／commit，最終 ELF 已驗證 wrapper 引用。 |
| USB buffer 所有權 | native queue 會複製 descriptor，完成後回傳內部 descriptor。producer 等待 output semaphore 並取回該指標後才重用編碼 buffer。 |
| 拔除／disable 不保證完成回收 | 原生處理為空操作；新增 disconnect 標記，completion 超時停止 producer，保留 DMA 記憶體至重啟。 |
| GET_INTERFACE 恆回傳 0 | 原生 `fun_get_alt` 不反映串流狀態；改用自訂 get_alt callback。 |
| NV12／H.264 ABI | `h264_initial` inputtype=1 使用 semiplanar，profile=2 關閉 CABAC；採 Baseline／Level 4.0。初始化失敗會自行釋放 context，避免重複 free。 |
| H.264 frame 輸入與 header | `h264_encode` 從 context 取得 Y/UV 位址；成功值為 1。`idrHeader=0` 路徑有保留 64-byte header gap 的複製，採 `idrHeader=1` 由 native encoder 附上 SPS/PPS。 |
| 無 sensor 編碼初始化 | 依 SDK USB camera 範例使用 video system／encoder clock 初始化；不呼叫 isp_init／isp_stream_start。是否能在目標板無 sensor 啟動仍需驗證。 |

USB 詳細 callback、queue 與 alternate setting 另以直接 objdump 交叉檢查，保留 [usbd-uvc-objdump.txt](evidence/usbd-uvc-objdump.txt)；此檔是 shell 工具輸出，不冒充 REA evidence。

## 完整 REA evidence

- [h264-encode.json](evidence/h264-encode.json) — `ev_33ca79b5fb917dc7c696c52fcefdfdad032d0e6596afcc0f4e62c2bc4f77f05a`
- [h264-initial.json](evidence/h264-initial.json) — `ev_beb59ac4a77043cc4410ed6c61bfe5903521a19a8cd96fb8d9199ea8476ee6c7`
- [sdk-usbd-symbols.json](evidence/sdk-usbd-symbols.json) — `ev_b51c21d87b2ed3b1d6d32bd8c108bf829a096091fd9328eab0780ba404517895`
- [uvc-event-assembly.json](evidence/uvc-event-assembly.json) — `ev_fa64e6ac9a708241d5e8d7f08fd13a918309ff9dd58b230a40a29257d7c4f9ab`
- [uvc-event-relocations.json](evidence/uvc-event-relocations.json) — `ev_fd9484477346a0d24b7662885c3cfdefcd0eefa2a487182e14e4af780d0ba94d`

## 可重現命令

以下在本機 toolchain 路徑有效；先以 `arm-none-eabi-ar x` 在暫存目錄抽出目標 archive member。

```sh
export PATH=/home/kevin/.nvm/versions/node/v22.23.4/bin:$PATH
export GHIDRA_INSTALL_DIR=/home/kevin/tools/ghidra_12.1.4_PUBLIC
export REA_OBJDUMP_COMMAND=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-objdump
rea inspect-with-objdump lib_usbd.a symbols --json
rea inspect-with-objdump usbd_event.o disassemble --json
rea inspect-with-objdump usbd_event.o relocations --json
rea decompile h264_encoder.o h264_initial --provider ghidra --json
rea decompile h264_encoder.o h264_encode --provider ghidra --json
```

## 驗證界線

反組譯、decompiler pseudocode、SDK source 與實際 link 互相對照；pseudocode 仍可能有型別推斷誤差。未執行板上 runtime trace，不能據此宣稱 USB 枚舉、傳輸完成時序、SPS/PPS 解碼或 FHD 30 fps 已實測通過。

程式審查另外發現 producer 超過一個幀週期時會多睡一幀，已先用失敗測試重現，再修正排程並通過回歸測試。所有上板檢查步驟見 README。
