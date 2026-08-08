# CarPlay Video Outgoing Timestamp 調查結果

## 調查目的

確認 CarPlay video frame 的 outgoing timestamp 是否有下列問題：

- timestamp 倒退或重複；
- frame 與 timestamp 配對錯誤；
- 本機與 iPhone timeline 的 clock rate 不一致；
- timestamp error 隨時間持續累積；
- 本機 queue/processing jitter 被帶入 outgoing timestamp。

## 量測邊界

Profiler 同時記錄以下時間：

1. iPhone video header 攜帶的 NTP timestamp；
2. frame 抵達本機的 monotonic time；
3. box 呼叫 outgoing timestamp callback 時，以本機 `GetTickCount()` 轉換出的 NTP timestamp；
4. frame queue、prepare 與 socket handoff 時間。

`normalized_error_us` 比較兩條 timeline 從共同 baseline 開始後的 elapsed time：

```text
normalized_error = outgoing elapsed - iPhone elapsed
```

它不比較兩個 NTP clock domain 的絕對 epoch，因此適合判斷相對漂移與短期相位變化。

## 第一個 10 秒視窗

```text
out_stamp_delta_us:
  avg/sigma = 43339/19148 us
  p50/p95/p99/max = 41000/84000/99000/109000 us

stamp_vs_arrival_abs_error_us:
  avg = 6675 us
  p95/max = 18260/35665 us

stamp_after_arrival_us:
  avg/max = 9300/36695 us

iphone_ntp_delta_us:
  p50/p95/p99/max = 33333/66666/66666/100000 us

wire_vs_iphone_delta_abs_error_us:
  avg = 19589 us
  p95/p99/max = 50666/65666/73000 us

normalized_error_us:
  current/min/max = 16333/-28667/111000 us
  window_change = -29333 us
  source_elapsed = 10133333 us
  slope = -2894 ppm
```

配對與有效性：

```text
paired/missing = 233/0
duplicate = 0
regress = 0
baseline_reset/regress = 0/0
```

## 後續 10 秒視窗

```text
arrival_delta_us:
  avg/sigma = 41427/18260 us

out_stamp_delta_us:
  avg/sigma = 41466/17472 us
  p50/p95/p99/max = 39000/71000/94000/136000 us

stamp_vs_arrival_abs_error_us:
  avg = 6033 us
  p95/max = 15375/23689 us

stamp_after_arrival_us:
  avg/max = 8116/30739 us

wire_vs_iphone_delta_abs_error_us:
  avg = 18312 us
  p95/p99/max = 44667/78666/110333 us

normalized_error_us:
  current/min/max = 21666/-81999/61666 us
  window_change = 3332 us
  source_elapsed = 9966667 us
  slope = 334 ppm
```

配對與有效性：

```text
paired/missing = 241/0
regress = 0
baseline_reset/regress = 0/0
```

## 判讀

### iPhone timestamp 正常

iPhone timestamp delta 主要是：

```text
33.333 ms
66.666 ms
100.000 ms
```

這符合 30 fps timebase。CarPlay 畫面沒有更新時可以不送 frame，因此 66.666 ms 與 100 ms 表示略過一個或兩個 frame period，不能直接視為錯誤。

沒有發現 timestamp regression、duplicate、baseline reset 或 frame/timestamp pairing missing。

### Outgoing timestamp 沒有持續累積 drift

第一個視窗的 normalized error current 約為 `+16.3 ms`，後續視窗約為 `+21.7 ms`。兩個相隔較久的視窗之間只變化約 5 ms。

單一視窗的 slope 曾為 `-2894 ppm`，後續則為 `+334 ppm`。方向會反轉，且 error 在正負值間擺動，這比較符合短期 frame arrival/queue jitter，而不是兩個硬體 clock rate 固定不一致。

若存在真正的累積 drift，應看到以下現象：

- normalized error 長期只朝單一方向增加；
- 多個視窗的 slope 保持相同正負方向；
- current error 隨運作時間持續遠離零點。

本次資料沒有呈現上述模式。

### Outgoing timestamp 會帶入短期 queue/arrival jitter

Outgoing timestamp 與本機 frame arrival 的絕對間隔誤差平均約 `6～7 ms`，明顯小於 outgoing 與 iPhone timeline 的單幀間隔誤差 `18～20 ms`。

此外：

```text
stamp_after_arrival avg = 8.1～9.3 ms
queue avg             = 8.0～9.2 ms
```

兩者接近，表示 outgoing timestamp 大致在 frame dequeue 附近以本機時間產生。它會包含 frame arrival 與 queue latency 的短期變化，但沒有把這些誤差持續累加到後續所有 frame。

短期 normalized error 曾到約：

```text
-82 ms ～ +111 ms
```

因此存在 timestamp phase jitter，但它會回復，而非不斷成長。

## 最終結論

目前 outgoing video timestamp 整體沒有重大問題：

- 沒有倒退；
- 沒有重複；
- 沒有 frame 配對遺失；
- 沒有看到長期累積性 clock drift；
- 沒有 error 隨播放時間不斷增加的證據。

目前觀察到的是短期 timestamp jitter。其來源主要是 outgoing timestamp 跟隨本機 frame arrival/dequeue 時間，因此會反映網路 burst 與 queue latency；但 error 能自行回到接近原本範圍。

依目前證據，timestamp 不像是畫面問題的主要根因，暫時不需要修改 timestamp 產生方式。

若日後仍要降低短期 jitter，較安全的研究方向是以 iPhone timestamp 加固定 clock-domain offset 產生 outgoing timestamp。未確認兩邊 NTP epoch 與同步關係前，不應直接原樣轉送 iPhone timestamp。
