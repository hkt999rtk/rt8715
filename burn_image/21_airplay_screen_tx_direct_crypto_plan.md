# AirPlay Screen TX Direct Crypto 調查與精準置換計劃

## 實作狀態（2026-08-08）

本文件中的精準置換已在 `feat/video-handover-zero-copy` branch 實作，預設由
`SCREEN_TX_DIRECT_CRYPTO=1` 啟用：

- vendor archive 保持不變，只在 derived `lib_Accessory2_handover.a` 內重寫
  `AirPlayScreen.o` 的 `malloc/free/memcpy` relocation。
- 只有通過 allocation、128-byte header、destination、length 與 crypto-call 全部驗證的
  normal-frame payload copy 才會 deferred；其他 `memcpy` 維持 legacy 行為。
- AES 由 `--wrap=AES_CTR_Update` 改成 `source -> wireBuffer + 128`。
- ChaCha Mode 2 跳過 plaintext staging，直接把原 source 交給 hardware backend，輸出到
  `wireBuffer + 128`；hardware precheck 不通過時才 materialize 並走既有 software path。
- runtime hardware failure 不做不安全的 retry，會印出 `[TXCRYPTO_DIRECT][FATAL]` 並停用
  後續新 direct transaction。
- 每 10 秒由 `[TXCRYPTO_DIRECT]` 列出 defer、AES/ChaCha success、fallback、failure、
  stale write、prefix rewrite 與實際 `bytes_saved`。

實作位置：

```text
src/carbox/screen_tx_direct_crypto.c/.h
GCC-RELEASE/carplay_app/patch_video_handover_archive.sh
GCC-RELEASE/carplay_app/chacha_m33/ChaCha20Poly1305.c
```

## 1. 目的與範圍

本文件調查 AirPlay screen video 在 AES-CTR 或 ChaCha20-Poly1305 加密前的
payload copy，並規劃在沒有 vendor source code 的前提下，如何將現行：

```text
plaintext frame
    -> memcpy(wire payload)
    -> encrypt in place(wire payload)
```

改為：

```text
plaintext frame
    -> hardware encrypt directly to wire payload
```

目標是移除 TX normal-frame path 每張 frame 的一次完整 payload copy。下文保留調查、
置換設計與 rollout 判定條件；上方「實作狀態」記錄目前 branch 的落地結果。

本計劃不包含：

- 移除 128-byte AirPlay screen header copy。
- 改變 AirPlay wire format。
- 改變 TCP/lwIP 的 pbuf fragmentation copy。
- 改變 socket backpressure 與 retry 語意。
- 修改 vendor 原始 archive。

## 2. 結論摘要

調查結果如下：

1. RX 沒有「先 copy 完整 encrypted frame，再原地 decrypt」的額外 copy。socket read
   已直接把資料收進 RX frame buffer，之後在該 buffer 原地 decrypt。因此 RX crypto
   path 不需要做本文件所述的置換。
2. TX normal-frame path 確實先配置完整 wire buffer，把整張 plaintext frame copy 到
   `wireBuffer + 128`，再原地 encrypt。這一筆 payload copy 可以移除。
3. AES-CTR public API 與 Realtek hardware API 都原生支援不同的 source/destination，
   AES 可以直接執行 `source -> wireBuffer + 128`。
4. ChaCha 底層 hardware API 也支援不同 source/destination，但目前 Mode 2 public
   wrapper 遇到 `src != dst` 時會先 staging copy，再於 destination 原地執行 hardware
   crypto。若只改呼叫參數，並不能省下 copy；必須增加真正的 direct one-shot path。
5. Closed sender 會在 crypto 前覆寫 payload 的前 4 bytes，因此 direct path 必須特別
   處理這個 length prefix。
6. 雖然沒有 sender source code，仍可在 derived archive 中只把 `AirPlayScreen.o` 的
   allocation/copy 引用導向專用 wrapper，再用 allocation、offset、length、task 與
   crypto call 五項條件精準辨識這一次 payload copy。

## 3. RX 為什麼不需要改

`lib_CarPlay.a:AirPlayReceiverSessionScreen.o` 的
`AirPlayReceiverSessionScreen_ProcessFrames()` 反組譯顯示：

```text
malloc(encrypted frame length)
socket read callback directly fills allocated frame buffer
ChaCha decrypt(frameBuffer, length, frameBuffer)
or AES_CTR_Update(frameBuffer, length, frameBuffer)
ScreenStreamProcessData(frameBuffer, length)
free(frameBuffer)
```

socket/TCP 資料本來是多個 pbuf fragments，因此仍需要 gather 到 application 所需的
連續 frame buffer；目前這部分已可由 linked-list GDMA 搬運。但 crypto 前沒有第二次
完整 frame copy。

若把 RX 改成 out-of-place decrypt，反而要再配置 destination buffer。除非能從 TCP
pbuf scatter list 直接 decrypt 到最終 handover buffer，否則不會減少 copy，還可能增加
allocation 與 memory bandwidth。

因此 RX 保持現行「read/gather 到 final RX buffer，然後 in-place decrypt」即可。

## 4. TX normal-frame 的實際流程

`lib_Accessory2.a:AirPlayScreen.o` 中的
`AirPlayScreen_SendScreenNormalFrame()` 反組譯確認以下順序：

| Object offset | 行為 |
|---|---|
| `+0x28`–`+0x74` | 在 stack 建立 128-byte screen header、timestamp 與 length |
| `+0x7e` | `malloc(wireLength)` |
| `+0x88` | `memcpy(wireBuffer, header, 128)` |
| `+0x96` | `memcpy(wireBuffer + 128, frame, frameLength)` |
| `+0xa0` | `AirPlayScreen_EncryptData(header, wireBuffer + 128, frameLength)` |
| `+0xbc` | `lwip_write(socket, wireBuffer + offset, remaining)` |
| `+0xcc`–`+0xd8` | `EAGAIN` 時印出 `screen block`、delay 並 retry |
| `+0xf6` | send 完成或失敗後 `free(wireBuffer)` |

加密模式下的 wire layout 是：

```text
wireBuffer
+----------------------+--------------------------+------------------+
| 128-byte header      | encrypted video payload  | 16-byte tag      |
+----------------------+--------------------------+------------------+
^                      ^                          ^
wireBuffer             wireBuffer + 128           payload + length
```

AES-CTR 沒有 16-byte authentication tag；ChaCha20-Poly1305 會保留該 tailroom。

`ScreenThread` 在 `AirPlayScreen_SendScreenFrame()` 返回後才 free handover frame。若
`lwip_write()` 因 backpressure retry，sender 仍同時持有 plaintext handover buffer 與
encrypted wire buffer。因此 crypto 執行期間 source lifetime 足夠，direct crypto 不需要
新增 reference counting。

## 5. `AirPlayScreen_EncryptData()` 的重要語意

反組譯顯示，`AirPlayScreen_EncryptData(header, payload, length)` 先執行：

```c
payload[0] = (length - 4) >> 24;
payload[1] = (length - 4) >> 16;
payload[2] = (length - 4) >> 8;
payload[3] = (length - 4);
```

之後才選擇 crypto：

```c
chacha20_poly1305_add_aad(state, header, 128);
chacha20_poly1305_encrypt(state, payload, length, payload);
chacha20_poly1305_final(state, payload + written, payload + length);
```

或：

```c
AES_CTR_Update(context, payload, length, payload);
```

這代表不能只跳過 payload copy，再把原始 handover frame 當作 source。closed code 寫入
的是 destination 的前 4 bytes，而 direct hardware 將讀取 source。

由於 handover frame 已由 `ScreenThread` 獨占，send 返回後立即釋放，建議 direct path
在提交 crypto 前同步這 4 bytes：

```c
memcpy(plaintextSource, wireDestination, 4);
```

然後 hardware 對整個 frame 執行：

```text
plaintextSource[0..length)
    -> encrypt
    -> wireDestination[0..length)
```

實作時應先 profile 原始前四 bytes 與新 prefix 是否相同，但 correctness 不依賴它們
原本相同。

## 6. AES-CTR direct path

本地 replacement source 中的 API 是：

```c
OSStatus AES_CTR_Update(
    AES_CTR_Context *context,
    const void *src,
    size_t len,
    void *dst);
```

它明確規定 source/destination 可以相同；若不同，則不得 overlap。硬體路徑亦直接
呼叫：

```c
crypto_aes_ctr_encrypt(src, len, iv, 16, dst);
```

因此 AES direct path 只需在 `AES_CTR_Update()` 入口偵測 pending TX mapping：

```c
if ((src == dst) && pending_matches(dst, len)) {
    sync_length_prefix(pending.src, dst);
    src = pending.src;
}
```

後面的完整 block、tail-byte、counter update 及 fallback 邏輯都可沿用。AES hardware
目前以最多 15360 bytes 為一個 validated chunk；多個 chunk 仍能由相同的分離 source/
destination 連續處理。

## 7. ChaCha20-Poly1305 direct path

Realtek backend 已有真正的 out-of-place interface：

```c
chacha_rtl8195b_encrypt(
    key, nonce, aad, aadLen,
    plaintext, length,
    ciphertext, tag);
```

raw/chunked helper 也分別接受 input/output。但是目前 firmware 使用：

```text
CARBOX_CHACHA_MODE = 2 (hardware-only performance mode)
CARBOX_CHACHA_HW_MIN_LEN = 4096
```

Mode 2 的 `chacha20_poly1305_encrypt()` 會呼叫 `chacha_deferred_copy()`：

- exact in-place (`src == dst`) 時 copy helper 直接返回，final 階段 hardware 原地加密。
- out-of-place (`src != dst`) 時先將 payload staging/copy 到 destination，final 階段仍用
  destination 作為 hardware input/output。

所以不能只把 closed caller 的 in-place 參數換成分離參數。需要為已確認的 TX mapping
增加 direct one-shot 狀態：

```text
chacha20_poly1305_encrypt()
    record original source, destination and length
    do not execute chacha_deferred_copy()
    preserve the return/final calling contract

chacha20_poly1305_final()
    call chacha_hardware_encrypt_auto(
        originalSource,
        length,
        wireDestination,
        tagDestination)
```

此 direct 狀態建議放在外部固定 context，而不是擴大
`chacha20_poly1305_state`。原因是 state storage 由 closed library 配置，任意擴大 C
structure 可能寫出 vendor BSS allocation。

對小於 4096 bytes或 hardware precheck 不通過的資料，software fallback 也必須直接
執行 `source -> destination`，不可先 materialize 整張 frame，否則會失去優化目的。

## 8. 沒有 vendor source 時的精準置換

### 8.1 Derived archive

vendor `lib_Accessory2.a` 維持 byte-for-byte 不變。build 時複製出 derived archive，
只處理其中的 `AirPlayScreen.o`：

```text
lib_Accessory2.a
    -> extract AirPlayScreen.o
    -> redirect selected undefined symbols to carbox wrappers
    -> rebuild lib_Accessory2_direct_crypto.a
```

關閉 feature flag 時重新連結原始 archive，即可完全恢復 legacy behavior。

### 8.2 Object-local wrapper

ELF symbol redefine 會影響 `AirPlayScreen.o` 內所有 `memcpy` relocation，無法單靠
symbol rename 只選 `+0x96`。因此 wrapper 必須只對已確認的 normal-frame layout 採取
特殊行為，其餘全部轉交 legacy `memcpy`。

建議記錄最近由此 object 建立的 candidate wire allocation，payload copy 必須同時符合：

```text
current task is ScreenThread
destination == trackedWireBuffer + 128
length == candidate frame length
allocation size == 128 + length
                or 128 + length + 16
source and destination do not overlap
no existing pending operation for this task
```

符合時 wrapper 不搬 payload，只建立：

```c
struct tx_direct_crypto_pending {
    TaskHandle_t owner;
    const uint8_t *src;
    uint8_t *dst;
    size_t len;
    void *wire_base;
    size_t wire_size;
    enum state;
};
```

不符合時立即呼叫 legacy `memcpy`。

### 8.3 為何 configure path 不會被誤判

`AirPlayScreen_SendScreenConfigure()` 使用約 748-byte stack workspace，複製 SPS/PPS 並
呼叫 `AirPlayScreen_SendScreenData()`。它的 destination 不是 tracked heap allocation 的
`base + 128`，所以不符合 normal-frame predicate，所有 copy 維持 legacy behavior。

keep-alive、128-byte header copy 及其他小 copy 同樣不符合完整 predicate。

## 9. Pending context 狀態機

建議狀態如下：

```text
EMPTY
  -> wire malloc observed
CANDIDATE
  -> header copy observed
HEADER_READY
  -> exact payload copy observed and deferred
PAYLOAD_DEFERRED
  -> AES update or ChaCha encrypt consumes mapping
CRYPTO_ACTIVE
  -> crypto success
MATERIALIZED
  -> lwip_write/free clears context
EMPTY
```

所有 transition 必須驗證 owner task、pointer、length 及 allocation boundary。使用固定
少量 context slots，不在 wrapper hot path 進行 malloc。

目前 screen video sender 主要由單一 `ScreenThread` 執行，但仍應以 task handle 綁定
context，不使用無 owner 的單一 global pending pointer。

## 10. Fallback 與安全網

### 10.1 在 payload copy wrapper 階段不吻合

立即執行 legacy copy；不建立 pending state。

### 10.2 Crypto 沒有 consume mapping

第一次 `lwip_write()` 前必須檢查：若仍是 `PAYLOAD_DEFERRED`，代表預期 crypto hook
沒有成功接管。此時應：

```text
legacy memcpy(src, dst, len)
execute only if crypto has not modified destination
log/materialize fallback
clear direct state
```

這個 write-boundary safety net 可避免未初始化 wire payload 被送入 TCP。

若 closed code 已開始 crypto 卻未正常完成，不能在 write wrapper 靜默補 copy；應丟棄
record並印出 fatal log。

### 10.3 Hardware pre-submit failure

- AES：沿用既有 software `src -> dst` fallback。
- ChaCha：direct software `src -> dst` 並產生 tag。
- 不需要恢復 legacy in-place layout。

### 10.4 Hardware post-submit failure

source 與 destination 分離後，plaintext source 理論上仍完整，因此比目前 in-place path
更有條件安全 retry。不過第一版應保留既有 runtime policy：

- 不自動重試已提交的 hardware record。
- 丟棄該 record。
- 在 10 秒 profile 印出 fatal/error count、backend、length 與 failure stage。

未經 board validation 前，不應改變 protocol-visible failure behavior。

### 10.5 Teardown

以下事件都必須清除相應 task context：

- `free(wireBuffer)`
- socket error
- screen session stop
- task exit
- length/pointer validation failure
- crypto fatal failure

context 清除不能釋放 plaintext；plaintext 原本的 owner/free sequence仍由 closed
`ScreenThread` 控制。

## 11. Cache、alignment 與 overlap

direct crypto 必須維持：

- source/destination 不 overlap。
- source cache clean before device read。
- destination invalidation policy與 hardware driver一致。
- crypto completion後 CPU 讀取 destination 前完成必要 invalidate。
- header 與 tag 邊界不被 cache-line maintenance破壞。

`wireBuffer + 128` 保留 malloc base 的 cache-line alignment，通常比任意 offset 更有利。
handover source 的 alignment仍需 profile。ChaCha backend 已處理多種 alignment/tail case，
但目前 board validation主要集中在 in-place；direct out-of-place 仍須新增測試。

## 12. Validation 計劃

### 12.1 Crypto self-test

AES 與 ChaCha 都測試：

- 16 bytes、4095、4096、50 KiB、64 KiB、256 KiB。
- source offset：0、1、4、16、31。
- destination offset：0、1、4、16、31。
- source/destination完全分離。
- source 前後 canary。
- destination header/tag 前後 canary。
- direct ciphertext/tag 與 legacy結果逐 byte 比對。

ChaCha 需分別覆蓋 combined、standalone、chunked + software Poly1305 backend。

### 12.2 Runtime shadow verification

bring-up 階段可低頻率抽樣：

1. 保留 plaintext snapshot或使用測試 frame。
2. direct path 產生 ciphertext/tag。
3. legacy/software reference 產生結果。
4. 比對完整 payload與 tag。

不能每張 frame 都做 shadow copy，否則會抵銷優化並加重 UART/CPU 負擔。

### 12.3 10 秒 profile

建議新增：

```text
[TXCRYPTO_DIRECT]
frames attempt/direct/fallback/error
bytes direct/saved/materialized
AES frames/bytes
ChaCha combined/standalone/chunked frames/bytes
prefix same/rewritten/mismatch
fallback predicate/precheck/write-safety
cache_us clean/invalidate total/avg/max
crypto_us total/avg/max
```

只有 error/fatal 立即印；正常統計集中於 10 秒 reporter，避免影響正式 task。

## 13. 預期效益與仍然存在的 copy

每張 normal video frame 可移除：

```text
memcpy(frameLength): handover plaintext -> wire payload
```

memory traffic 由：

```text
copy:   read source + write wire
crypto: read wire   + write wire
```

降低為：

```text
crypto: read source + write wire
```

這可減少約一半的 crypto 前後 payload memory traffic，並降低：

- linked-list GDMA channel占用。
- copy 所需 cache maintenance。
- memory bus contention。
- wire payload 的重複 cache pollution。
- `ScreenThread` 在 crypto 前等待 copy完成的時間。

但以下資料搬運仍保留：

1. 128-byte header 建立/copy。
2. AES/ChaCha hardware 本身讀 source、寫 ciphertext destination。
3. `lwip_write()` 將 contiguous wire record scatter 到 TCP pbufs 的 copy；目前可由
   linked-list GDMA offload，但除非新增 owned TCP write API，不能直接刪除。
4. USB/NCM/WLAN driver 後續必要的 transport copy 或 DMA mapping。

wire buffer allocation也必須保留，因為 ciphertext需要 128-byte headroom、ChaCha tag
tailroom，且 `lwip_write()` backpressure時必須持續保存完整 encrypted record。

## 14. 建議實作階段

### Phase 1：Observe-only

- Object-local wrapper仍執行 legacy copy。
- 僅驗證 allocation/layout/task/crypto sequence。
- 10 秒輸出 predicate match與 bytes candidate。

### Phase 2：AES direct

- 只啟用 AES-CTR `src -> dst`。
- 保留 ChaCha legacy copy。
- 完成 board ciphertext comparison、long-run與 backpressure測試。

### Phase 3：ChaCha direct

- 增加外部 one-shot state。
- 繞過 Mode 2 staging copy。
- 分 backend完成 out-of-place self-test與 runtime verification。

### Phase 4：Default-on 與清理

- 關閉高頻 verification。
- 保留 fatal log與 10 秒摘要。
- feature flag可切回原始 vendor archive與 legacy crypto path。

這個順序先使用 AES 已存在且語意明確的 out-of-place API驗證 closed sender置換機制，
再處理需要修改 Mode 2 state machine 的 ChaCha，可將風險分離。
