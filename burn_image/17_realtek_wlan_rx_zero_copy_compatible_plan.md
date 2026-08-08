# RTL8195B WLAN RX Buffer-Swap Zero-Copy 向下相容修改計畫

## 1. 文件目的

本文件提供給 Realtek WLAN RD team，目標是在不改變既有客戶 API、ABI 與
預設行為的前提下，消除 RTL8195B WLAN RX hot path 中的這次完整 payload
copy：

```text
RX descriptor buffer --rtw_memcpy()--> newly allocated skb buffer
```

建議不另外建立複雜的 per-packet backing object，也不在 hot path 做
DMA buffer malloc/free。Driver 使用一組固定、DMA-safe、同格式的 RX/SDK buffer
pool，把 completed descriptor 的 buffer 與剛分配給 skb 的 buffer 互換：

```text
before:
    descriptor -> buffer A (contains received frame)
    skb        -> buffer B (empty replacement)

after swap:
    descriptor -> buffer B (rearm immediately)
    skb        -> buffer A (deliver upward without payload copy)
```

所有 buffer 在 driver lifetime 內都不釋放回 heap，只在 `RING`、`STACK`、`FREE`
狀態之間轉移 ownership。

## 2. 為什麼需要改

### 2.1 實機 profile 顯示 WLAN RX 是主要 CPU 負載

CarPlay video streaming 期間，`rtw_recv_tasklet` 長期佔約 13% 至 23% CPU samples，
通常與 `TCP_IP` 相近，部分視窗為系統最高負載 task。

每秒數千個約 1332、1440 或 1460 bytes 的 packet 都經過同一次
ring-to-skb copy。單次 copy 不大，但累積後同時增加：

- M33 cycles；
- DRAM read/write traffic；
- cache pollution；
- RX tasklet 佔用時間；
- TCP/IP、USB 與 screen task 的 scheduler 壓力。

### 2.2 上層 skb-to-lwIP copy 已經消除

本專案已經使用 `skb_clone()` 與 lwIP custom pbuf 直接 reference skb payload：

```text
driver skb -> skb_clone -> custom pbuf -> TCP/IP
```

因此剩下的主要 payload copy 是 closed WLAN driver 內部：

```text
RX ring --full copy--> skb --reference--> lwIP
```

把這一次 `rtw_memcpy()` 改成 ownership swap，才能得到 end-to-end RX zero-copy。

### 2.3 為什麼不用 GDMA 取代 memcpy

GDMA 只是換一個 engine 執行同一次 payload copy；資料仍會被讀寫一次，還有
descriptor setup、cache maintenance、channel contention 與 completion 成本。Buffer swap
完全不搬 payload，才是最直接的改法。

## 3. 最小改動設計

### 3.1 Private function pointer 就是 runtime flag

在 WLAN driver private adapter/context 新增 callback：

```c
typedef int (*rtw_rx_buffer_swap_fn)(void *adapter,
                                     struct rtw_rx_desc *desc,
                                     struct sk_buff *skb,
                                     uint32_t frame_offset,
                                     uint32_t frame_length);

struct rtw_wlan_private {
    /* existing private fields remain unchanged */
    rtw_rx_buffer_swap_fn rx_buffer_swap;
};
```

Function pointer 本身就是 runtime flag：

```c
#if CONFIG_RTW_RX_BUFFER_SWAP
    priv->rx_buffer_swap = rtw_rx_fixed_pool_swap;
#else
    priv->rx_buffer_swap = NULL;
#endif
```

不需要讓 application 看到 private descriptor，也不需要修改任何既有 exported
API。舊 application 連結新 driver archive 時，callback 預設為 `NULL`，行為與舊版
完全相同。

### 3.2 RX hot path 只增加一個分支

原本程式：

```c
skb = dev_alloc_skb(RX_BUFFER_CAPACITY, RX_HEADROOM);
if (skb == NULL)
    return RX_DROP;

rtw_memcpy(skb->data, desc->buffer + frame_offset, frame_length);
skb_put(skb, frame_length);
rx_rearm_same_buffer(desc);
return existing_rx_processing(skb);
```

修改後：

```c
skb = dev_alloc_skb(RX_BUFFER_CAPACITY, RX_HEADROOM);
if (skb == NULL)
    return RX_DROP;

if (priv->rx_buffer_swap != NULL &&
    priv->rx_buffer_swap(adapter, desc, skb,
                         frame_offset, frame_length) == 0) {
    stats->swap_packets++;
    stats->swap_bytes += frame_length;
} else {
    /* Exact legacy behavior. The callback must leave desc/skb unchanged
     * whenever it returns failure. */
    rtw_memcpy(skb->data, desc->buffer + frame_offset, frame_length);
    skb_put(skb, frame_length);
    rx_rearm_same_buffer(desc);
    stats->legacy_copy_packets++;
    stats->legacy_copy_bytes += frame_length;
}

return existing_rx_processing(skb);
```

這是 Realtek RD 需要修改的主要 call site。

### 3.3 Callback contract

`rx_buffer_swap()` 必須遵守下列 contract：

- 回傳 `0`：完整 swap 成功，descriptor 已綁定 replacement buffer 並 rearm，
  skb 已綁定 received buffer；
- 回傳非 `0`：descriptor、skb、buffer ownership 必須與進入 callback 前完全
  一樣，caller 可安全執行 legacy `rtw_memcpy()`；
- callback 不得等待 buffer，不得 busy poll；
- callback 不得在成功 publish descriptor 後再回傳失敗；
- 所有 validation 必須在更改 descriptor/skb pointer 前完成。

## 4. Fixed buffer manager

### 4.1 Buffer 不釋放，只轉移 ownership

所有 backing buffer 由 driver 在初始化時一次建立，正常 RX 期間不呼叫 heap
free。每個 buffer 只需要最小 private 狀態：

```c
enum rtw_rx_buffer_owner {
    RTW_RX_BUFFER_FREE = 0,
    RTW_RX_BUFFER_RING,
    RTW_RX_BUFFER_STACK
};

struct rtw_rx_buffer {
    uint8_t *base;
    uint32_t capacity;
    enum rtw_rx_buffer_owner owner;
};
```

若現有 skb data pool 已經是固定 pool，可直接重用現有 allocator 與 reference
count，不要另建第二套 binding table 或 reference count。

### 4.2 互換的是 backing buffer，不是只換 `data` pointer

Swap 時必須一起轉移：

- buffer base/head；
- capacity/end；
- data offset；
- allocator/pool identity；
- DMA/cache ownership；
- skb backing reference。

不得只交換 `desc->buffer` 與 `skb->data`，否則 `skb_put()`、headroom/tailroom
或最後的 pool recycle 會使用錯誤的 base/capacity。

### 4.3 Buffer 格式要求

Descriptor buffer 與 skb backing buffer 必須可互換：

- capacity 均不小於目前 RX buffer，已知實機值約為 2112 bytes；
- 符合 WLAN RX DMA alignment 與 address-range 要求；
- 有足夠 headroom/tailroom 給 decrypt、header conversion 與 skb operations；
- 兩者的 backing memory 都必須由同一 fixed manager 回收，或 swap 時一起
  交換 pool identity；
- 不得把 static ring memory 交給會執行 heap free 的舊路徑。

## 5. Swap 流程

```c
static int
rtw_rx_fixed_pool_swap(void *adapter,
                       struct rtw_rx_desc *desc,
                       struct sk_buff *skb,
                       uint32_t frame_offset,
                       uint32_t frame_length)
{
    struct rtw_rx_buffer *received;
    struct rtw_rx_buffer *replacement;

    received = rtw_desc_backing(desc);
    replacement = rtw_skb_backing(skb);

    /* All rejection checks occur before any pointer/state change. */
    if (!rtw_rx_swap_eligible(desc, received, replacement,
                              frame_offset, frame_length))
        return -1;

    rtw_rx_complete_for_cpu(received->base, received->capacity);
    rtw_rx_prepare_for_device(replacement->base, replacement->capacity);

    rtw_rx_lock(adapter);

    if (received->owner != RTW_RX_BUFFER_RING ||
        replacement->owner != RTW_RX_BUFFER_STACK) {
        rtw_rx_unlock(adapter);
        return -1;
    }

    /* One atomic ownership transaction from the driver's point of view. */
    rtw_desc_set_backing(desc, replacement);
    rtw_skb_set_backing(skb, received,
                        frame_offset, frame_length);
    replacement->owner = RTW_RX_BUFFER_RING;
    received->owner = RTW_RX_BUFFER_STACK;

    rtw_rx_publish_barrier();
    rtw_rx_rearm_descriptor(desc);
    rtw_rx_unlock(adapter);
    return 0;
}
```

Realtek 應以其 private descriptor/skb 欄位實作 `rtw_desc_set_backing()` 與
`rtw_skb_set_backing()`。參考程式不應被直接當成實際 descriptor layout。

## 6. skb clone/free 與 recycle

此方案不需要釋放 DMA memory，但仍需要知道何時可重用。應優先重用現有
skb backing reference count：

- `skb_clone()` 沿用現有 backing reference increment；
- original skb 先釋放時，clone 仍可讀取 payload；
- 最後一個 `kfree_skb()` 不執行 heap free，只將 backing buffer 放回 fixed
  skb/free pool；
- 後續 `dev_alloc_skb()` 再取得該 buffer，它就可以在下一次 swap 成為
  descriptor replacement。

若現有 `skb_clone()`/`kfree_skb()` 已經對 skb data pool 實作正確的共用
reference，這部分不需要新設計。Realtek 只需確認 ring 的 buffer 交換後也納入
同一套 manager。

## 7. Cache 與 descriptor publish ordering

Swap 不 copy payload，但 cache ownership 仍必須正確：

### Hardware -> CPU

1. 確認 completed descriptor ownership 已回 CPU；
2. invalidate received payload range；
3. `DMB/DSB`；
4. 再讓 CPU/stack 讀取 buffer。

### CPU -> Hardware

1. replacement buffer 不得存在會覆蓋新 RX data 的 dirty cache line；
2. clean/invalidate replacement DMA range；
3. 寫入 descriptor address/capacity/control；
4. `DMB/DSB`；
5. 最後才把 descriptor ownership 交給 hardware。

## 8. Legacy fallback 與相容性

下列情況 callback 回傳失敗，caller 直接執行原本 `rtw_memcpy()`：

- callback 未安裝；
- feature flag 關閉；
- skb/ring buffer capacity、alignment 或 address range 不相容；
- malformed/error descriptor；
- 特殊 monitor/MP/CSI frame；
- packet mutation 需要額外 headroom/tailroom；
- driver reset/quiesce 中；
- ownership state 不是預期的 `RING/STACK`。

重要要求：callback 失敗必須是 transaction-abort，不能留下半完成 swap。

既有客戶相容性：

- `netif_rx()`、`rltk_wlan_recv()`、`dev_alloc_skb()`、`skb_clone()`、
  `kfree_skb()` prototype 不變；
- public `struct sk_buff` layout 不變；
- private callback 預設 `NULL`；
- feature off 時不走 swap，完整保留舊 copy path；
- 不需要 application 修改或重新編譯。

## 9. 統計與診斷

建議保留 counters，但量產預設不週期印 UART：

```c
struct rtw_rx_swap_stats {
    uint64_t swap_packets;
    uint64_t swap_bytes;
    uint64_t legacy_copy_packets;
    uint64_t legacy_copy_bytes;
    uint32_t callback_null;
    uint32_t callback_reject;
    uint32_t ownership_error;
    uint32_t recycle_error;
};
```

開發版可每 10 秒印一次；量產版改由 debug command 主動讀取。

## 10. Realtek RD 建議交付步驟

### Phase 1：只加 callback 與 legacy fallback

- private `rx_buffer_swap` function pointer；
- callback 預設 `NULL`；
- hot path 增加 `callback != NULL` 分支；
- callback 未安裝時與現行 driver binary behavior 相同。

### Phase 2：同格式 fixed pool

- RX descriptor 與 skb 使用可互換的 DMA-safe backing buffers；
- 正常運作期間不 heap free backing memory；
- 沿用現有 skb reference/pool recycle；
- 完成 `rtw_rx_fixed_pool_swap()`。

### Phase 3：先啟用普通 data frame

- 一般 STA/AP unicast data 先開 swap；
- decrypt/reorder/A-MSDU/fragment 未驗證前回到 copy；
- 用 CarPlay 與 iperf 壓力測試擴大 coverage。

## 11. Acceptance criteria

- feature off 與舊 driver 無 regression；
- callback `NULL` 時 100% 走 `rtw_memcpy()`；
- callback 失敗後 descriptor/skb 狀態未改變；
- swap 成功後 packet content mismatch 為 0；
- double ownership、double recycle、heap free of pool buffer 為 0；
- `skb_clone()` original-first-free 測試通過；
- Wi-Fi reconnect/reset 無 stale descriptor 與 use-after-recycle；
- ring-to-skb payload copy bytes 明顯下降；
- `rtw_recv_tasklet` CPU 比例以實機 A/B profile 確認改善。

## 12. 給 Realtek RD 的摘要需求

> 請在 WLAN driver private RX context 新增一個預設為 `NULL` 的
> `rx_buffer_swap` function pointer。`rtl8195b_recv_tasklet()` 在原本
> `rtw_memcpy(skb->data, ring_data, frame_len)` 位置，若 callback 存在且回傳
> 成功，則以 fixed DMA buffer ownership swap 取代 payload copy；callback 不存在
> 或回傳失敗時，必須完整執行原本 `rtw_memcpy()` path。Swap 將
> completed descriptor buffer 交給 skb，並將 skb 的空 backing buffer 交給 descriptor
> 立即 rearm。所有 backing buffers 由 fixed pool 終身管理，不做 per-packet
> heap malloc/free；最後一個 skb reference 離開時只 recycle buffer 回 pool。既有
> public API、ABI 與 `struct sk_buff` layout 必須保持不變。

## 附錄：最小參考代碼

```c
typedef int (*rtw_rx_buffer_swap_fn)(void *adapter,
                                     struct rtw_rx_desc *desc,
                                     struct sk_buff *skb,
                                     uint32_t frame_offset,
                                     uint32_t frame_length);

static int
rtw_rx_receive_one(struct rtw_wlan_private *priv,
                   struct rtw_rx_desc *desc,
                   uint32_t offset,
                   uint32_t length)
{
    struct sk_buff *skb;

    skb = dev_alloc_skb(RX_BUFFER_CAPACITY, RX_HEADROOM);
    if (skb == NULL)
        return -1;

    if (priv->rx_buffer_swap != NULL &&
        priv->rx_buffer_swap(priv->adapter, desc, skb,
                             offset, length) == 0) {
        priv->stats.swap_packets++;
        priv->stats.swap_bytes += length;
    } else {
        rtw_memcpy(skb->data, desc->buffer + offset, length);
        skb_put(skb, length);
        rtw_rx_rearm_same_buffer(desc);
        priv->stats.legacy_copy_packets++;
        priv->stats.legacy_copy_bytes += length;
    }

    return existing_rtw_recv_entry(priv->adapter, skb);
}

static int
rtw_rx_fixed_pool_swap(void *adapter,
                       struct rtw_rx_desc *desc,
                       struct sk_buff *skb,
                       uint32_t offset,
                       uint32_t length)
{
    struct rtw_rx_buffer *rx = rtw_desc_backing(desc);
    struct rtw_rx_buffer *empty = rtw_skb_backing(skb);

    if (rx == NULL || empty == NULL ||
        rx->owner != RTW_RX_BUFFER_RING ||
        empty->owner != RTW_RX_BUFFER_STACK ||
        offset > rx->capacity || length > rx->capacity - offset ||
        empty->capacity < rx->capacity ||
        !rtw_dma_buffer_compatible(empty)) {
        return -1; /* Nothing has changed: caller may safely memcpy. */
    }

    rtw_rx_complete_for_cpu(rx->base, rx->capacity);
    rtw_rx_prepare_for_device(empty->base, empty->capacity);

    rtw_rx_lock(adapter);
    rtw_desc_set_backing(desc, empty);
    rtw_skb_set_backing(skb, rx, offset, length);
    empty->owner = RTW_RX_BUFFER_RING;
    rx->owner = RTW_RX_BUFFER_STACK;
    rtw_rx_publish_barrier();
    rtw_rx_rearm_descriptor(desc);
    rtw_rx_unlock(adapter);
    return 0;
}

static void rtw_rx_swap_init(struct rtw_wlan_private *priv)
{
#if CONFIG_RTW_RX_BUFFER_SWAP
    if (rtw_fixed_pool_is_swap_compatible(priv))
        priv->rx_buffer_swap = rtw_rx_fixed_pool_swap;
    else
        priv->rx_buffer_swap = NULL;
#else
    priv->rx_buffer_swap = NULL;
#endif
}
```

可供單獨 review 的參考檔案位於：

```text
burn_image/realtek_wlan_rx_zc_reference/
    rtw_rx_zc_reference.h
    rtw_rx_zc_reference.c
    README.md
```
