# wcam H.264 UVC POC

在 RAM 產生 1920×1080 NV12 color bar，畫面中心顯示 `FRAME 0000000000`，經 SDK 硬體 H.264 encoder 從 USB UVC 輸出。不呼叫 image sensor / ISP 串流初始化，不連結客戶 `.a`。

| 項目 | 設定 |
| --- | --- |
| 影像來源 | RAM NV12，BT.601 limited-range 八色色條 |
| 解析度／目標幀率 | 1920×1080，30 fps |
| 編碼 | H.264 Baseline Level 4.0，CBR 2 Mbit/s |
| GOP | 30：1 個 I／IDR + 29 個 P，沒有 B frame |
| IDR header | 每個 IDR 附 SPS/PPS；啟動串流時強制 IDR |
| USB 格式 | UVC 1.1 frame-based H.264，單一解析度與幀率 |
| Counter | 開機從 0 開始，每次產生影像加 1，32-bit 回捲 |

Counter 計算的是產生的影像；若主機在編碼途中停止串流，該影像不送出，因此接收端可能看見跳號。

![色條與計數器預覽](preview.png)

此圖由相同 C pattern generator 在主機產生 NV12 後轉成 PNG，**不是實機 UVC 擷取畫面**。

## 韌體

[flash_is.bin](flash_is.bin) 是包含 SDK bootloader 與 IS 韌體的完整 flash 映像。

- 大小：1,984,256 bytes
- SHA-256：`2d67cd9d72dd48376b9abb357001ff6b34c3982d4400b4a06f09f57997980cd2`
- 建置時間：2026-11-03 13:03:13 +0800
- 原先的最小 SDK 範例保留在 git commit `021e06d`。

新增的 `lib_usbd.a` 取自本機原廠 SDK 備份；來源、雜湊與逆向證據見 [REA.md](REA.md)。

## 建置

在 `project/realtek_amebapro_v0_example/GCC-RELEASE` 依序執行，禁止 parallel `make all`：

```sh
PREFIX=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-
make clean CROSS_COMPILE="$PREFIX"
make -j1 -B ram_lp CROSS_COMPILE="$PREFIX"
make -j8 ram_is CROSS_COMPILE="$PREFIX"
```

產物為 `application_is/flash_is.bin`。完整 clean build 已成功，程式審查後的排程與 USB alternate setting 修正亦完成 IS 重建。SDK 既有編譯警告仍存在。

若使用需要 `gnu_utility/vfs.py` 的 SDK 打包設定，重建前先確認 `python3 -c 'from pyfatfs.PyFat import PyFat; import littlefs'`；缺少時依根目錄 AGENTS.md 安裝至暫存目錄並設定 PYTHONPATH。

## 主機驗證

從 repository 根目錄執行：

```sh
sh wcam/tests/run.sh
python3 wcam/tests/verify_elf.py project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/Debug/bin/application_is.axf
(cd wcam && sha256sum -c flash_is.bin.sha256)
```

已通過：

- ASan/UBSan：色條 Y/UV、counter 更新／回捲／清除與邊界。
- UVC probe/commit：GET_MIN/MAX/DEF/CUR、錯誤索引、長度及短回覆。
- 幀率排程：編碼逾時不多等一幀、小數 tick 累積、tick 回捲。
- 實際連結 ELF：FS/HS 描述符皆為 H.264 FHD／30 fps，callback wrapper 有效，沒有 sensor pipeline 與客戶符號。
- 連結 map：僅 SDK／toolchain archives，沒有客戶 `.a`。

## 上板驗證

尚未燒錄實機；USB 列舉、硬體編碼、可解碼的 SPS/PPS、實際 GOP 與穩定 30 fps 都待確認。

1. 燒錄本目錄的 `flash_is.bin`，接上 USB device port 至主機，再重新開機；保留 UART log。
2. Linux 主機執行 `v4l2-ctl --list-devices` 找出節點，再用 `v4l2-ctl -d /dev/videoX --list-formats-ext` 確認 H264／1920×1080／30 fps。以下請替換 `/dev/videoX`。
3. 使用支援 H.264 UVC 的接收工具，例如：

```sh
ffplay -f v4l2 -input_format h264 -video_size 1920x1080 -framerate 30 /dev/videoX
```

4. 擷取 300 frames 並檢查解碼結果與 I/P 排列：

```sh
v4l2-ctl -d /dev/videoX --set-fmt-video=width=1920,height=1080,pixelformat=H264 --set-parm=30 --stream-mmap=4 --stream-count=300 --stream-to=wcam.h264
ffprobe -v error -select_streams v:0 -show_entries frame=key_frame,pict_type,width,height -of csv=p=0 wcam.h264
ffmpeg -v error -i wcam.h264 -f null -
```

確認八色色條、中央遞增 counter、每 30 frames 一個 IDR，以及開始串流可立即解碼。上述接收命令是待上板執行的測試步驟。

**目前 POC 限制：** USB 拔除或 completion 超時後，保留可能仍被 DMA 使用的 buffer 並停止 producer；重新接線後需重新開機。單一 buffer 串行編碼／傳送，30 fps 是目標值，無法由編譯與主機單元測試保證。尚未驗證跨作業系統相容性或 Full Speed 效能。
