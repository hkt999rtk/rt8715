# AirPlay Video RX Handover Buffer 精準 Pointer Swap 計劃

計劃日期：2026-08-08

## 1. 目標

第一階段只消除下列完整 frame copy：

```text
AirPlay video RX decrypted source
  -> malloc(frameLength)
  -> memcpy(handover, source, frameLength)
  -> ScreenThread queue
```

改為：

```text
AirPlay video RX decrypted source
  -> transfer ownership/refcount
  -> ScreenThread queue
```

本階段不修改：

- receiver 的 TCP read/gather；
- ChaCha/AES implementation；
- `ScreenThread` 的 handover-to-wire copy；
- `lwip_write()` 的 wire-to-TCP-pbuf copy；
- TCP ACK/retransmission ownership；
- AirPlay protocol header、timestamp 或 socket write 行為。

目標是先以最小範圍證明 handover pointer swap 的資料正確性、free 時序與長時間
穩定性，再決定是否處理下一層 copy。

## 2. 為什麼這次 copy 可以被消除

此 copy 不做格式轉換、repack、加解密或 checksum。queue element 只有：

```c
struct screen_frame_item {
    void *data;
    int bytes;
};
```

完整 copy 的唯一目的，是讓 receiver callback 返回並釋放 source 後，另一個 thread
仍有一份有效資料。只要把 source ownership 從 receiver 正式轉給 queue/consumer，
payload copy 本身便沒有必要。

不能單獨讓 `memcpy()` no-op；若 queue 仍保存原 destination，consumer 會讀到未初始化
資料。pointer replacement、producer free 與 consumer free 必須是一個完整 transaction。

## 3. 已確認的 binary 邊界

### 3.1 Handover producer

```text
lib_Accessory2.a : AirPlayScreen.o
function         : AirPlayScreen_SendVideo
```

Object-relative relocations：

| Offset | Relocation | 語意 |
|---:|---|---|
| `+0x26` | `malloc` | 配置 `frameLength` bytes handover destination |
| `+0x2c` | local store | 建立 `{destination, frameLength}` |
| `+0x34` | `memcpy` | source → destination 完整 frame copy |
| `+0x3c` | `CVector_push_back` | 將 8-byte item 複製進 screen queue |

### 3.2 Handover consumer

同一個 `AirPlayScreen.o`：

| Function/offset | 語意 |
|---|---|
| `AirPlayScreen_ScreenThread+0xc2` | 正常 send 返回後釋放 dequeued frame |
| `AirPlayScreen_ScreenThread+0x11a` | shutdown drain 時釋放尚未消費的 frame |

### 3.3 Receiver source owner

```text
lib_CarPlay.a : AirPlayReceiverSessionScreen.o
function      : AirPlayReceiverSessionScreen_ProcessFrames
```

| Offset | Relocation | 語意 |
|---:|---|---|
| `+0x16c` | `malloc` | 配置 encrypted body/source buffer |
| `+0x436` | `ScreenStreamProcessData` | 同步 parse/decrypt 後的 callback chain |
| `+0x44a` | `free` | callback 返回後釋放 source |

這些 offset 是 object/function-relative evidence。final ELF address 會隨 link layout 改變，
implementation 不應只依賴某一次 build 的 absolute address。

## 4. 採用的精準攔截方式

### 4.1 重用既有 wrapper 邊界

工程目前已有：

- final link 的全域 `--wrap=memcpy`；
- `--wrap=AirPlayScreen_SendVideo` profiling 入口；
- `--wrap=CVector_push_back` profiling 入口；
- 已辨識的 screen vector 與 `{pointer, length}` layout。

計劃是在這些 wrapper 加入一個獨立、預設關閉的 handover ownership module。不要在
generic memcpy 只用 `length > 40 KiB` 猜測；`AirPlayScreen_SendVideo` wrapper 先建立
task-local active scope，後續 memcpy/push 必須同時符合：

```text
current task == active producer task
src == active source
len == active frame length
dst != src
screen vector/layout 已驗證
沒有另一筆未完成 pending transaction
ownership table 有空位
```

任一條件不符就完整執行 legacy path，包括目前既有的 GDMA memcpy wrapper。

### 4.2 只對兩個 object redirect free

`free` 不採全域 wrapper，避免每次 heap free 都查表。build-time 建立衍生 archive，
只重新命名以下 object 的 undefined relocation：

```text
lib_Accessory2.a:AirPlayScreen.o
  free -> video_handover_consumer_free

lib_CarPlay.a:AirPlayReceiverSessionScreen.o
  free -> video_handover_producer_free
```

兩個 wrapper 內仍以 tracked pointer、task role 與 callsite/context 做第二層確認；其他
free 一律立即呼叫 real free。原始 vendor archives 保持不變，build 使用可重建的
patched copies，並保存輸入 archive/member hash。

若 object relocation rewrite 在某版本不可用，fallback 才是全域 `--wrap=free`；該
fallback 必須只對 ownership table 中的 exact pointer 攔截，並先量測全域 lookup
overhead。

## 5. Pointer swap transaction

### 5.1 Begin

`AirPlayScreen_SendVideo(source, length)` wrapper：

1. 驗證 source、length、task 與 feature state；
2. 建立此 task 的 active producer scope；
3. 呼叫原始 `AirPlayScreen_SendVideo()`；
4. 返回時確認沒有殘留 pending transaction；若有則記錄 fatal anomaly 並清理；
5. 離開 active scope。

### 5.2 Deferred memcpy

當 `__wrap_memcpy(dst, src, len)` 完全匹配 active scope：

1. 保留原函式要求的 return value `dst`；
2. 在固定 pending slot 記錄 `{temp_dst, source, length, task}`；
3. 暫時不搬 payload。

尚未完成 queue replacement 前，`temp_dst` 仍是有效的完整大小 allocation。這是故意
保留的第一階段安全措施：若下一步驗證失敗，wrapper 仍可補做完整 legacy copy，
不會因為只配置小 token 而發生 buffer overflow。

### 5.3 Queue pointer replacement

匹配的 `CVector_push_back(vector, element)` wrapper：

1. 讀取原 item `{temp_dst, length}`；
2. 取得同 task pending record，驗證 destination/length；
3. 在 ownership table 建立 source owner，初始 refs 為 producer + consumer；
4. 在真正 push 前 publish owner，避免高優先級 ScreenThread 搶先消費；
5. 建立 replacement item `{source, length}`；
6. 呼叫 real `CVector_push_back(vector, &replacement_item)`；
7. 確認 vector size 增加；
8. real-free 不再使用的 `temp_dst`；
9. 清除 pending record。

若第 1–4 步任何驗證失敗：

1. 立即執行 `legacy_memcpy(temp_dst, source, length)`；
2. 將原 item 交給 real push；
3. 不建立 ownership record；
4. 原始 producer/consumer free 語意保持不變。

若 push 後 vector size 沒有增加，必須 rollback consumer ref、釋放 temp destination，
並讓 producer 保留 source ownership。因原 library 不檢查 push return value，這個狀況
必須列為嚴重 anomaly。

## 6. Ownership state machine

固定表格中的每筆 owner 至少包含：

```c
struct video_handover_owner {
    void *source;
    uint32_t length;
    TaskHandle_t producer_task;
    uint32_t sequence;
    uint32_t created_us;
    uint8_t producer_ref;
    uint8_t consumer_ref;
    uint8_t queued;
};
```

不要在 hook 中動態配置 owner metadata。table capacity 應大於 screen queue 最大深度，
並在無空位時完整 fallback。

正常狀態：

```text
EMPTY
  -> PENDING_COPY_ELISION
  -> QUEUED (producer_ref=1, consumer_ref=1)
  -> one side releases
  -> remaining ref=1
  -> other side releases
  -> real_free(source)
  -> EMPTY
```

producer release 是 `ProcessFrames` 返回後原本的 source free；consumer release 是
`ScreenThread` send 返回或 shutdown drain 的 frame free。兩個順序都合法：

```text
producer first -> consumer last -> actual free
consumer first -> producer last -> actual free
```

ownership table 更新與 ref decrement 必須在短 critical section/atomic operation 中完成；
actual `free()` 必須在 critical section 外呼叫。

## 7. 必須保留的語意與 error paths

- `bScreenSend == false`、screen vector 尚未建立或 queue 不可用時不做 swap；
- source 在 queue publish 前仍由 producer 擁有；
- replacement item publish 後，consumer 可立刻執行；
- normal send、socket error、video stop 與 shutdown drain 都必須 release consumer ref；
- receiver parse/decrypt/callback error 仍必須 release producer ref；
- tracked source 在最後一個 ref 前不可回到 heap；
- untracked pointer 永遠走 real free；
- duplicate release、unknown tracked state、owner timeout 或 table corruption 必須停用
  新的 swap，保留診斷，後續 frame 回到 legacy copy；
- runtime failure 不得讓 pointer swap 一半成功、一半沿用舊 ownership。

第一版不嘗試消除 `malloc(frameLength)`；它會在 pointer swap 成功後立即釋放。確認
長時間穩定後，第二階段才評估用固定 token 或直接替換 producer function 來消除這次
短生命週期 allocation。

## 8. Feature control 與 binary compatibility

新增獨立 build/runtime flag，例如：

```text
CONFIG_AIRPLAY_VIDEO_HANDOVER_SWAP=0
```

建議模式：

| Mode | 行為 |
|---|---|
| off | 完全 legacy；wrapper 只做既有 profiling |
| shadow | 仍做 copy，只驗證匹配、free 順序與預期可節省 bytes |
| active | 執行 pointer swap；任何不確定條件逐 frame fallback |
| disabled-after-error | 本次 boot 後續全部 legacy，10 秒 report 保留 first error |

build 必須驗證：

- archive/member SHA-256；
- `AirPlayScreen.o` 仍有預期 malloc/memcpy/push/free relocations；
- `AirPlayReceiverSessionScreen.o` 仍有預期 source malloc/process/free sequence；
- queue element size 仍為 8；
- function-relative callsite fingerprint 符合已驗證版本。

驗證失敗時 build 應中止，或 feature 強制為 off，不能靜默套用舊 offset。

## 9. 10 秒低干擾 profile

新增一行彙總：

```text
[VIDEO_HANDOVER_SWAP]
frames eligible/shadow/swapped/fallback
bytes saved/legacy
owners current/max
release producer/consumer/final
release_order producer_first/consumer_first
pending_collision/table_full/push_fail/unknown_free/duplicate_free/timeout
oldest_owner_us
state enabled/shadow/disabled reason
```

禁止逐 frame UART log。只有 first fatal anomaly 可立即列印一次，其餘併入 10 秒摘要。

`bytes saved` 只計成功省略的 source-to-handover payload copy；不能把 GDMA offload、
handover-to-wire 或 TCP pbuf copy 算進來。

## 10. 分階段實施

### Phase A：純 shadow

- 不改 pointer、不跳過 copy、不延遲 free；
- 驗證每次 `SendVideo` 都形成唯一的 memcpy/push pair；
- 記錄 producer/consumer free 的實際順序與最大 owner age；
- 確認 stop/reconnect/error path。

通過條件：長時間 CarPlay video、反覆 connect/disconnect、screen stop/start 都沒有
unpaired transaction、unknown free 或 queue layout mismatch。

### Phase B：pointer swap，保留 temporary allocation

- skip handover payload copy；
- queue pointer 改成 source；
- temporary destination 在 push 後立即釋放；
- source 以雙 ref ownership 管理；
- 任一 frame 驗證失敗時該 frame legacy fallback。

這是本計劃的主要交付階段。

### Phase C：移除 temporary allocation

只有 Phase B 長時間穩定後才進行。因 `malloc(length)` 本身拿不到 source pointer，
不採用讀 caller register/stack 的脆弱方式。可選方案為替換完整 SendVideo producer，
或新增明確 allocation/push hook；未有安全 ABI 前維持 Phase B。

## 11. 測試矩陣

功能測試：

- I/P frame、SPS/PPS/IDR mixed input；
- 30 fps 長時間畫面；
- screen start/stop；
- CarPlay connect/disconnect/reconnect；
- socket block、send error、remote disconnect；
- queue backlog 與 shutdown drain；
- producer-first 與 consumer-first release race。

故障注入：

- ownership table full；
- pending slot collision；
- forced legacy fallback；
- push size 不增加；
- duplicate producer/consumer release；
- feature 在 first fatal anomaly 後自動停用。

資料驗證：

- active 初期可抽樣比較 source 與 legacy shadow destination；
- H.264 frame length/NAL classification 不變；
- crypto authentication failure 不增加；
- output frame drop/corruption 不增加；
- heap free count、owner table 與 queue drain 最終全部歸零。

效能驗證：

- `[VIDEO_HANDOVER_SWAP] bytes saved` 約等於 video payload rate；
- contiguous large-memcpy GDMA calls/bytes 應少一筆每 frame；
- cache maintenance、GDMA completion IRQ 與 synchronous wait 相應下降；
- queue age、frame latency與 CPU profile 不惡化。

## 12. Go/No-Go 條件

可以進入 active：

- shadow 模式所有 frame 都能唯一配對；
- producer/consumer 兩種 release 順序皆驗證；
- stop/error/shutdown owner 全數歸零；
- object fingerprint 與 queue layout 驗證通過；
- 不需要猜測 malloc caller register 或修改 caller stack。

必須維持 legacy：

- 無法辨識 producer source 的確切 free；
- queue insertion success 無法驗證且可能靜默失敗；
- tracked pointer 存在 unknown/duplicate free；
- archive更新後 fingerprint/callsite 不符；
- ownership table可能在正常 queue depth 下耗盡；
- 實機出現 frame corruption、heap fault、use-after-free 或 owner leak。

## 13. 結論

Video RX source-to-handover 是已確認的 ownership-only copy，適合以精準 pointer swap
消除。最小安全方案是：在既有 SendVideo/memcpy/CVector wrapper 內完成 deferred
copy elision 與 queue item replacement，並只對 `AirPlayScreen.o` 和
`AirPlayReceiverSessionScreen.o` 的 free relocation 建立雙 ref ownership。

第一版保留 temporary handover allocation，只消除 50 KiB payload copy；這讓任何
pre-push驗證失敗仍能補做 legacy copy。待 pointer ownership 在所有 race、stop 與
error path 都通過後，再獨立處理 allocation，避免一次改變過多語意。
