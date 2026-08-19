# USB CDC-NCM TX zero-copy 調查紀錄

更新日期：2026-08-16（Asia/Taipei）

## 範圍與目前狀態

本文件記錄客戶新版 `lib_usbsmart.a` 整合後，CDC-NCM TX 是否阻塞 lwIP、
NTB 組包 copy、zero-copy 可行性、Stage 1 profiler 實測，以及 Stage 2 wrapper
失敗後的精確回退結果。

目前可工作的基準是 **Stage 1 observation-only profiler**。它只讀取 pbuf metadata
與量測時間，不改 payload、pbuf 結構、ownership、客戶 library flow 或 USB flow。
Stage 2 的 `ncm_wrap_ntb` 攔截實驗已完整移除，不是目前基準的一部分。

## 已由硬體確認的 working baseline

- Git 基底：`fa93452`（`usb: integrate customer smart library flow`）
- firmware：
  `project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`
- firmware SHA-256：
  `41d92a4456a215399b752aa9276287e7063cc95ada1874e98080ed778caa61ca`
- 客戶 library：
  `project/realtek_amebapro_v0_example/GCC-RELEASE/usb_lib/build/lib_usbsmart.a`
- 客戶 library SHA-256：
  `5c732996b89e6af1a93000ecf970abcce76adfc3a642b1903f8d989fc0d521ac`
- 硬體結果：可連線、可進 CarPlay、可正常出圖。

此映像是完全移除 Stage 2 後，依序執行 clean 與單一 clean build 所產生；不是在
Stage 2 object 上做增量 relink 的結果。

working baseline 目前只有兩個有意義的 source/build 變更：

1. `ethernetif.c` 的 NCM TX eligibility/timing profiler；
2. `application.is.mk` 預設開啟 `NCM_TX_PROFILE`，並用 mode stamp 確保切換此設定時
   `ethernetif.o` 會重新編譯。

這些修改在撰寫本文件時尚未因本次記錄動作而 commit。

## 目前 TX flow

```text
TCP_IP / lwIP
  -> low_level_output_mii()
  -> ncm_tx_async_enqueue()
       pbuf_ref()，queue timeout = 0
  -> 立即回到 TCP_IP

ncm_tx worker
  -> ncm_send_pbuf_sync()
       single pbuf: 直接使用 payload pointer
       chained pbuf: flatten 到 aligned TX_BUFFER
  -> 客戶 usbh_cdc_ncm_send_data()
  -> 客戶 ncm_wrap_ntb()
  -> usbh_cdc_ncm_bulk_send()
  -> 等待 USB completion semaphore
  -> pbuf_free()
```

async queue 深度為 128，worker priority 與 `TCPIP_THREAD_PRIO` 相同，目前一次只處理
一筆 synchronous USB transaction。queue 只保留 pbuf reference，不會在 enqueue
時複製 Ethernet payload。

### 是否會卡住 lwIP

正常 async 路徑下，USB completion wait 發生在 `ncm_tx` worker，不會直接卡住
`TCP_IP`。不過 USB 變慢或不回 completion 時仍會間接造成 backpressure：

- worker 卡在客戶 send/completion wait；
- queue 中的 pbuf reference 持續佔用記憶體；
- queue 填滿後 enqueue 回 `ERR_BUF`；
- 若 async 初始化失敗而走 synchronous fallback，客戶 send 會直接在呼叫者 context
  等待，此時才可能直接卡住 `TCP_IP`。

因此「有 async worker」只能隔離正常延遲，不能消除 USB stall 的資源壓力。

## 客戶 `lib_usbsmart.a` 的靜態調查

以下分成 disassembly 可確認的事實與依現有設定作出的推論。

### 可確認

- `usbh_cdc_ncm_send_data()` 呼叫 `ncm_wrap_ntb()` 建立 NTB，再呼叫 bulk send。
- bulk submit 成功後會用無限 timeout 等待 completion semaphore。
- submit error 有 retry loop 與短 delay；因此 profiler 的 send phase 同時包含組包、
  submit、retry（若有）及 completion wait。
- HCD 接口目前只看到單一 contiguous buffer pointer 加 length，未看到可直接提交
  header/payload scatter-gather list 的接口。
- 客戶 library 的 `memcpy` 會經由全域 `-Wl,-wrap,memcpy` 進入
  `component/soc/realtek/8195b/misc/platform/libc_wrap.c` 的 `__wrap_memcpy()`。

### 由 disassembly 與 negotiated 參數推論

- `ncm_wrap_ntb()` 是一般 NCM 組包器；目前協商為 NTH16/NDP16、CRC off、
  `dwNtbOutMaxSize=16384`、payload alignment 4。
- 目前單一 datagram NTB 的前置 metadata 約為 28 bytes。實作 zero-copy 前仍應以
  客戶實際 layout/測試向量再次驗證，不能只靠這個推論寫入正式 flow。
- 組包器會清理 output 區、複製 payload，再填入 NCM header/table；所以即使上游
  pbuf 是 single/contiguous，現有客戶 flow 仍會做一次 payload copy。

`__wrap_memcpy()` 的現有策略是：一般與不適用 GDMA 的 copy 使用 M33 optimized
版本；只有長度大於 4096 且 alignment/scheduler 等條件成立時才嘗試 GDMA。因此正常
MTU 級 copy 多半是 M33，較大的 chained flatten 或組包才可能受益於 GDMA。

## 2026-08-16 後續精確反組譯結果

本節是在 USB TX lifetime profiler 通過實機測試後，針對客戶 archive 重新做的純靜態
調查。這一階段沒有修改 firmware flow。

### Object 邊界與 wrapper 可行性

客戶 archive 中相關物件分離如下：

```text
usbh_cdc_ncm_hal.o
  U ncm_wrap_ntb
  U usbh_cdc_ncm_bulk_send
  T usbh_cdc_ncm_send_data

ncm.o
  T ncm_wrap_ntb

usbh_cdc_ncm.o
  T usbh_cdc_ncm_bulk_send
```

因此 `usbh_cdc_ncm_hal.o` 對 `ncm_wrap_ntb` 是真正的 undefined relocation，GNU ld
`--wrap=ncm_wrap_ntb` 技術上可以攔截，不存在「定義與呼叫都在同一個 object，導致
`--wrap` 無法介入」的問題。這只證明 link interception 可行，不代表修改 flow 已經安全；
先前 Stage 2 的硬體失敗仍然有效，任何新 wrapper 都必須單獨 clean build 與硬體 gate。

### 客戶 TX buffer allocation 與 ownership

`usbh_cdc_ncm_init_thread` 執行下列等價行為：

```c
host_user.tx_ntb = malloc(16384);
```

反組譯中的 packed offset 是 `host_user + 43`。`usbh_cdc_ncm_do_deinit()` 會讀取同一
欄位、呼叫 `free()`，再將四個 pointer bytes 清零。因此這個 pointer 的 owner 是客戶
library，生命週期涵蓋 NCM init 到 deinit。

不能長期把此欄位改指向上游 pbuf 或我們的靜態 `TX_BUFFER`；否則 deinit 會對錯誤的
pointer 執行 `free()`。若實驗需要暫時替換 pointer，必須證明 disconnect、error、retry、
deinit 等每條路徑都能先還原原 pointer，目前不採用此高風險方案。

### `ncm_wrap_ntb()` 的精確 common-case layout

函式 ABI 已確認為：

```c
int ncm_wrap_ntb(void *host_user, const void *ethernet_packet,
                 uint32_t packet_length);
```

目前 `ndp16_opts` 常數為：

```text
NTH signature       = "NCMH"
NDP signature       = "NCM0"
NTH16 size          = 12 bytes
NDP16 base size     = 8 bytes
datagram entry size = 4 bytes
max datagrams       = 1
CRC                 = off
```

common case 的計算結果是：

```text
payload_offset = align4(12 + 8 + 2 * 4) = 28
ntb_length     = packet_length + 28
```

組包器依序執行：

```c
memset(host_user->tx_ntb, 0, packet_length + 28);
memcpy(host_user->tx_ntb + 28, ethernet_packet, packet_length);
/* fill NTH16 and NDP16 fields in bytes 0..27 */
return packet_length + 28;
```

對應的單 datagram NTB16 layout 為：

```text
offset  size  contents
0       4     NTH16 signature "NCMH"
4       2     wHeaderLength = 12
6       2     wSequence
8       2     wBlockLength = packet_length + 28
10      2     wNdpIndex = 12
12      4     NDP16 signature "NCM0"
16      2     wLength = 16
18      2     wNextNdpIndex = 0
20      2     datagram index = 28
22      2     datagram length = packet_length
24      4     terminating zero datagram entry
28      N     Ethernet packet
```

`usbh_cdc_ncm_send_data()` 不使用 wrapper 回傳的 output pointer；它只使用回傳 length，
然後重新從 `host_user + 43` 載入客戶 TX buffer pointer並呼叫
`usbh_cdc_ncm_bulk_send()`。submit 成功後再無限等待 completion semaphore。

因此「只在上游 pbuf 前 reserve 28 bytes」不會自動省 copy：原函式仍會清除客戶 buffer，
把傳入 pointer 的內容複製到客戶 buffer `+28`，最後提交客戶 buffer。

### Lifetime profiler 的實機交叉驗證

```text
[USBTXLIFE] logical calls/ok/error=6824/6824/0 bytes=9574942
[USBTXLIFE] hcd submit/error=6825/0 bytes=9766014
[USBTXLIFE] source scoped/exact/range/internal=6825/0/0/6825
[USBTXLIFE] return_pending=0
[USBTXLIFE] source_release anomaly/pending=0/0
```

HCD bytes 與 logical bytes 差值為：

```text
9766014 - 9574942 = 191072
191072 / 6824 = 28 bytes per logical packet
```

這與反組譯得到的 NTB16 layout 精確一致。所有 HCD pointer 都被分類為客戶 internal
buffer；沒有任何一次 HCD submit 直接使用來源 packet pointer。submit/terminal 平衡，
logical send return 時沒有 pending transfer，pbuf release 時也沒有 pending/anomaly，證明
目前客戶同步 completion 與上游 pbuf lifetime contract 是安全的。

### USB DMA alignment 的精確結論

DWC HCD source contract 寫明：transfer buffer physical address 若不是 DWORD-aligned，
會改用 `dw_align_buf`。因此 USB controller 的最低起始地址要求是 **4 bytes**。

32 bytes 是 M33 D-cache line 大小與建議 allocation/isolation alignment，不是 DWC USB DMA
硬體的最低要求。TX buffer建議仍以32-byte配置，因為它同時滿足 DWORD alignment並簡化
cache clean；但判定 HCD是否會因 unaligned而使用 bounce buffer時，關鍵是4-byte對齊。

### 最保守的首次 copy-elision 方案

目前不能安全地把客戶 `host_user.tx_ntb` pointer改成上游 packet。最低風險方案是保留客戶
配置、提交、completion與free flow，只讓 chained pbuf直接 flatten 到客戶既有 buffer的
`+28`：

```text
現況（chained pbuf）
  pbuf chain -> our TX_BUFFER -> customer tx_ntb + 28 -> HCD

候選方案
  pbuf chain -----------------> customer tx_ntb + 28 -> HCD
```

這需要一個嚴格的 hybrid gate：

1. observation wrapper先取得並發布客戶 `tx_ntb` pointer與有效 generation；
2. NCM TX只有在 connected、buffer有效、單 worker序列化且長度不超過容量時，才直接
   flatten到 `tx_ntb + 28`；
3. `ncm_wrap_ntb` wrapper只有在 `input == tx_ntb + 28` 時，略過 payload `memcpy`，只清除
   及建立前28 bytes header/table；
4. 所有其他情況呼叫真實客戶 `ncm_wrap_ntb()`；
5. detach/deinit前撤銷發布的 pointer，且 generation不匹配時禁止fast path；
6. 不改客戶 pointer、不改 bulk send、不改 semaphore、不改free contract。

這個方案只消除 chained pbuf 的第二次copy，不是完整 application-to-USB zero-copy。
single pbuf若仍複製到客戶 `tx_ntb + 28`，copy次數與原 flow相同，沒有性能收益；若要讓
single pbuf原地提交，則必須改變客戶 bulk-send pointer或整體 send function，風險高很多，
不列入第一個實驗。

## Stage 1：observation-only profiler

### 量測內容

`[NCMTXPROF]` 每十秒統計：

- calls、errors、bytes；
- single/chained pbuf 與 segment 數；
- total、flatten、send phase 時間；
- size bins 與長延遲 send 次數。

`[NCMZC_PROFILE]` 每十秒額外統計：

- storage 是否與 pbuf struct contiguous；
- custom pbuf；
- 32/64/128-byte headroom；
- payload 4/32-byte alignment；
- pbuf ref count；
- single + contiguous + 32-byte headroom 的理論候選數。

候選判斷只 mirror `pbuf_add_header_impl()` 的 bounds calculation。它不呼叫
`pbuf_header()`，也不修改 `payload`、`len`、`tot_len` 或 reference count。

### 硬體實測原始摘要

```text
[NCMTXPROF] window_us=10003525 calls ok/error=2015/0 bytes=2868184
             pbuf single/chained=1197/818 segments avg/max_x100=140/200
[NCMZC_PROFILE] candidate/head32=2015/1111
                reject chain/storage/head=818/0/86
                storage contiguous/external/custom=2015/0/0
[NCMZC_PROFILE] headroom ge32/ge64/ge128=1111/0/0
                payload_align 4/32=0/0 ref 1/2/>2=247/1768/0
                observation_only=1
[NCMTXPROF] phase_us total/flatten/send/other=478641/8364/469850/426
             avg=237/4/233/0 max total/flatten/send=1514/45/1507
             send_ge_ms 1/5/10/20=2/0/0/0
```

### 結論

- 十秒內 2015 calls、2.868 MB、0 error。
- single pbuf 1197（59.4%），chained pbuf 818（40.6%）。
- 1111 筆（55.1%）同時是 single、contiguous 且有至少 32-byte headroom；這只是
  layout 候選，不代表已可安全交給 USB DMA。
- 86 筆 single/contiguous pbuf 沒有足夠的 32-byte headroom。
- 2015 筆 payload 中，4-byte 與 32-byte aligned 都是 0。這是 direct USB DMA
  zero-copy 的主要阻礙；除非先確認 HCD/USB DMA 接受該 unaligned pointer，或改變
  上游 allocation/layout，否則不能直接採用。
- ref=2 佔多數，符合 async queue 額外持有 reference 的可能情境；ref count 是採樣
  時刻的觀測值，不應單獨當作 ownership proof。
- send 平均 233 us、最大 1.507 ms，只有兩筆超過 1 ms，沒有超過 5 ms。這個窗口
  沒有證據顯示 USB completion wait 是主要卡點。
- chained flatten 平均 4 us、最大 45 us，目前成本很低。只為移除 flatten copy 而
  引入高風險 ownership flow，效益有限。

### 2026-08-16 observation wrapper 硬體 gate

`--wrap=ncm_wrap_ntb` 的純觀察版本已實機測試。它正確轉送三個參數、回傳
real function 的結果，且量到 39/39 個 header 完全符合、output pointer 沒有
變動。第一次回報無法出圖，之後以相同 wrapper 重建並複測，確認可以配對及出圖；
第一次結果屬於測試誤判。

因此 observation wrapper 通過目前的硬體 gate，`NCM_WRAP_PROFILE` 恢復預設 `1`。
它仍會增加巢狀 stack frame，並在每次組包後進入 critical section 更新統計；後續
若加入 copy-elision，必須另外通過一次硬體 gate，不能沿用本次結論。

更完整的反組譯也顯示 `ncm_wrap_ntb()` 是通用 builder；28-byte prefix 是目前
negotiated NCM format/alignment 的 runtime 結果，不能硬編碼成無條件 ABI。

## Stage 2 攔截實驗與回退

### Stage 2A：chained-pbuf scoped copy-elision

第一個實驗只處理原本必須 flatten 的 chained pbuf，single pbuf 完全不變：

1. 第一包仍走客戶原始 flow，wrapper 從實際 NTB header 學習 output pointer、
   generation 與 payload offset。
2. 後續 chained pbuf 取得一次性 token，直接 flatten 到客戶 internal NTB 的
   payload 位置。
3. 客戶 `ncm_wrap_ntb()` 仍完整執行，因此 sequence 與 negotiated header 都由
   客戶程式產生。
4. 只在 token、generation、pointer、length 全部吻合的 builder scope 內，
   `memset` 保留已填入的 payload，且同位址 `memcpy` 成為 no-op。
5. prepare、flatten 或 scope 驗證失敗時，取消 token 或保留原 libc 操作；不把
   此路徑擴大到 single pbuf。

此版本新增 `[NCMELIDE]`，必須以 `prepare == activate == preserve == skip ==
success`、`cancel == fallback == 0` 作為正確性 gate，並同時確認
`[NCMWRAP] header mismatch=0`、USB lifetime anomaly/pending=0 及實際可出圖。

2026-08-16 實機 gate 通過，CarPlay 可正常出圖：

```text
[NCMELIDE][4] prepare/cancel/activate=6363/0/6363
preserve/skip/success/fallback=6363/6363/6363/0
bytes_saved=9410670 payload_offset=28
[NCMWRAP][4] calls/ok/error=6737/6737/0 header match/mismatch=6737/0
[USBTXLIFE][4] source_release calls/anomaly/pending=6737/0/0
```

同一窗口共有 6,619 個 NCM packet，其中 6,259 個是 chained pbuf；copy-elision
覆蓋主要 data path。剩餘 single-pbuf 流量只有約 5.4% packet calls，且多為小型
control packet。此結果證明可以移除 `TX_BUFFER -> customer NTB payload` 的第二次
copy，但 pbuf segments 仍需 flatten 一次，才能形成客戶 USB flow 要求的連續 NTB。

Stage 2A 因此固化為預設路徑。build 會核對通過實機 gate 的 customer
`lib_usbsmart.a` SHA-256；archive 不一致時直接停止 build。這可防止未來客戶改成
multi-datagram NTB 或更換 private layout 後，舊 wrapper 仍被誤用。新 archive 必須先
重新 disassemble、更新 wrapper 並重做配對、出圖、header 與 lifetime gate。

固化版 `flash_is.bin`（SHA-256
`e69b9150d1e2acebc9f41d950b0ec0cdf49ac77b5c197cd8c3da5ffecc49ac77`）再次通過
實機出圖 gate：

```text
[NCMELIDE][8] prepare/cancel/activate=6469/0/6469
preserve/skip/success/fallback=6469/6469/6469/0 bytes_saved=9548714
[NCMWRAP][8] calls/ok/error=6845/6845/0 header match/mismatch=6845/0
output generation/publications/changes=1/1/0
[USBTXLIFE][8] logical calls/ok/error=6788/6788/0 return_pending=0
source_release calls/anomaly/pending=6788/0/0
```

builder 平均 8 us；單次最大 1.023 ms 是該窗口的 outlier，但沒有伴隨 error、
fallback、lifetime anomaly 或 USB pending，不構成功能性失敗證據。

Stage 2 曾以 `--wrap,ncm_wrap_ntb` 攔截客戶組包器。實驗 wrapper 先呼叫真實客戶
builder，再於 stack 建立 28-byte shadow header 比對，設計上不應改寫客戶 output。

早期硬體測試曾回報不出圖，失敗 log 已經走到 connected、AirPlay SetupInit/Info，
之後出現：

```text
Modes changed: accessory not connect
Accessory info change
CarApi_SoftReset
```

後續以相同 observation wrapper 重新 build 並實測已可出圖，因此早期結果不再視為
wrapper failure 的證據。它不能用來推導 timing/ABI 或 NCM TX queue 有問題。

之後執行下列保守回退：

1. 移除 Stage 2 source、define、mode stamp 與 link option；
2. 將殘留的 Stage 2 `.o/.d/.su` 生成物移出 build tree；
3. 依序 full clean，再由精確 Stage 1 source clean build；
4. 確認 ELF/workspace 不再含 Stage 2 wrapper；
5. 產生 SHA-256 為 `41d92a...1ca` 的映像並通過硬體出圖。

所以目前可下的結論只有：

- Stage 2 不可接受，已完整回退；
- 精確 Stage 1 clean build 可工作；
- 尚不能把 Stage 2 失敗唯一歸因於 wrapper 本身；
- 任何 wrapper/link option/flow 變更都必須 full clean build 後再做硬體 gate，不能用
  增量 rollback 的結果判定好壞。

## 真正 zero-copy 的必要條件

目前 HCD 只接受單一 contiguous TX buffer。要真正省掉客戶 NTB payload copy，不能只把
原本 copy 換成另一個 wrapper；buffer 必須從一開始就同時滿足：

1. Ethernet payload 前方有足夠空間容納 NTH/NDP/padding；
2. 整個 NTB 是 HCD 可接受的 contiguous memory；
3. 起始位址、payload offset 與 cache/DMA alignment 符合硬體要求；
4. USB completion 前，原 pbuf/payload 不會被 lwIP 或 application 回收或改寫；
5. completion/error/disconnect/retry 的每一條路徑都只 release 一次；
6. chained、unaligned、headroom 不足及特殊 pbuf 都能可靠回到客戶原始 builder。

因此合理的最終型態會是 hybrid：

```text
eligible single + aligned + enough headroom
  -> in-place NCM header + USB completion ownership（zero-copy）

chained / unaligned / insufficient headroom / ownership uncertain
  -> 客戶 ncm_wrap_ntb() 原始 flow
     -> memcpy 由既有 M33/GDMA policy 選擇
```

以 Stage 1 數據來看，若不先解決 alignment，理論 headroom 候選也不能直接轉成安全的
zero-copy。若 HCD 確認要求 alignment，下一個高價值工作會是找出 payload 為何固定不對齊，
以及能否在上游 allocator/padding 保守修正；這比再次攔截客戶組包器更優先。

## 下一步（保守順序）

1. 凍結 `41d92a...1ca` 為 Stage 1 working reference，不在同一輪混入 flow 修改。
2. 由 HCD source/disassembly 確認 bulk TX buffer 的最低 alignment、cache clean、DMA
   address range 與 completion ownership contract。
3. 記錄實際 payload address modulo（至少 mod 4/mod 32）與 allocation origin，找出
   zero alignment hit 的原因。
4. 優先找不攔截 `ncm_wrap_ntb()` 的觀測點；若必須新增 profiler wrapper，只允許
   已證明不改 ABI、buffer、flow、timing-sensitive state 的純觀測 hook。
5. 先實作嚴格 eligibility gate 與原 flow fallback，再讓極小比例 eligible packet
   試跑；每一階段單獨 clean build、燒錄、配對、出圖與壓力測試。
6. zero-copy correctness 穩定後，才評估多 NTB aggregation、雙 buffer/pipeline 或
   completion 非同步化。客戶目前的同步 completion 與單一 contiguous NTB flow，使這些
   修改的風險高於單純 copy 最佳化。

## 後續硬體 gate

每個階段至少記錄：

- Git commit/dirty diff；
- clean build command；
- `flash_is.bin` SHA-256；
- `lib_usbsmart.a` SHA-256；
- 能否配對、進 CarPlay、出圖、audio、reconnect；
- `[NCMTXPROF]` errors/send latency；
- async queue depth/drop、USB completion timeout；
- zero-copy candidate/hit/fallback/release/live；
- 長時間測試中 live owner 是否單調增加。

出現 accessory state regression、畫面消失、double free、live owner 不下降、queue 持續
增長或 USB completion 不回時，應立即回到最後一個已驗證 hash，而不是在同一 build tree
反覆切 define 做增量比較。

## 2026-08-19 客戶新版 USB/NCM baseline

客戶提供新版 `lib_usbsmart.a` 與可編譯的 NCM source 後，以實機配對及出圖做了逐層 A/B。
最後確認下列組合可正常運作，並固定為正式 build path：

- CDC、HAL 與 `cdc_ncm_ctx` 全部使用新版 archive/source ABI；
- `ncm_tx.c` 保留 `USE_TIMER`，確保結構 ABI 與新版 archive 一致；
- TX 使用 `NCM_TX_COMPAT_SINGLE_DATAGRAM=1`；
- 每個 Ethernet frame 立即形成一個 NCM NTB16：12-byte NTH16、16-byte
  NDP16（含資料 DPE 與 zero terminator）、payload offset 28；
- 不使用尚未完成 ISR-to-task consumer 的 deferred aggregation flush。

不可把舊版 `ncm.o` 或舊 HAL 與新版 context 混用。實測該組合會因私有
`struct cdc_ncm_ctx` layout 不一致而在 `cdc_ncm_fill_tx_frame()` fault；這不是單純的
link symbol 問題。Makefile 因此不再保留舊 archive、跨版本 CDC/HAL 或 timer-flush 的
A/B 選項，避免之後誤建出 ABI 不相容映像。

這個 baseline 的時脈設定為 CPU 300 MHz、LPDDR 240 MHz，LPDDR DQ/WDQ phase 固定 35。
後續若要恢復多 datagram aggregation，應另開實驗分支完成 timer ISR-to-task handoff，
不能直接改動此基準路徑。
