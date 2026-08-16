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

## Stage 2 攔截實驗與回退

Stage 2 曾以 `--wrap,ncm_wrap_ntb` 攔截客戶組包器。實驗 wrapper 先呼叫真實客戶
builder，再於 stack 建立 28-byte shadow header 比對，設計上不應改寫客戶 output。

硬體結果是不出圖；停用 wrapper 後只做增量 build 的版本仍不出圖。失敗 log 已經走到
connected、AirPlay SetupInit/Info，之後出現：

```text
Modes changed: accessory not connect
Accessory info change
CarApi_SoftReset
```

這比較像 accessory state transition 失敗，而不是已證明的 NCM TX queue stall；但僅憑
現有 log 無法確定是 wrapper 的 timing/ABI 影響，或增量 build 留下混合 object/layout。

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
