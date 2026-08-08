# RTL8195B Crypto Engine 共用與等待機制調查

## 結論

RTL8195B 在本專案中只有一個共用的硬體 Crypto Engine。AES、ChaCha20、Poly1305，以及 ChaCha20-Poly1305 都透過同一個全域 HAL adapter `g_rtl_cryptoEngine_s` 操作相同的硬體資源。

因此：

- AES 與 ChaCha 不能在硬體中真正同時執行。
- 不同 task 可以同時進入上層 API，但硬體 transaction 必須序列化。
- 目前 CarPlay 的 AES 與 ChaCha wrapper 都以 `RT_DEV_LOCK_CRYPTO` 保護完整的「初始化、設定 key/mode、送出 operation、等待完成」區段，所以兩者會排隊，不會互相覆蓋 engine context。
- 正常等待不使用 busy polling；等待 engine 使用 mutex，等待硬體完成使用 IRQ + semaphore。

## 共用硬體的程式證據

AES 與 ChaCha wrapper 都引用：

```c
extern hal_crypto_adapter_t g_rtl_cryptoEngine_s;
```

相關檔案：

- `project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/aes_rtl8195b/AESUtils.c`
- `project/realtek_amebapro_v0_example/GCC-RELEASE/carplay_app/chacha_m33/ChaCha20Poly1305_rtl8195b.c`

底層 `crypto_api.c` 的 AES、ChaCha 和 Poly1305 API 也都只是轉呼叫同一組 `rtl_crypto_*` HAL API，沒有各自獨立的 engine instance：

- `component/common/mbed/targets/hal/rtl8195b/hal/rtl8195bh/crypto_api.c`

## AES/ChaCha 如何序列化

ChaCha transaction 的基本形式是：

```c
device_mutex_lock(RT_DEV_LOCK_CRYPTO);
chacha_rtl_prepare_locked();
rtl_crypto_chacha_poly1305_init(...);
rtl_crypto_chacha_poly1305_encrypt/decrypt(...);
device_mutex_unlock(RT_DEV_LOCK_CRYPTO);
```

AES 的 `_AES_RTL_Begin()` 至 `_AES_RTL_End()` 同樣持有 `RT_DEV_LOCK_CRYPTO`。

這個鎖必須涵蓋 init 與 data operation，不能只保護單一 HAL call。原因是硬體 adapter 內保存目前的演算法、key、mode、IRQ callback 及執行狀態；若 AES init 和 ChaCha operation 被交錯執行，context 會被破壞。

## 等待 Crypto Engine 所有權

`device_mutex_lock(RT_DEV_LOCK_CRYPTO)` 的實作位於：

```text
component/os/os_dep/device_lock.c
```

底層使用 `rtw_mutex_get_timeout()`。Engine 正忙時，等待 task 會阻塞並讓 FreeRTOS scheduler 執行其他 task，不會在迴圈中持續讀取 crypto register。

其 10 秒 timeout 是診斷週期；timeout 時會印出 `device lock timeout: 2`，然後繼續等待。正常 crypto operation 遠短於這個時間。

## 等待硬體 operation 完成

AES 與 ChaCha 都把 HAL completion 改為 IRQ + semaphore：

```text
送出硬體 operation
    -> 呼叫 task 等待 completion semaphore
    -> Crypto Engine IRQ
    -> ISR 更新 HAL 狀態並 give semaphore
    -> 原 task 被喚醒並處理結果
```

因此正常資料路徑中，等待 DMA/Crypto Engine 完成時 CPU 可以執行其他 task。

AES 使用 `gAESRTLCompletionSema`；ChaCha 使用 `g_chacha_completion_sema`。因為 HAL adapter 與 IRQ callback 是全域的，兩個 wrapper 在取得 `RT_DEV_LOCK_CRYPTO` 後，會在每次 transaction 前確認或重新安裝自己的 callback。鎖可確保 callback 不會在 operation 中途被另一種演算法換掉。

## 什麼情況會 polling

正常情況不 polling。只有 IRQ/semaphore 等待超過目前設定的 1000 ms，才會進入例外復原路徑並呼叫 HAL 原始的 `g_crypto_wait_done()` 做 bounded polling，以確認晚到的 DMA completion、完成 cache maintenance，或判定 engine 必須 reset。

這表示若 log 出現以下訊息，才代表走到 polling/異常路徑：

```text
AES HW IRQ TIMEOUT: ...
[CHACHA][HW] IRQ timeout ...
```

正常運作不應出現這些訊息。

## 同時有 AES 與 ChaCha 流量時的影響

正確性方面，CarPlay wrapper 已序列化，沒有 AES/ChaCha engine race。

效能方面，只有一個 engine，所以：

- AES 正在執行時，ChaCha task 會睡在 mutex；反向亦然。
- 總硬體吞吐量是單一 engine 的吞吐量，不能把 AES 與 ChaCha 吞吐量相加。
- 若兩種演算法同時有大量或很長的 operation，可能增加 mutex wait 與 media jitter，但等待本身不消耗 CPU。
- 軟體 fallback 不占用 Crypto Engine；它使用 CPU，因此可與硬體 operation 在 task scheduling 的意義下交錯進行，但本平台仍是單 CPU core。

## 新增硬體 crypto caller 的注意事項

底層 `crypto_api.c`/`rtl_crypto_*` API 本身沒有在每個函式中自動取得 `RT_DEV_LOCK_CRYPTO`。因此不能假設任意直接呼叫 HAL 的新程式碼都會自動安全序列化。

任何新增的硬體 crypto caller 都應遵守：

1. 在 task context 執行，不要在 ISR 中等待 mutex/semaphore。
2. 取得 `RT_DEV_LOCK_CRYPTO`。
3. 在同一個 critical transaction 內完成 engine init、algorithm/key init 與 data operation。
4. 等 operation 同步完成後才釋放 `RT_DEV_LOCK_CRYPTO`。
5. 不要在持鎖期間呼叫可能再進入硬體 crypto 的其他程式，以免 self-deadlock。

目前這份結論確認的是本專案 CarPlay AES 與 ChaCha wrapper。若之後啟用 TLS、其他 mbedTLS hardware acceleration 或直接使用 `rtl_crypto_*` 的模組，仍應逐一確認它們是否也持有同一個 device lock。
