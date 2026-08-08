# Crypto Engine AES／ChaCha Contention 實機量測結果

## 目的

本報告承接 `13_crypto_engine_sharing_and_waiting.md` 的靜態調查，使用實機 profiling 回答以下問題：

1. AES 與 ChaCha 是否因共用單一 Crypto Engine 而形成瓶頸。
2. 兩種演算法各自占用多少 engine 時間。
3. `RT_DEV_LOCK_CRYPTO` 是否出現大量或長時間等待。

認證流程不在本次分析範圍；數據取自 CarPlay media 正常傳輸期間的一個代表性 10 秒窗口。

## Profile 邊界與定義

Profiler 以 linker wrapper 觀察 `device_mutex_lock/unlock(RT_DEV_LOCK_CRYPTO)`，並依 lock caller 分成：

- `AES`：CarPlay `AESUtils` 硬體 AES 路徑。
- `CHACHA`：RTL8195B ChaCha／Poly1305 backend。
- `OTHER`：TLS 或其他未歸入上述兩類的 Crypto Engine caller。

統計欄位：

- `wait_us`：task 呼叫 crypto mutex 到實際取得 mutex 的時間。
- `hold_us`：取得 mutex至釋放 mutex的時間，包括 engine setup、cache/DMA 處理與 IRQ/semaphore completion wait。
- `busy`：該類型在 10 秒窗口內持有共用 Crypto Engine mutex 的比例；這是保守的 serialized-engine occupancy 上限。
- `active`：統計窗口切換時是否仍有一筆 transaction 持鎖。

Mutex wait 會阻塞 task 並讓 FreeRTOS 執行其他 task，因此 `wait_us` 是 latency／contention 指標，不等於同等數量的 CPU busy time。

## 實機原始數據

```text
[CRYPTOPROF][3] window_ms=10000 lock/completed=1129/1129
wait_us total/avg/max=42134/37/9091
ge10/100/1000/10000us=439/31/8/0

[CRYPTOPROF][3] hold_us total/avg/max=792079/701/27687
ge1/5/10/50ms=269/20/7/0 busy=7.9% active=0

[CRYPTOPROF][3][AES] ops=387/387
wait_us total/avg/max=18429/47/9091 ge100/1000=7/3
hold_us total/avg/max=355680/919/27687 ge1/10ms=104/4
busy=3.5% caller=0x70095093

[CRYPTOPROF][3][CHACHA] ops=742/742
wait_us total/avg/max=23705/31/2597 ge100/1000=24/5
hold_us total/avg/max=436399/588/12702 ge1/10ms=165/3
busy=4.3% caller=0x700b9283

[CRYPTOPROF][3][OTHER] ops=0/0 busy=0.0%
```

## Breakdown

| 類型 | 操作數 | Engine busy | 平均 hold | 最大 hold | 平均 wait | 最大 wait | wait ≥ 1 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| AES | 387 | 3.5% | 919 µs | 27.687 ms | 47 µs | 9.091 ms | 3 |
| ChaCha | 742 | 4.3% | 588 µs | 12.702 ms | 31 µs | 2.597 ms | 5 |
| Other | 0 | 0.0% | 0 | 0 | 0 | 0 | 0 |
| Total | 1129 | 7.9% | 701 µs | 27.687 ms | 37 µs | 9.091 ms | 8 |

AES 與 ChaCha 分項 busy 相加顯示 7.8%，而 total 顯示 7.9%，差異來自每一項各自取到小數一位，不是遺漏 transaction。

## Caller 解析

### AES

```text
0x70095093 -> _AES_RTL_Begin
```

最大 AES wait 與 hold 都由 `_AES_RTL_Begin()` 取得 `RT_DEV_LOCK_CRYPTO` 的 transaction 產生。

### ChaCha

```text
0x700b9283 -> chacha_rtl8195b_chacha_xor
```

最大 ChaCha hold 來自 standalone hardware ChaCha XOR 路徑。這與同一窗口中大量 payload bytes 走 hardware ChaCha 的統計一致。

## 判讀

### 1. Crypto Engine 沒有飽和

總 busy 為 7.9%，代表共用 engine 約 92.1% 的窗口時間沒有被 AES/ChaCha transaction 持有。即使把 mutex hold 當成 hardware occupancy 的保守上限，距離飽和仍很遠。

### 2. Lock contention 很低

10 秒內總 wait 為 42.134 ms，相當於單核牆鐘時間的約 0.42%。1129 次操作中：

- 平均 wait 37 µs。
- 只有 31 次超過 100 µs。
- 只有 8 次超過 1 ms。
- 沒有任何 wait 超過 10 ms。

因此沒有持續排隊或 mutex starvation。

### 3. AES 單次較重，ChaCha 總工作量較大

- AES 操作數較少，但平均 hold 為 919 µs，最大 27.687 ms。
- ChaCha 操作數約為 AES 的 1.9 倍，平均 hold 較短，為 588 µs。
- ChaCha 累積占用 436.399 ms，略高於 AES 的 355.680 ms。

AES 是最大的單次 latency spike 來源；ChaCha 則是較高頻率、較均勻的工作負載。

### 4. 無未分類硬體 crypto 流量

`OTHER=0`，表示此量測窗口沒有 TLS 或其他模組取得 `RT_DEV_LOCK_CRYPTO`。觀察到的 engine 時間可完整歸因於 CarPlay AES 與 ChaCha。

### 5. 統計窗口完整

`lock/completed=1129/1129` 且 `active=0`，表示窗口內每次取得鎖的 transaction 都有對應 unlock，切換窗口時也沒有跨界中的 operation。

## 結論

目前單一 Crypto Engine 不是系統主要 bottleneck，也沒有證據顯示 AES/ChaCha lock contention 導致持續性 media backlog：

- Engine utilization 只有 7.9%。
- 平均 lock wait 只有 37 µs。
- 超過 1 ms 的等待只占 0.7%（8/1129）。
- 沒有其他 crypto caller 競爭 engine。

AES 偶爾有 20～28 ms 的長 transaction，可能形成單次 latency spike，但頻率很低。除非後續逐幀時間線能證明這些 AES spike 與可見畫面卡頓同步，否則不建議為此修改 Crypto Engine 排程、拆鎖或改為 software crypto。

若需要提高結論置信度，可在不同 CarPlay 情境（大量畫面更新、音訊、通話、Siri）各收集多個 10 秒窗口，確認 busy、最大 wait 與最大 hold 的上界；現有數據已足以排除「Crypto Engine 長期飽和」作為目前主要瓶頸。
