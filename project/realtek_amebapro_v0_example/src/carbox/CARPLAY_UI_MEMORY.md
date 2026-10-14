# CarPlay UI H.264 memory change

## 2026-10-14：已整合的測試韌體

本節為目前實作。後面保留舊版問題與推導，避免把分析階段與實作混淆。
板上相容性仍待實機確認；以下驗證使用實際 vendor ARM archive，在 QEMU 執行。

### 目前流程

1. `Accessory.o` 保留原本 UI 繪圖，main surface 使用 RGB565。
2. 繪圖結束的 tail shim 呼叫 `carbox_ui_encode_surface(surface, width, height, fps, filename)`。
   取得 pixel buffer 所有權，清掉 surface 的 pixel pointer，再刪除 surface。
3. 兩列 scratch＋bitmap 原地轉 compact I420，再向後展開為 x264 的 padded I420。
   2560×1440 保留原 RGB allocation 7,372,800 bytes；scratch 僅 10,780 bytes。
   padded layout 使用 6,003,712 bytes。沒有額外完整 RGB 或 I420 配置，也不使用 realloc 縮小。
   很小的影像若 RGB allocation 放不下 padding，會檢查配置另一小塊 padded buffer。
4. 直接呼叫 core API：single thread、ref=1、B=0、subme=0、CQP=23、scenecut/mbtree/lookahead=0、
   CABAC。fps denominator 設為 0，與原 UI 輸出的 SPS timing 格式一致；requested dimensions
   保留，16 對齊由 SPS cropping 處理。
5. 受 archive SHA 約束的 `x264_frame_new` 三個 plane allocation call sites，
   讓第一張的 fdec 與 fenc **共用**上述 padded I420 buffer；picture copy 僅設定
   metadata。x264 仍執行完整 IDR intra reconstruction，但沒有第二份 fdec pixels。
6. 第一張明確產生 IDR。解析實際 SPS/PPS，確認 progressive I420/CABAC、POC type 0 等條件。
   NAL payload 必須連續且完全位於追蹤的 allocation；轉移該 allocation 的所有權，避免另複製 IDR。
7. IDR 完成後立即關閉 x264，釋放 fenc/fdec、原 RGB allocation 與 core 遺漏的
   CQM allocation。獨立 CABAC skip writer 只使用 archive 的 CABAC arithmetic primitives，
   不建立 encoder 或 reference frame。每張 P-skip 有新的 frame_num/POC；20 張 NAL
   暫存於 40 KB 有界緩衝，再與 IDR 一起寫入原 UI 檔案。
8. 最後釋放持有的 NAL allocation。檔案 open/write/close 結果均有 log。

### OOM / closed library 所有權

`patch_ui_x264_archive.py` 產生衍生 archive，改名 raw malloc/free/strdup，並將
x264_malloc/free 定義設為 weak，讓 local 強符號能攔截同一個 `.o` 內的呼叫。
原 vendor archive 不修改。`patch_carplay_ui565_archive.py` 以整個 CreateUi section SHA、
指令、relocation 與 x264 archive SHA 保護 ABI；只停用被取代 tail 的 relocation，
保留繪圖、literal、epilogue 與其他 relocation，bridge BL 使用新目標。

所有 core allocation 以 task owner 追蹤；其他 task 的 allocator pass through。
這條路徑固定 single-thread，archive 沒有 pthread 依赖。配置失敗時從 allocator
`longjmp` 回 transaction boundary，直接釋放已登記 raw allocations，
不走 vendor 不安全的 partial-init cleanup。512-entry 表在 LPDDR BSS，約 4.3 KB。
連續重入會記錄 busy 並刪除該次 surface。此機制限定已驗證 archive / single-thread UI 用途。

### 已完成驗證

`python3 project/realtek_amebapro_v0_example/src/carbox/tests/run_carplay_ui_tests.py`

- ASan/UBSan：RGB565、BGR888 多尺寸原地轉換與原 converter byte-for-byte 一致；padding 正確。
- 實際 ARM library：32×32、1920×1080、2560×1200、2560×1440。
  每個案例重複兩次，全部 allocation 釋放，尾端 guard 無越界。
- FFmpeg 解碼：每個輸出 21 張、尺寸正確、21 張 decoded frame hash 一致。
- 上述尺寸加上 2560×1440 隨機圖的**完整 bitstream**，均與未共用 fdec 且使用
  原 vendor skip helper 的基準輸出逐 byte 相同。
- 32×32、320×240、1920×1080、2560×1200、2560×1440 的 20 個 P NAL
  均與原 vendor CABAC skip helper 輸出逐 byte 相同。
- 2560×1440 隨機雜訊圖：也通過解碼、guard、cleanup；bitstream 可超過舊 307,200-byte 限制。
- 320×240 對全部 82 個配置點逐一注入 OOM，各重複兩次：安全退出、無配置殘留。
- 韌體 linked disassembly 確認 CreateUi 呼叫 local bridge；fenc hooks 與 strong allocator 已生效。

| 測試輸入 | QEMU 追蹤的 UI 峰值 bytes |
|---|---:|
| 1920×1080 黑白棋盤 | 8,953,645 |
| 2560×1200 黑白棋盤 | 11,858,245 |
| 2560×1440 黑白棋盤 | 13,670,845 |
| 2560×1440 隨機 RGB565 雜訊 | 22,222,002 |

這些數字包含 UI 像素與 core malloc requests，**不含其他韌體任務、heap metadata、
allocator table、stack、測試 guard**。複雜內容會使 x264 bitstream buffer 成長；
不能用 13.67 MB 當作所有圖片的上限。高熵圖片在板上可能仍 OOM，但會安全退出並記錄原因，
不會自動降解析度或降低 QP 品質。失敗時可能沒有新的 UI 檔案；既有檔案不會在編碼失敗前覆寫。
fdec 與 fenc 共用同一份 pixels，使第一張 IDR 的峰值也降低約 6 MB；
獨立 skip writer 則讓這份 pixels 在產生 P-frame 前釋放。

### 板上測試

編譯開關沿用 `CARBOX_UI565_ZERO_COPY=1`（預設）；設為 0 可恢復 vendor UI/x264 路徑。
請測試 2560×1440 的首次啟動與重連，確認 UI 畫面、色彩、skip 延續與後續 CarPlay 視訊。
RGB565、CQP23 會改變 IDR 壓縮輸出與畫質，實機需確認。
預期 log：

```text
[UI MEM] start 2560x1440 ... bpp=2 rgb=7372800 scratch=10780
[UI MEM] before_open heap0_free=... largest=... blocks=...
[UI MEM] fdec shares I420 allocation
[UI MEM] fenc shares RGB allocation, padded=6003712
[UI MEM] after_IDR heap0_free=... largest=... blocks=...
[UI MEM] before_skip_no_reference heap0_free=... largest=... blocks=...
[UI MEM] IDR=... skip20=2260; reference freed before skip
[UI MEM] saved ... bytes=... skip=2260 status=OK
[UI MEM] done heap0_free=... largest=... blocks=...
```

若失敗，請保留 `[UI MEM] abort ...`、heap free/largest 與後续 fault log。

## 舊版 RGB565＋picture 借用實作（歷史紀錄）


The customer `lib_SystemLib.a(Accessory.o)` and `lib_x264.a(x264.o)` have no
matching C sources in this repository. The implementation in
`carplay_ui_memory.c` is tied to the bundled x264 ABI 76 archive. Build with
`CARBOX_UI565_ZERO_COPY=0` to restore the unmodified vendor UI and x264 paths.

At 2560×1440 the old path allocated an 11,059,200-byte RGB888 surface, a
5,529,600-byte I420 image, then another 5,529,600-byte x264 input picture.
`Accessory.o` frees the RGB surface before x264 initialization. The observed
failure was the second I420 allocation: only 2,501,568 bytes of heap0 were
free, with a 2,343,168-byte largest block. This was capacity exhaustion at
that point, not fragmentation.

The derived SystemLib archive changes the main surface format from 0 (RGB888)
to 2 (RGB565), and passes 2 bytes per pixel to `RGB2YUV`. A strong local
`RGB2YUV` converts RGB565 to I420 with the bundled routine's BT.601 limited
range coefficients and top-left chroma sample. The temporary main surface is
7,372,800 bytes, saving 3,686,400 bytes while rendering and converting.

The x264 hooks fill the ABI 76 picture descriptor without allocating another
pixel buffer. Before the first encode call they point its Y/U/V planes at the
I420 image owned by `Accessory.o`. The legacy front end then calls `memcpy`
with identical source and destination pointers; the project's memcpy wrapper
returns immediately for that case. The picture cleanup hook leaves the
borrowed I420 image alone, and `Accessory.o` frees it after encoder exit.
This removes the failed 5,529,623-byte `x264_malloc` request.

`tools/patch_carplay_ui565_archive.py` creates a copy of the vendor archive,
checks the exact instruction bytes and call relocations, then edits only the
two immediate values and weakens the vendor `RGB2YUV` definition. It edits ELF
section bytes in place. `objcopy --update-section` must not be used here: it
silently drops the function section's relocations, leaving invalid linked
branch targets. The script compares relocations before and after and checks
the bundled `lib_x264.a` SHA-256 because the picture descriptor offsets depend
on that exact ABI.

Host conversion check:

```sh
gcc -std=c99 -Wall -Wextra -Werror -DCARBOX_UI_MEMORY_HOST_TEST \
  project/realtek_amebapro_v0_example/src/carbox/carplay_ui_memory.c \
  project/realtek_amebapro_v0_example/src/carbox/tests/carplay_ui_memory_host.c \
  -o /tmp/carplay_ui_memory_host
/tmp/carplay_ui_memory_host
```

Initial board acceptance criteria were: at 2560×1440, confirm the RGB allocation is
7,372,800 bytes, the 5,529,623-byte x264 picture request is absent, no heap
allocation or RTOS assert follows, and the saved H.264 stream decodes to the
expected dimensions and visible UI. The image can change slightly because
RGB565 has lower color precision. The closed UI caller has no error check for
x264 initialization or output buffer capacity, so malformed bitstreams must
be treated as a failed board validation, even if the device remains running.

## 2026-10-14：實作前的板上 log 與 memory review

**當時的 RGB565＋picture 借用版未通過 2560×1440 板上驗證。** RGB565 與借用輸入 I420 已生效，
但仍在第一張實際影格的內部配置階段 OOM。以下為當時的分析；目前實作狀態請看文件開頭。

### 實際用途與生命週期

由目前 `application_is.asm` 的 `CAccessory_CreateUi` 確認：

1. 建立 RGB565 UI，配置 7,372,800 bytes。
2. 配置 compact I420，5,529,600 bytes，執行 RGB2YUV。
3. `CSurface_delete` 釋放 RGB surface 的實際 pixel buffer，再進入 x264。
4. 配置 307,200-byte H.264 輸出區。
5. `lib_x264_init` → `x264_encoder_open_76`。此時已配置 fdec 重建畫面。
6. `lib_x264_encode(3, ...)` 將 picture type 設成 AUTO，第一張產生 IDR。
   `x264_encoder_encode` 另外建立 fenc，複製輸入 I420。
7. 呼叫 `lib_x264_encode(0, ...)` **20 次**。這個 vendor API 的 type 0
   直接走 `x264_encoder_skip_pframe`，不是再編碼 20 張 YUV，也不是 flush。
8. `lib_x264_exit`，儲存 H.264，再釋放輸出區與 UI I420。

因此 RGB565 節省的是繪圖／轉換階段的峰值；因 RGB 在 x264 open 前已釋放，
這 3.69 MB 不能再算成編碼階段的節省量。現有 ZERO_COPY 只消除了前端
`x264_picture_alloc` 的 5.53 MB，尚未消除 core 的 fenc copy。

新 log 的兩筆失敗均位於 fenc 的配置流程：

| 配置 | 計算／用途 |
| --- | --- |
| 3,946,496 bytes | `(2560 + 64) × (1440 + 64)`，fenc 的 padded Y plane |
| 4,214,784 bytes | `4 × (1280 + 64) × (720 + 64)`，四個 lowres 預分析 Y plane |
| 4,214,807-byte malloc | 上一筆加 x264 allocator 的 23-byte 對齊保留空間 |

fenc 的 U/V 在 Y 之前各配置 1,028,608 bytes，所以一組 padded YUV 為
6,003,712 bytes。fdec 也有這組 pixel planes，另外有 motion vector、reference
index 與其他 metadata。fdec 已在 open 配好，不是等這兩筆成功後才開始配置。
第二筆失敗時 heap0_free=434,944、largest=371,904，確定連總量也不足。

### 用原 ARM archive 做 QEMU 實測

測試直接連結 repo 的 `lib_x264.a`，沒有以新版 x264 代替。
SHA-256：`e4a2c837959c7333c4b12cd692825e078365a8d85b86961d57aff8aa4e034cb4`。
使用 Realtek GCC、QEMU user-mode、newlib syscall shim，攔截配置／釋放並加尾端
canary。輸入為 2560×1440 checkerboard I420，picture 直接借用它。

下表是活躍配置的 requested bytes，包含 x264 的對齊保留空間與 UI I420，
不含測試器 guard/header、FreeRTOS block overhead、307,200-byte UI 輸出區、
其他 task、stack、靜態區。**它不是板上的全系統用量，也不是任意畫面的最壞上限。**

| 設定 | open 後 | 第一張 encode 後 |
| --- | ---: | ---: |
| 目前 vendor 設定 | 19,591,474 | 30,238,119 |
| CQP 23、關閉 lowres 條件 | 17,821,327 | 23,835,207 |
| 上一項再關 CABAC，使用 CAVLC | 15,963,658 | 21,977,538 |

目前 vendor 已是 threads=1、ref=1、bframes=0、subme=0、rc-lookahead=0。
validate 後 mbtree=0，但 rc_method=ABR、scenecut=40，仍會設定 b_have_lowres=1。
所以再設一次 lookahead=0 不會消除 4.21 MB。

CQP 實驗使用 `x264_param_parse` 設定：
`qp=23, scenecut=0, b-adapt=0, mbtree=0, rc-lookahead=0, ipratio=1, pbratio=1`。
實測 lowres=0，QP 範圍變成 23–23。這是品質／碼率策略的改變，QP 23 只是
實驗值；不能只把原本 `i_qp_constant=0` 配上 CQP，否則會進入 lossless 模式。

減少的 6,402,912 bytes 可拆成：

- lowres pixel planes：4,214,807 bytes（含 alignment reserve）。
- lowres 與 AQ 等相關配置：417,958 bytes。
- MV cost tables 從 29 張減為 2 張：省 27 × 65,561 = 1,770,147 bytes。

關 CABAC 另省 1,857,669 bytes，主要是兩張 921,600-byte MVD cache 和
14,400-byte chroma prediction mode cache，加上 allocator reserve。
此舊版即使 bframes=0，CABAC cache init 仍配置兩張 MVD cache。

### skip frame 與 reference 的界線

後續全畫面 skip 可以只產生 H.264 語法，不需要 encoder 保存畫面 pixel。
**decoder 仍須保留解碼後的 reference**，SPS/PPS 和 slice 的參照設定不能刪掉。
不能把同一包 skip NAL 原封不動重送：frame_num、POC、參照管理須隨序列前進。

已完成獨立 proof：取原 library 在 CAVLC 模式產生的第一張 IDR，另產生 20 張
P-skip slice。每張使用一個 `mb_skip_run=160×90=14400`，依實際 SPS/PPS 的
12-bit frame_num、13-bit POC 與其他欄位編寫 header，加入 RBSP trailing bits
及 emulation-prevention bytes。此測例每張 13 bytes，20 張共 260 bytes。
FFmpeg 解碼得到 21 張 2560×1440 畫面，frame MD5 全部相同，decode error 為 0。
這個 proof 限定該組 SPS/PPS、20 張及 progressive CAVLC；還不是通用產生器，
未驗證 frame_num/POC wrap、其他解析度或車機相容性。

當時 vendor skip 函式仍呼叫 `x264_macroblock_cache_load/save`，會讀 fenc、
寫 fdec。上文的獨立 CABAC writer 已取代它，因此第一張完成時即可 close encoder。
第一張 IDR 的 intra prediction 仍用到相鄰重建像素；不能直接把 ref 設成 0。
當時尚未驗證 fdec/fenc 共用，所以上述 CAVLC proof 只能省 IDR 之後的記憶體。
目前上文的實作已在受 SHA 保護的 call sites 共用兩者的 pixel planes，並以
多尺寸、隨機圖的完整 bitstream 逐 byte 比對確認第一張沒有改變。

### Review 另發現的具體問題

1. **vendor CABAC skip 的 NAL escaping 配置不足。** 函式配置 raw payload + 5，
   卻以 `x264_nal_encode` 寫入 escaped payload。CQP 測例配置 80 bytes、輸出
   113 bytes，canary 被覆寫；沒有 guard 的 QEMU 實驗後續在 free 崩潰。
   這與目前板上第一張 OOM 是不同問題，但會妨礙後續驗證。
2. **不能只關 CABAC 後沿用 vendor skip。** 它的 CAVLC 路徑逐 MB 寫 UE(1)，
   不能表達完整的 all-skip slice；測例 FFmpeg 報 decoding errors，畫面 MD5
   不一致。必須搭配正確的獨立 skip writer。
3. **close 後仍有未釋放配置。** QEMU 的一般 close 與釋放外部 I420 之後，
   殘留 requested bytes 1,570,958。其中 1,536,027 來自 encoder open 的 NAL
   buffer，其餘 34,931 來自 cqm init。重建 UI／重連時可能逐次累積，需要
   依實際 ABI 檢查 ownership 與 aliases 後補 cleanup，並做板上多次循環驗證。
4. **輸出區容量固定 307,200 bytes 且無界限檢查。** 測例首張 CABAC 132,472
   bytes、CAVLC 268,382 bytes；CAVLC 會增加資料量，不能假設任意圖案皆裝得下。
   新前端應檢查總容量，超出時重試較高 QP 或使用有容量管理的輸出流程。
5. **OOM 路徑不安全。** 目前 archive 的 frame allocation 在 NULL 之後仍做
   plane 指標運算；外部 CreateUi 也未檢查 init／encode 失敗值。修正用量時
   必須同時讓失敗路徑停止編碼、清理所有權，避免把錯誤長度累加到輸出指標。

### 建議的實作順序

1. 為這個「一張 IDR＋20 張 skip」用途建立可檢查容量與錯誤的本機 encoder
   front end。固定 QP、關閉 lowres，保留原尺寸，避免一般影片模式的額外配置。
2. 將 UI I420 與 fenc 真正共用：一開始便配置 core 所需的 padded planes，
   RGB2YUV 依 padded stride 直接填入，core 借用且不重複配置／free。
   不能直接把 compact I420 指標塞進去：目前 core 的 MB 載入會使用 fdec stride。
   要同步處理 planes、stride、padding、frame pool 與 cleanup ownership。
   省去 compact I420 的重複儲存後，第一張峰值推估由 23.84 MB 降到約 18.31 MB。
3. 若採 CAVLC，搭配獨立 skip writer；第一張峰值再降到約 16.45 MB。
   此數字為配置推估，尚未實作共用 fenc，不能當成板上通過的結果。
   第一張完成後 close x264，後續只保留參數／counter／輸出，讓 encoder 端不再
   保留 reference pixels。補上前述 cleanup 與輸出容量問題。
4. 驗證每階段 free/largest、OOM 注入、反覆建立／釋放、bitstream 解碼尺寸、
   21 張相同畫面及車機顯示。最終 heap 須留給 USB、Wi-Fi、AirPlay 等並行工作。

硬體 `lib_rtk264` 在本 repo 的介面限制寬度至 1920，且此 CarPlay build 已停用
其 VOE/ISP 相依項，不能直接解決目前 2560 寬度案例。減少 reference 數或開關
更多 threads 也沒有現成收益，因目前已是 1／1。

本次 QEMU 原型、配置 log、測試 bitstream 與 frame MD5 位於
`/tmp/rt8715-x264-review/`（暫存證據，不是韌體原始碼或交付映像）。
重現時需直接連上述 SHA-256 的 ARM archive，使用 `qemu-arm -cpu max`，
對 malloc/free/realloc/calloc/strdup 與 x264_malloc 做記帳；不可把 host 新版 x264
的 RSS 當成本版證據。歷史原始碼參考為
[VideoLAN ABI 76 snapshot](https://download.videolan.org/x264/snapshots/x264-snapshot-20091005-2245.tar.bz2)，
但 vendor 有差異，例如 bitstream 起始配置固定為 1,024,000 bytes，須以反組譯
與原 archive 實驗為準。本次沒有產生新的 flash_is.bin。

### 追加：RGB 原地轉換與共用 allocation

RGB 轉完便 free 的步驟原本已存在。進一步可做 in-place，但不能直接把目前
RGB2YUV 的 src/dst 設成同一指標：U/V 寫入可能蓋掉尚未讀取的 RGB。

已用 host 原型驗證以下演算法，沿用現有 RGB565／RGB888 展開、BT.601 係數
與 top-left chroma sampling：

1. 每次複製兩行 RGB 到小型 scratch，轉成 `[Y行0, Y行1, U行, V行]` stripe，
   寫回原 RGB allocation 的前端。輸出比輸入短，不會碰到下一組未讀的 RGB。
2. 以半行為一個 chunk，透過 cycle permutation 在原 buffer 內整理成完整
   `Y plane → U plane → V plane`。重用同一份 scratch，另用小 bitmap 記錄 cycle。
3. 若要交給 core 共用，再從 V、U、Y 依序由尾端逐行 memmove，展開為該 ABI
   的 padded stride／plane offset。2560×1440 padded YUV 的 6,003,712 bytes
   可容納於原 RGB565 的 7,372,800-byte allocation。

2560×1440 的 scratch 加 bitmap：RGB565 為 **10,780 bytes**，RGB888 為
15,900 bytes。ASan／UBSan 驗證 RGB565 與 RGB888，尺寸涵蓋小圖、2560×1200、
2560×1440；compact I420 與原本 out-of-place 轉換逐 byte 一致，展開後各 padded
plane 的有效像素也一致。原型為 `/tmp/rt8715-x264-review/inplace.c`。
此驗證不含 firmware ownership、heap、core 借用整合與 padding 邊界初始化。

如此 RGB565 轉換階段的大 buffer 峰值可從 12,902,400 bytes
（RGB565＋compact I420）降為約 7,383,580 bytes（同一 allocation＋scratch）。
代價是額外的重排／搬移；M33 上的耗時尚未量測。

整合時有三個必要條件：

- 原 caller 在 RGB2YUV 前就 malloc I420，需一併取消該配置，否則原本的重疊
  峰值已經發生。
- `CSurface_delete` 會 free surface offset 24 的 pixel buffer。須在移交 ownership
  後將該欄位解除，讓最後的 YUV cleanup 只 free 一次。
- 目前實際連入的是 `configSUPPORT_REALLOC=1`：即使縮小也先 malloc 新區、
  memcpy、再 free 舊區。不能用一般 realloc 假裝原地回收尾端；若要退回尾端
  須另外實作並驗證 shrink-only heap API，而不是直接切換未驗證的 mode 2。

第一階段可以保留整塊 7.37 MB、避免修改 allocator，同時讓 core fenc 共用
其中的 padded YUV。以先前 QEMU 數字推估，CQP 的首張配置峰值約 **19.67 MB**；
搭配 CAVLC 與正確獨立 skip writer 約 **17.82 MB**，皆未含其他 RTOS 工作及
UI 輸出區。日後若可安全原地退回尾端約 1.37 MB，才是前節約 18.31／16.45 MB。
若只有原地轉 compact I420，卻仍讓 core 另配 fenc 且不縮小原 RGB allocation，
編碼時反而比目前多占 1.84 MB，因此 in-place 與 core buffer 共用應一起設計。
