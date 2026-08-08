# RTL8195B WLAN RX Ring Zero-Copy 可行性與 Realtek API 需求

## 目的

本文件整理目前對 RTL8195B Wi-Fi RX 路徑的靜態調查，以及在不破壞
descriptor ownership、cache coherency 與既有 WLAN protocol processing 的前提下，
實作 RX ring-to-lwIP zero-copy 所需的 Realtek driver API。

目前先不實作 zero-copy。下一步會先對 `rtw_recv_tasklet` 做分階段 profiling，
確認 ring-to-skb copy 在實際 CPU 成本中的占比，再決定修改 closed WLAN driver
是否值得。

## 現有 RX 路徑

反組譯 `lib_wlan.a` 後確認主要路徑如下：

```text
Wi-Fi MAC / DMA
    |
    v
RX descriptor ring + rx_ring_pool
    |
    | rtl8195b_recv_tasklet()
    | dev_alloc_skb(frame_len + 14, 0)
    | rtw_memcpy(skb->data, ring_buffer + offset, frame_len)
    v
skb / recv_frame
    |
    | rtw_recv_entry()
    | decrypt / reorder / 802.11-to-Ethernet
    | rtw_recv_indicatepkt()
    v
rltk_netif_rx() -> netif_rx() -> lwIP pbuf
```

目前 skb-to-lwIP 已能利用 skb clone/reference count 保留 payload，主要尚存的
完整 packet copy 是：

```text
RX ring buffer -> skb data buffer
```

## RX Ring 靜態調查結果

### 建立位置

RX ring 由 `lib_wlan.a(rtl8195ba_ops.o)` 中的：

```text
rtl8195b_init_desc_ring()
```

建立。

### Ring layout

反組譯確認：

| 項目 | 數值 |
|---|---:|
| RX descriptor 數量 | 24 |
| 每個 descriptor 大小 | 8 bytes |
| descriptor array 大小 | 192 bytes |
| 每個 RX data buffer 大小 | 2112 bytes |
| RX data pool 大小 | 24 x 2112 = 50,688 bytes |

Descriptor array 由：

```c
rtw_zmalloc(24 * 8);
```

配置，但實際承載 RX packet 的 buffer 不是由 `rtw_zmalloc()` 配置。這些 buffer
來自 `rtl8195ba_ops.o` 內的靜態 BSS：

```text
rx_ring_pool
```

初始化流程等價於：

```c
rx_desc = rtw_zmalloc(24U * 8U);

for (i = 0; i < 24U; ++i) {
    rx_buffer[i] = &rx_ring_pool[i * 2112U];
    rx_desc[i].buffer_addr = rx_buffer[i];
    rx_desc[i].buffer_size = 2112U;
}
```

因此 hook `rtw_zmalloc()` / `rtw_mfree()` 只能看到 descriptor array 或其他 WLAN
dynamic allocations，無法取得或替換靜態 `rx_ring_pool` 的 packet buffer，也無法
單靠 allocator hook 消除 ring-to-skb copy。

## Ring Pending／滿載判斷

`rtl8192ee_check_rxdesc_remain()` 讀取 register `0x3B4`，從上下兩個 11-bit
欄位取得 hardware/software index，並以 24 格 ring 做 modulo difference：

```c
value = rtw_read32(adapter, 0x3B4);

index_a = (value >> 16) & 0x7ffU;
index_b = value & 0x7ffU;
pending = (index_a - index_b + 24U) % 24U;
```

實際 producer/consumer 欄位命名仍應以 Realtek register specification 為準，
但該差值就是 driver 尚未消化的 completed RX descriptors。

一般 ring 會保留一格區分 empty 與 full，因此：

- `pending == 0`：沒有待處理 descriptor。
- `pending` 暫時升高後回到 0：正常 burst。
- `pending` 長時間偏高：receive tasklet 消化速度不足。
- `pending` 接近 23：接近可用容量上限；是否確實以 23 為 full，仍應用實機
  register profile 或 Realtek specification 確認。

## 為什麼 Descriptor Address Rotation 可以 Zero-Copy

Consumer index 可以指出哪個 completed descriptor 已由 hardware 寫完，因此可以
把該 descriptor 的舊 buffer 拆下交給上層，同時補上一塊 replacement buffer：

```c
index = consumer_index;
desc = &rx_desc[index];

received = desc->buffer_addr;
replacement = rx_spare_get();

desc->buffer_addr = replacement;
desc->buffer_size = 2112U;
rx_descriptor_publish(desc);
rx_consumer_advance(index);

rx_deliver_external_buffer(received, packet_offset, packet_length);
```

上層 reference count 歸零後，舊 buffer 回到 spare pool，供其他 descriptor
後續 replacement：

```c
static void rx_buffer_release(void *buffer, void *context)
{
    rx_spare_put(buffer);
}
```

這是可行的 zero-copy 基礎，但不能只寫入一個新 address。還必須同時處理：

1. replacement/spare buffer pool；
2. descriptor publish 與 consumer index 更新順序；
3. DMA/cache coherency 與 memory barrier；
4. skb/external buffer ownership；
5. lwIP 最後一次 reference release；
6. spare pool耗盡時的 fallback 或 drop policy；
7. 原有 decrypt、fragment、reorder 與 802.11-to-Ethernet processing。

## Closed Archive 內的相關 Object Members

### `rtl8195ba_ops.o`

此 member 不只含一個 function，還包含 interrupt、TX/RX ring與HAL operations。
Zero-copy 直接相關的 symbols 為：

```text
rtl8195b_init_desc_ring
rtl8195b_reset_desc_ring
rtl8195b_free_desc_ring
rtl8195b_free_rx_ring        (local symbol)
```

### `rtl8195b_recv.o`

此 member 包含：

```text
CheckRxTgRtl8195b
rtl8192ee_check_rxdesc_remain
rtl8195b_recv_tasklet
rtl8195ba_init_recv_priv
rtl8195ba_free_recv_priv
rtl8195ba_rxhandler
```

Zero-copy 核心主要在：

```text
rtl8192ee_check_rxdesc_remain
rtl8195b_recv_tasklet
```

不能刪除整個 archive member 後只補回單一 function，否則同 member 的其他
symbols 會一起消失。若自行 patch，必須做 symbol-level replacement；但重新實作
`rtl8195b_recv_tasklet()` 會複製大量 private driver logic，維護與正確性風險很高。

## 建議 Realtek 提供的最小 API

### RX buffer描述

```c
struct rtw_rx_buffer {
    void     *token;
    uint8_t  *base;
    uint8_t  *data;
    uint32_t length;
    uint32_t capacity;
    uint32_t descriptor_index;
};
```

`token` 必須能唯一識別 descriptor transaction，避免上層直接依賴 private
descriptor layout。

### 1. Detach completed buffer

```c
int rtw_rx_detach_buffer(void *adapter,
                         struct rtw_rx_buffer *rx);
```

必要語意：

- 依 producer/consumer index取得 completed descriptor。
- 回傳 base、packet data offset、length、capacity與opaque token。
- 完成 hardware-write buffer所需的 cache invalidate。
- 在 rearm 成功前不得把 descriptor歸還 hardware。

### 2. Rearm descriptor with replacement buffer

```c
int rtw_rx_rearm_buffer(void *adapter,
                        void *token,
                        void *replacement,
                        uint32_t capacity);
```

必要語意：

- 驗證 replacement 的容量、DMA可達性及alignment。
- 更新 descriptor address及size。
- 完成 cache clean/invalidate與memory barrier。
- 最後才推進 consumer index並通知 hardware。
- API 成功返回後，舊 buffer ownership才正式轉交上層。

### 3. Process an external RX backing buffer

```c
int rtw_rx_process_external_buffer(
        void *adapter,
        struct rtw_rx_buffer *rx,
        void (*release_cb)(void *buffer, void *context),
        void *context);
```

此 API 應保留現有 driver processing：

- RX descriptor與PHY status parsing；
- security/decryption；
- fragmentation/reordering；
- 802.11-to-Ethernet conversion；
- `rtw_recv_entry()` / `rltk_netif_rx()` indication。

但不得再次複製完整 payload。當 skb/lwIP 最後一個 reference被釋放時，必須呼叫
`release_cb()`，讓 buffer 回到 spare pool。

## Realtek 更適合維護的 Callback 方案

若 Realtek 不希望暴露 descriptor token/index，建議由 driver內部完成 detach/rearm，
只對外提供 buffer ownership callbacks：

```c
struct rtw_rx_buffer_ops {
    void *(*alloc)(uint32_t capacity, uint32_t alignment);
    void  (*indicate)(void *buffer, uint8_t *data,
                      uint32_t length, void *token);
    void  (*release)(void *buffer, void *token);
};

int rtw_register_rx_buffer_ops(
        const struct rtw_rx_buffer_ops *ops);
```

這個方案讓 descriptor format、cache operation、consumer update及錯誤復原繼續由
Realtek driver負責，整體風險低於由 application重寫 tasklet。

## 先做 Profiling 的決策計畫

在要求 Realtek修改 driver前，先對現有 `rtw_recv_tasklet` 做 10 秒窗口統計。
Profiler不逐 packet印 log，且只做量測，不改 RX routing。

### 建議統計

```text
[WLANRXPROF]
tasklet calls / total_us / avg_us / max_us
ring_pending now / avg / max / ge12 / ge18 / ge22
frames_per_tasklet avg / max
payload_copy calls / bytes / avg_len / max_len / total_us / pct_of_tasklet
metadata_copy calls / bytes / total_us
dev_alloc_skb calls / requested_bytes / total_us / max_us / fail
rtw_recv_entry calls / total_us / max_us
netif_rx calls / bytes / total_us / max_us
unaccounted_us
```

`rtw_recv_entry` 與 `netif_rx` 是 inclusive階段，兩者不得直接和其他 nested時間相加。
報告必須區分 direct/non-overlapping與inclusive數據。

### Ring pending 的低干擾取得方式

Tasklet原本就會經由 `rtl8192ee_check_rxdesc_remain()` 呼叫：

```c
rtw_read32(adapter, 0x3B4);
```

Profiler可以只在 tasklet active期間 wrap既有的 `rtw_read32()`，攔截
`reg == 0x3B4` 的返回值並計算 pending，不額外增加一次 MMIO read。

### Zero-copy 值得實作的證據

若實機量測同時呈現以下現象，zero-copy價值較高：

1. payload copy占 `rtw_recv_tasklet` direct CPU time 的顯著比例；
2. 每 10 秒 ring-to-skb copy量達數 MB以上；
3. ring pending max經常接近容量上限；
4. `dev_alloc_skb()` 不是主要成本或主要失敗來源；
5. `rtw_recv_entry()` 的 protocol processing不是壓倒性主成本。

若 payload copy只占 tasklet很小比例，而主要成本落在 decrypt、reorder、PHY parsing、
interrupt/MMIO或上層 protocol processing，冒險修改 closed driver的 zero-copy收益可能
不足。

## 建議提供給 Realtek 的簡短需求

> 請提供 RTL8195B RX DMA buffer detach/rearm ownership API，或 external RX buffer
> callback API。Driver需允許 completed RX buffer交給上層，立即以 replacement
> DMA-safe buffer重新啟用 descriptor，並在上層 reference count歸零時透過 release
> callback回收。現有 RX descriptor parsing、decrypt、fragment/reorder及
> 802.11-to-Ethernet流程必須保留，但不得執行 ring-to-skb payload copy。

## 目前結論

透過 descriptor index與buffer address rotation實作真正 RX zero-copy在架構上可行。
阻礙不是無法辨識 completed descriptor，而是 closed driver沒有公開 buffer ownership
transfer、replacement rearm及上層 release callback。

在取得 profiler結果前，不建議直接替換 `rtl8195b_recv_tasklet()`。應先確認完整 payload
copy確實是 `rtw_recv_tasklet` 的主要 CPU成本，再決定要求 Realtek增加正式 API，或承擔
symbol-level driver patch的維護風險。
