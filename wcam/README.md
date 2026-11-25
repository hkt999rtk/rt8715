# wcam firmware: unitest, two TX sources, RX

Build all four images with `./wcam/build-pair.sh all` from the repository root
(`all` is the default). The script cleans **each destination role**, builds LP
with `-j1 -B`, then IS with `-j8`,
checks the linked role/OS ABI/UVC descriptors and publishes only successful
outputs. `./wcam/build-pair.sh unitest`, `tx_colorbar`, `tx_gc2053`, or `rx`
rebuilds one role; `tx` is an alias for `tx_colorbar`, and `both` builds both TX
sources plus RX. Build logs and the
matching ELF/map for fault decoding are retained in `wcam/build-logs/`.

| Image | Role | Data path |
|---|---|---|
| `wcam/flash_wcam_unitest.bin` | Local USB test | Synthetic NV12 720p color bars + center frame counter → SDK H.264 → USB H.264 UVC |
| `wcam/flash_wcam_tx_colorbar.bin` | Wi-Fi station/client | Reversed NV12 720p color bars + `TX FRAME` counter → SDK H.264 → RTP/UDP |
| `wcam/flash_wcam_tx_gc2053.bin` | Wi-Fi station/client | GC2053 sensor → ISP NV12 720p without overlay → SDK H.264 → RTP/UDP |
| `wcam/flash_wcam_rx.bin` | 5 GHz Wi-Fi AP + USB camera | Black `WAITING FOR CONNECTION` screen + frame counter until complete TX SPS/PPS/IDR, then RTP/UDP → H.264 UVC |

Each image has an adjacent `.bin.sha256`. `wcam/flash_wcam_tx.bin` remains an
alias of the color bar TX image. The old `wcam/flash_is.bin` remains the
previous local color-bar POC; use the explicit `flash_wcam_*` names for these builds.

## Pairing and test

- RX AP SSID: **WCAM-RX**, WPA2-AES password: **wcam720p30**, 5 GHz channel **36**.
- TX checks its associated channel and retries if it is not channel 36.
- RX static IP **192.168.4.1**, TX static IP **192.168.4.2**, mask **255.255.255.0**.
- One fixed pair; no DHCP server is needed. TX automatically joins at boot and
  retries indefinitely (1 second between failed connect attempts; the SDK's
  association call has its own timeout). A late join uses the same SSID, channel
  and static-IP checks. Each new connection starts with SPS/PPS and an IDR.
  GC2053 discards queued captures and waits for a fresh frame on connection.
- RTP: UDP **5004**, PT **96**, 90 kHz timestamp, at most **1200 bytes** per RTP
  datagram including its header. RTCP uses UDP **5005**.
- Format remains **1280×720, 30 fps**, Baseline H.264, target 2 Mbps, GOP 30.
  The color bar TX image uses the synthetic source; the GC2053 TX image uses
  the SDK's `isp_gc2053.bin`, sensor port S1 and 27 MHz sensor clock. It has no
  `TX` label or frame count. RX accepts either TX image with no setting change.
- Debug UART: **115200 / 8N1 / no flow control**. Check the boot banner for
  `diag=4 role=TX` or `diag=4 role=RX`, then the `[wcam][wifi]` and `[wcam][net]` logs.

Flash RX and connect its USB device port to Windows. Power TX; it joins RX
regardless of which board starts first. Windows camera name is
**wcam H264 wireless RX**. In **cmd.exe** (binary pipe):

```bat
ffmpeg -hide_banner -f dshow -video_size 1280x720 -framerate 30 -vcodec h264 -i video="wcam H264 wireless RX" -c:v copy -copyinkf -flush_packets 1 -f h264 pipe:1 | ffplay -hide_banner -probesize 32768 -analyzeduration 1000000 -f h264 -framerate 30 -i pipe:0
```

## Transport and memory

The imported MIT source is a selected ireader/media-server subset, pinned to
`c0463893bc509e65bea9f4f76be68c1f89ccdfcf`; see
`project/realtek_amebapro_v0_example/src/wcam/third_party/ireader/README.md`.
TX sends Single NAL/FU-A; RX accepts Single NAL/STAP-A/FU-A. TX currently sends
SPS/PPS separately; automatic STAP-A aggregation is not enabled.

The TX packetizer uses a reusable 1200-byte packet buffer. RX has 16 bounded
reorder slots (20 ms gap deadline), a 100 ms incomplete-AU deadline and three
512 KiB aligned frame buffers. FU fragments are copied directly into the
current Annex B frame buffer; completed buffers are handed to UVC without an
additional frame copy. Buffers are recycled only after USB completion. No
per-packet/frame heap allocation occurs after initialization.

RTCP sends compound SR/RR + SDES, with PLI requests while RX waits for a new IDR.
PLI is sent immediately for a new RTP sequence gap or corruption and repeats
every 500 ms while RX awaits an IDR; encoder force-IDR requests are coalesced
and limited to one per 250 ms within a connection. RX measures the 250 ms
timeout from the arrival of the last forwarded remote frame, including time
spent in the USB queue. It switches back only on a fresh complete SPS/PPS/IDR.
Startup, UVC stream restart, missing/corrupt data,
queue pressure and RTP source changes require a complete SPS/PPS/IDR AU before
forwarding resumes. There is no NACK retransmission or adaptive bitrate in this
first port. RTCP reports use an uptime-based relative clock.

The network task owns the receive assembler and free-frame pool. UVC owns a
submitted frame until completion. On USB timeout/disconnect RX stops UVC and
retains uncertain DMA ownership until reboot, matching the earlier POC behavior.

## Verification and remaining hardware check

`sh wcam/tests/run.sh` exercises the portable transport under ASan/UBSan:
fragmentation, byte-exact reassembly, reordering, sequence wrap, duplicate/lost
packets, malformed/truncated packets, capacity bounds, IDR recovery, PLI routing,
RR loss, trailing non-VCL marker and long-idle recovery. It also round-trips all
270 decodable AUs of the previous board capture byte-for-byte and, when installed,
uses FFmpeg to decode the result with `-xerror`.

Host regressions exercise late Wi-Fi association and stale RTP fragments,
GC2053 probe cleanup and fresh capture after reconnect, UVC session changes,
and RX deadlines across tick wrap and extended idle periods.

The GC2053 variant also needs its sensor module and ISP firmware tested on a
physical TX board. These checks do not replace testing two physical boards. Wi-Fi association,
reconnection, wireless loss behavior, sustained throughput and end-to-end
Windows latency still need that test. During testing collect both UART logs;
`lost`, `late`, `bad`, `pool_drop`, `wait_idr`, `pli`, `send_err` distinguish
network loss, buffering and encoder/USB failures.

---

# wcam H.264 UVC POC

在 RAM 產生 1280×720 NV12 color bar，畫面中心顯示 `FRAME 0000000000`，經 SDK 硬體 H.264 encoder 從 USB UVC 輸出。不呼叫 image sensor / ISP 串流初始化，不連結客戶 `.a`。

| 項目 | 設定 |
| --- | --- |
| 影像來源 | RAM NV12，BT.601 limited-range 八色色條 |
| 解析度／目標幀率 | 1280×720，30 fps |
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

- 大小：1,987,648 bytes
- SHA-256：`682c954be3b709435d92cadd272a4c136f55b66a99aac6daa624dc2e742de618`
- 建置時間：2026-11-03 22:48:22 +0800
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

產物為 `application_is/flash_is.bin`。完整 clean build 已成功，程式審查後的排程、USB alternate setting 修正、UART 診斷 log 、720p 解析度設定、OS timer callback 表修正與 UVC 1.1 Processing Unit descriptor 修正亦完成 IS 重建。SDK 既有編譯警告仍存在。

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
- 診斷統計：不同 tick 頻率／回捲、completion 幀率／碼率、NAL 邊界與重複 slice、事件 buffer 溢位。
- 實際連結 ELF：FS/HS 描述符皆為 H.264 720p／30 fps，callback wrapper 有效，沒有 sensor pipeline 與客戶符號。
- 連結 map：僅 SDK／toolchain archives，沒有客戶 `.a`。
- OS 函式表：實際 ELF 中所有 timer／thread／wakelock／scheduler callback 正確；舊版會在 USB 初始化誤用 timer 名稱為 handle 的錯位已修正。

## UART 診斷 log

UART 設為 **115200 baud、8N1、Flow control=None**；bootloader 與應用程式相同。

此版本開機印出 `diag=3`、編譯日期／時間、影像參數與 buffer 大小。`[stage]` 記錄 pattern、video clocks、encoder hardware、encoder init、USB init/wait、UVC init 與首次 encode；如果卡住，請保留最後一個階段。

串流中約每秒輸出 `[stat]`、`[perf]` 各一行；未串流時約每 5 秒輸出一次。範例數字僅供欄位解讀，並非實機測量：

```text
[wcam][stat] tick=5200 window_ms=1000 active=1 gen=1 next=120 enc=30 sent=30 skip=0 fps=30.0 kbps=2000 size=1500/40000 over=0
[wcam][perf] enc_ms=5/8 usb_ms=3/12 nal_idr=1 nonidr=29 sps=1 pps=1 no_vcl=0 heap=100000/2000000 ctrl=12/2/0/0 trace_lost=0
```

| 欄位 | 意義 |
| --- | --- |
| `tick`, `window_ms` | RTOS tick 與本次統計區間毫秒數；目前 tick_hz=1000。 |
| `active`, `gen`, `next` | 串流開關、啟動串流次數、下一張影像的 counter。 |
| `enc`, `sent`, `skip` | 區間內編碼成功數、USB driver 回報完成數、因停止／重啟串流而未提交 USB 的影像數。 |
| `fps`, `kbps` | 以 USB driver completion 計算的幀率及 H.264 payload 碼率；不含 USB header，也不表示主機應用已成功解碼。 |
| `size` | 區間內編碼影像的最小／最大 bytes。 |
| `over` | 本區間沒有剩餘幀週期可等待的次數，包含剛好到 deadline 的情況；不是掉幀數。 |
| `enc_ms`, `usb_ms` | 平均／最大耗時；前者量測 h264_encode，後者涵蓋提交 USB queue 到取回完成 descriptor。整數毫秒，低於 1 ms 會顯示 0。 |
| `nal_idr`, `nonidr`, `sps`, `pps` | 編碼影像內找到該 Annex B NAL 類型的影像數；同張影像多個 slice 只算一次。`nonidr` 是 type 1 slice，沒有解析 slice header 判斷 P/B。 |
| `no_vcl` | 編碼影像中未找到 type 1／5 NAL 的次數；非零時需檢查格式與第一幀 hex header。 |
| `heap` | SDK heap 0／heap 1 當下剩餘 bytes；不是歷史最低值。 |
| `ctrl` | 開機至今 SETUP／有效 SET_CUR data／STALL／EP0 queue error 的累積次數。 |
| `trace_lost` | 開機至今因 32 筆事件 buffer 已滿而省略的事件數。 |

`[ctrl]` 顯示主機實際協商請求，`selector=1` 為 PROBE、`2` 為 COMMIT；SET 紀錄同時保留主機要求的 format、frame、interval，之後正規化為固定 720p／30 fps。`[usb]` 記錄啟停、拔除及 USB speed（0=未確定、2=Full Speed、3=High Speed）。

每次串流啟動的 `[first]` 印出第一張編碼影像的前 16 bytes 與 `nal_flags`：bit 0=non-IDR slice、bit 1=IDR、bit 2=SPS、bit 3=PPS，`e` 代表 IDR+SPS+PPS。它只檢查 NAL header，不是解碼驗證。

USB callbacks 不做 UART 輸出，只存入固定大小的事件 buffer。正常每秒最多輸出 8 筆事件，啟動／停止時最多輸出 32 筆。事件可能延後列印；請依事件順序解讀。統計使用實際經過時間，console 輸出的成本會反映到後續區間。encoder 若完全卡死，單一 producer 也會停止輸出統計，此時最後的 `[stage]`／`[first]` 是判讀依據。

錯誤會包含 `[error]` 與 `[stop]`：失敗階段、回傳值、frame、bytes、等待 tick、USB 狀態。SDK 呼叫錯誤保留其回傳值；本程式偵測到配置、無效編碼長度或 queue／timeout 失敗時用 `rc=-1`。USB completion 超時仍保留 DMA buffer，不為了 log 改變其所有權。

**回報問題時：** 請提供從開機到異常後的完整 UART 文字（含至少 10 秒統計）、燒錄檔 SHA-256、主機 OS／接收命令，以及是否曾停止串流或拔插 USB。若主機能擷取，也請附一小段原始 `.h264`；這樣可對照 firmware throughput 與主機解碼結果。

## 上板驗證

使用者已上板回報舊版在 USB 初始化發生 UsageFault，根因與修正見 [REA.md](REA.md#2026-11-03實機-usb-初始化-usagefault)。`diag=2` 已由使用者上板確認通過 USB 初始化且持續運行超過 70 秒；Windows 可辨識 USB Composite Device，但 Camera 子裝置為 Error。本版 `diag=3` 修正 UVC 1.1 Processing Unit 缺少 bmVideoStandards 的 descriptor 錯誤；使用者後續確認 Windows usbvideo 驅動 ProblemCode=0，DirectShow 可辨識 H264 1280×720／30 fps；約 10 秒串流 UART 顯示 High Speed、每秒 30 個 encode／send 與 1 IDR＋29 P。實際捕獲檔的分析如下，Windows 設定／內建相機預覽仍未成功。

1. 燒錄本目錄的 `flash_is.bin`，接上 USB device port 至主機，再重新開機；保留 UART log。
2. Linux 主機執行 `v4l2-ctl --list-devices` 找出節點，再用 `v4l2-ctl -d /dev/videoX --list-formats-ext` 確認 H264／1280×720／30 fps。以下請替換 `/dev/videoX`。
3. 使用支援 H.264 UVC 的接收工具，例如：

```sh
ffplay -f v4l2 -input_format h264 -video_size 1280x720 -framerate 30 /dev/videoX
```

4. 擷取 300 frames 並檢查解碼結果與 I/P 排列：

```sh
v4l2-ctl -d /dev/videoX --set-fmt-video=width=1280,height=720,pixelformat=H264 --set-parm=30 --stream-mmap=4 --stream-count=300 --stream-to=wcam.h264
ffprobe -v error -select_streams v:0 -show_entries frame=key_frame,pict_type,width,height -of csv=p=0 wcam.h264
ffmpeg -v error -i wcam.h264 -f null -
```

確認八色色條、中央遞增 counter、每 30 frames 一個 IDR，以及啟動串流時何時收到第一組 SPS/PPS／IDR。上述 Linux 接收命令尚待 Linux 上板驗證。

**目前 POC 限制：** USB 拔除或 completion 超時後，保留可能仍被 DMA 使用的 buffer 並停止 producer；重新接線後需重新開機。單一 buffer 串行編碼／傳送，約 10 秒 Windows 實測已達約 30 fps，長時間穩定性尚待驗證。尚未驗證跨作業系統相容性或 Full Speed 效能。


## Windows 實機捕獲與預覽

2026-11-03 使用者提供 [wcam-debug.h264](evidence/board-capture-2026-11-03/wcam-debug.h264)，89,240 bytes，SHA-256 `34ab7ba88a42cbbd43b0a5f25cf52fcb06b35193fdc3841fbbe39981736abbc3`。

- Annex B：290 個 P slice、9 組 SPS／PPS／IDR。檔頭是 29 個沒有參數集的 P frame；第一組 SPS 位於 byte 3793。
- 從第一組 SPS/PPS／IDR 起，270 張影像解碼成功，沒有解碼錯誤；9 組 GOP 均為 1 IDR＋29 P。
- 以相同 5×7 字型辨識解碼影像中央計數器，從 126 到 395 全部連續，沒有重複或跳號；見 [分析結果](evidence/board-capture-2026-11-03/counter-analysis.json)。
- [實機解碼畫面](evidence/board-capture-2026-11-03/frame-126.png)；[可播放 MP4](evidence/board-capture-2026-11-03/verified-720p30.mp4) 保留原始編碼，只移除檔頭 29 個無參考 P frame，並依設定與 UART 的 30 fps 補上時間戳，長度 9 秒。裸 H.264 本身沒有 DirectShow 封包時間戳或 keyframe 標記，不能從它還原主機原始 flags。
- 這份樣本證明硬體 H.264、USB 出流、色條與 counter 可正常解碼；不能宣稱啟動首個 GOP 完整接收、Windows 內建相機預覽或長時間串流已通過。

Windows 內建預覽仍啟動失敗。接下來以 FFmpeg DirectShow 選取 H.264 並交給 FFplay 即時解碼；此命令待使用者在 Windows 確認：

可下載並雙擊 [preview-windows.cmd](preview-windows.cmd)。它會檢查 FFmpeg／FFplay 是否可執行，並透過 cmd 的二進位管線啟動預覽。此腳本尚待 Windows 實機驗證。

在 **cmd.exe** 執行（避免舊版 Windows PowerShell 的文字管線改寫二進位資料）；先關閉其他相機程式：

```bat
ffmpeg -hide_banner -f dshow -video_size 1280x720 -framerate 30 -vcodec h264 -i video="wcam H264 color bar" -c:v copy -copyinkf -f h264 - | ffplay -hide_banner -f h264 -framerate 30 -i -
```

捕獲原始 H.264 則可在 PowerShell 執行：

```powershell
ffmpeg -hide_banner -f dshow -video_size 1280x720 -framerate 30 -vcodec h264 -i 'video=wcam H264 color bar' -t 10 -c:v copy -copyinkf wcam-debug.h264
```

`-copyinkf` 允許複製起始未標記為 keyframe 的封包，便於診斷。它不會補出缺少的參數集或修復損壞影格。來源：[FFmpeg 文件](https://www.ffmpeg.org/ffmpeg.html)。啟動時若檔案由 P frame 開始，必須等到下一組 SPS/PPS／IDR 才能解碼；本樣本第一個 GOP 之後解碼正常。
