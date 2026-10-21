# CarPlay ChaCha20-Poly1305 漸進測試說明

## 1. 目的與注意事項

本版本提供三種 ChaCha20-Poly1305 運行模式，讓硬體加速功能可以分階段
驗證及導入。請務必依照 `Mode 0 -> Mode 1 -> Mode 2` 的順序測試。

硬體路徑使用 RTL8195B Crypto Engine，完成通知採用 IRQ + semaphore，
不再以 busy waiting 長時間占用 CPU。Crypto Engine 由
`RT_DEV_LOCK_CRYPTO` 序列化，避免與 AES 或其他硬體 crypto 操作同時使用。

M33 ASM 優化已移除，不需要、也不再支援
`CARBOX_CHACHA_USE_M33_ASM`。

## 2. 三種模式

| Flag | 模式 | 對外採用的結果 | 用途 |
|---:|---|---|---|
| `CARBOX_CHACHA_MODE=0` | Software only | Software | 基準測試及安全回退 |
| `CARBOX_CHACHA_MODE=1` | Software + hardware verify | Software | 同時計算 hardware，和 software 結果比較 |
| `CARBOX_CHACHA_MODE=2` | Hardware preferred | 符合條件時 Hardware；送出 HW 前不適用才走 Software | 硬體正式導入 |

Mode 2 在 hardware 尚未送出前仍可選擇 software；一旦 DMA/HAL transaction
已送出，失敗只打印並回報，不再 software 重算。

## 3. 編譯方法

請在 `project/realtek_amebapro_v0_example/GCC-RELEASE` 執行：

```bash
# Mode 0：software baseline
make -f application.is.mk all \
  CARBOX_EXPERIMENTAL_SMART_A_LINK=1 \
  CARBOX_CHACHA_M33=1 \
  CARBOX_CHACHA_MODE=0

# Mode 1：software + hardware verify
make -f application.is.mk all \
  CARBOX_EXPERIMENTAL_SMART_A_LINK=1 \
  CARBOX_CHACHA_M33=1 \
  CARBOX_CHACHA_MODE=1

# Mode 2：hardware preferred；HW 已送出後失敗不 retry
make -f application.is.mk all \
  CARBOX_EXPERIMENTAL_SMART_A_LINK=1 \
  CARBOX_CHACHA_M33=1 \
  CARBOX_CHACHA_MODE=2
```

Mode 2 的非 16-byte aligned 封包可另外做 Poly1305 A/B 測試：

```bash
# Baseline：standalone hardware Poly1305，保留完整 Poly input copy
make -f application.is.mk all \
  CARBOX_EXPERIMENTAL_SMART_A_LINK=1 \
  CARBOX_CHACHA_M33=1 \
  CARBOX_CHACHA_MODE=2 \
  CARBOX_CHACHA_NONALIGNED_SW_POLY=0

# Copy-reduction：hardware ChaCha + software Poly1305
make -f application.is.mk all \
  CARBOX_EXPERIMENTAL_SMART_A_LINK=1 \
  CARBOX_CHACHA_M33=1 \
  CARBOX_CHACHA_MODE=2 \
  CARBOX_CHACHA_NONALIGNED_SW_POLY=1
```

此選項預設為 `0`，不會改變原 Mode 2 baseline。設為 `1` 時只改變
4 KiB～64 KiB 且 payload 長度不是 16 bytes 倍數的 backend；aligned
combined hardware 路徑及超過 64 KiB 的 chunked 路徑維持不變。

啟動後應看到對應 policy：

```text
[CHACHA] nonaligned_poly=hardware (baseline)
```

或：

```text
[CHACHA] nonaligned_poly=software (copy-reduction A/B)
```

如工具鏈不在專案預設位置，可另外傳入：

```bash
CROSS_COMPILE=/absolute/path/to/newlib/bin/arm-none-eabi-
```

每次燒錄前請確認使用的是本次 mode 產生的最新 image，並保存完整 build
command、image checksum、測試機板版本及完整 UART log，避免混用不同 mode。

### HAL 能力診斷版本

第一次調查硬體 Poly1305 streaming 與 ChaCha in-place 能力時，請使用 Mode 1
並另外加入：

```bash
make -f application.is.mk all \
  CARBOX_EXPERIMENTAL_SMART_A_LINK=1 \
  CARBOX_CHACHA_M33=1 \
  CARBOX_CHACHA_MODE=1 \
  CARBOX_CHACHA_HW_SELFTEST=1
```

Self-test 會在第一筆真正進入 crypto hardware 的 transaction 前執行一次。
測試期間仍持有共用 crypto mutex，不會和 AES／其他 crypto operation 並行。
它是診斷功能，預設為 `0`；測試結束後正式版本必須恢復為 `0`。

正常會出現：

```text
[CHACHA][SELFTEST] BEGIN diagnostic-only; production routing unchanged
[POLY1305][SELFTEST] oneshot-known PASS
[POLY1305][SELFTEST] init-process-single PASS
[POLY1305][SELFTEST] block-stream PASS
[POLY1305][SELFTEST] aead-segments PASS
[POLY1305][SELFTEST] byte-stream PASS
[POLY1305][SELFTEST] cumulative-128k PASS
[CHACHA][SELFTEST][INPLACE-RAW] PASS cases=20 failures=0
[CHACHA][SELFTEST][INPLACE-CHUNKED] PASS cases=3 failures=0 len=65600
[CHACHA][SELFTEST][INPLACE-COMBINED] PASS cases=9 failures=0 (capability-only)
[CHACHA][SELFTEST] END poly_failures=0 raw_inplace_failures=0 chunked_inplace_failures=0 combined_inplace_failures=0
```

判讀方式：

- `aead-segments PASS`：可以用完整 16-byte block 分段送入硬體 Poly1305，
  有機會移除 payload-sized `poly_input`，只保留 16-byte staging buffer；
- `byte-stream PASS`：HAL 也能跨 process 保存不足 16 bytes 的 partial block；
- `cumulative-128k PASS`：兩次 65536-byte `process()` 可以累積同一個
  Poly1305 狀態，總長度可超過單次 HAL 的 64 KiB 限制；
- `INPLACE-RAW PASS`：standalone ChaCha input/output 相同地址，包含
  `0、1、15、16、31` cache-line offset，結果與 guard bytes 都正確；
- `INPLACE-CHUNKED PASS`：跨越 64 KiB HAL transaction 後，in-place
  input/output、ChaCha counter 銜接和 guard bytes 都正確；
- `INPLACE-COMBINED PASS` 只表示 HAL 能力可行。正式 decrypt 仍須考慮 tag
  失敗時 ciphertext 已被覆蓋的 API 語意，不能只因本測試通過就直接啟用。

如果某個 streaming 或 in-place 測試顯示 `FAIL`，請保留完整 log；正常
CarPlay Mode 1 路由不會因此自動改變。診斷測試會暫時配置約 200 KiB，並讓
第一筆符合條件的硬體 operation 延後到測試矩陣完成。任何 HAL error 或
IRQ timeout 會立即停止該測試類別，不會把每一個 size／offset 都等待一次。

2026-07-31 RTL8195B 實機結果：ChaCha raw／chunked／combined in-place 通過，
但 Poly1305 只有 single process 正確，所有 split process tag 都不一致。因此
本版本將 Mode 1 disposable shadow buffer 與 Mode 2 正式硬體路徑改為
in-place；完全相同的 `src == dst` staging copy 也會跳過。仍不會用
`rtl_crypto_poly1305_process()` 組成 streaming Poly1305。Mode 2 不再配置
payload commit buffer；DMA/HAL 送出後若失敗，input 可能已部分被覆寫，因此
只打印 `[CHACHA][HW][FAIL]`，不得 software fallback。Decrypt 呼叫者在
`out_error != 0` 時不得使用 output。

## 4. 第一階段：Mode 0 software baseline

開機並建立 CarPlay 連線後，應看到一次：

```text
[CHACHA] mode=SOFTWARE_ONLY
```

建議至少完成以下基準測試：

1. 冷開機、CarPlay 首次連線及正常斷線。
2. 連續斷線／重連至少 20 次。
3. 地圖、畫面串流、音樂、通話及控制操作。
4. 持續運行至少 60 分鐘。
5. Wi-Fi、USB、AES 及 CarPlay 同時有流量的壓力測試。
6. 記錄 CPU utilization、重連成功率、畫面／聲音異常及 crash 情況。

Mode 0 通過後，保留該 image 作為後續比較及現場回退版本。

## 5. 第二階段：Mode 1 software + hardware verify

Mode 1 對外仍使用 software 結果；hardware 只做 shadow calculation 及比對。
因此這是第一次在實體機啟用硬體路徑時必須使用的模式。

首次使用 ChaCha 時應看到：

```text
[CHACHA] mode=SOFTWARE_HW_VERIFY (software authoritative, hardware not board-validated)
```

當符合硬體條件的資料第一次進入 hardware backend，應看到：

```text
[CHACHA][HW] RTL8195B IRQ + semaphore backend initialized (not board-validated, timeout=1000 ms)
```

依封包條件，第一次使用各路徑時還會打印一次：

```text
[CHACHA][HW] backend=combined-chacha-poly1305 min_len=4096
[CHACHA][HW] backend=standalone-chacha+hardware-poly1305 min_len=4096
[CHACHA][HW] backend=standalone-chacha+software-poly1305 min_len=4096
[CHACHA][HW] backend=chunked-chacha+software-poly1305 min_len=4096
```

其中兩個 standalone Poly1305 backend 由
`CARBOX_CHACHA_NONALIGNED_SW_POLY` 及實際封包條件決定，不要求每次
測試都必然同時出現。

請重做 Mode 0 的全部測試，並增加：

1. 高流量畫面串流下反覆連線／斷線。
2. 前景／背景切換、休眠／喚醒及長時間運行。
3. 同時使用 AES、Wi-Fi 和 ChaCha，驗證共享 Crypto Engine 的互斥。
4. 測試不同大小及不同切分方式的網路資料。
5. 檢查 cache、DMA、IRQ、semaphore timeout 及資料一致性相關打印。

下列打印表示 software 與 hardware 結果不一致。只要出現一次，就不可進入
Mode 2：

```text
[CHACHA][VERIFY][MISMATCH] ...
```

Decrypt mismatch 中的 `data_diff` 是「第一個不同 byte 的 offset」：
`data_diff=-1` 才表示 software/hardware plaintext 完全相同；
`data_diff=0` 表示從第一個 byte 就不同。

下列打印代表 hardware／IRQ 路徑異常，也不可進入 Mode 2：

```text
[CHACHA][HW] IRQ timeout ...
[CHACHA][HW] crypto_init failed ...
[CHACHA][HW] completion semaphore init failed
[CHACHA][HW] key init failed ...
[CHACHA][HW] operation failed ...
[CHACHA][HW] engine recovered after operation failure
[CHACHA][HW] engine recovery failed ...
```

以下 `VERIFY skipped` 通常不是計算錯誤，表示該次資料不符合目前 hardware
限制，結果仍由 software 正常產生：

```text
[CHACHA][VERIFY] skipped: reason=... len=... aad_len=... count=...
```

目前預設策略：

- `data_len < 4096`：刻意使用 software，不打印 fallback；
- `data_len >= 4096`，長度不超過 64 KiB且為16 bytes整數倍、AAD不超過
  496 bytes：combined hardware ChaCha20-Poly1305；
- 其他不超過64 KiB的資料，如果
  `round16(AAD)+round16(data)+16 <= 65536`：standalone hardware ChaCha
  加 standalone hardware Poly1305；
- 超過 standalone Poly限制：64 KiB chunked hardware ChaCha，加上
  software Poly1305處理完整 ciphertext；
- 所有硬體路徑都要求支援的連續 streaming layout、非 interrupt context
  及 temporary buffer配置成功。

大封包 chunked 路徑的 Mode 1 會驗證 hardware ChaCha輸出；該路徑的
Poly1305本來就是 software，並非獨立的 hardware Poly比對。

常見 skip reason：

| Reason | 說明 |
|---|---|
| `unsupported-length` | raw hardware primitive 的長度或 counter不支援 |
| `unsupported-aad-length` | combined hardware 的 AAD超過496 bytes |
| `poly1305-input-too-large` | standalone Poly1305組合後輸入超過64 KiB |
| `unsupported-buffer-layout` | streaming input/output 不是支援的連續 layout |
| `interrupt-context` | 呼叫發生在 ISR context |
| `scratch-allocation-failed` | temporary buffer 配置失敗 |

Skip／fallback log 為避免洗版，只打印前 8 次及之後 count 為 2 的次方時；
不能只用 UART 行數計算實際次數。

### Mode 1 通過標準

必須同時符合：

- CarPlay 功能及穩定性不低於 Mode 0；
- 確認 hardware backend 曾初始化並實際有合適流量進入；
- `VERIFY MISMATCH` 為 0；
- IRQ timeout、init/key/operation error 及 recovery error 全部為 0；
- 無 cache/DMA 資料破壞、crash、deadlock 或異常重連；
- CPU utilization、latency 和記憶體使用可接受；
- 連續壓力測試及長時間測試通過。

Mode 1 未通過時，請保存完整 UART log、觸發步驟、image checksum、封包長度及
AAD 長度，並先回退 Mode 0。

邊界測試至少要包含：

```text
4095, 4096, 4097
65007, 65008, 65009
65535, 65536, 65537
131071, 131072, 131073 bytes
```

並搭配 AAD `0, 2, 128, 496, 497`。4096 bytes以下不進硬體是預期行為。

## 6. 第三階段：Mode 2 hardware preferred

只有 Mode 1 完整通過後才可測試 Mode 2。開機後應看到：

```text
[CHACHA] mode=HARDWARE_ONLY (in-place hardware; runtime HW failure is reported, not retried)
```

符合條件的 operation 採用 hardware 結果。Hardware 尚未送出前若不符合條件，
會安全使用 software，並可能打印：

```text
[CHACHA][HW] software fallback: reason=... len=... aad_len=... count=...
```

因 `unsupported-length`、`unsupported-aad-length`、
`unsupported-buffer-layout` 或 `interrupt-context` 而 fallback 是設計行為。
若送出後發生 `hardware-init-failed` 或 `hardware-operation-failed`，會打印：

```text
[CHACHA][HW][FAIL] op=decrypt reason=hardware-operation-failed status=-2 backend=... len=... aad_len=... input_may_be_overwritten=1 count=...
```

這是硬體異常，不會 fallback；decrypt 以 `out_error` 回傳負值，encrypt 因
既有 ABI 沒有 error return，會清除 tag 並依靠此 log 診斷。

Mode 2 必須重做 Mode 0、Mode 1 的功能、重連、並行 crypto、壓力及長時間
測試。通過標準為：

- 所有 CarPlay 功能正常，沒有認證、解密或 tag verify 問題；
- 沒有 IRQ timeout、hardware operation error、recovery error；
- 沒有 crash、deadlock、cache/DMA corruption；
- software fallback 只出現在 hardware 尚未送出的預期條件；
- `[CHACHA][HW][FAIL]` 必須為 0；
- CPU utilization／latency 相對 Mode 0 有合理改善，且沒有其他 task starvation。

## 7. 問題處理與回退

發現 mismatch、硬體錯誤、IRQ timeout、穩定性退化或原因不明的連線問題時，
立即改回：

```bash
CARBOX_CHACHA_MODE=0
```

重新 build 並燒錄 Mode 0 image。Mode 0 不會進入 ChaCha hardware backend。

回報問題時請提供：

- Mode 0 與問題 mode 的完整 build command；
- firmware image SHA-256；
- 機板版本、SDK／source 版本及測試手機資訊；
- 從開機開始的完整 UART log；
- 最短重現步驟、發生頻率及壓力測試時間；
- 問題前後的 CPU utilization、stack peak 和 memory 狀態。

請勿提供 key、nonce、plaintext、ciphertext 或其他敏感內容；目前程式打印也不會
輸出這些資料。
