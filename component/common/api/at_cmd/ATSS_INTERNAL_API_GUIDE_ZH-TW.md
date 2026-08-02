# ATSS 內部 API 客戶整合說明

## 功能

ATSS 的統計核心與 command-line 打印已分離：

- `atss_stats_start()`：啟動統計，不打印。
- `atss_stats_get()`：一次複製最新的全部 task 統計，不打印。
- `atss_stats_stop()`：停止 internal 使用，不打印。
- `ATSS`／`ATSS=start`：啟用 command-line 週期打印。
- `ATSS=stop`：只關閉 command-line 打印；internal 仍使用時統計會繼續。

Internal 與 command line 共用一個 monitor task，不會重複取樣。

## 固定 buffer 範例

`atss_internal_example.c` 使用 64-entry static buffer：

```c
static atss_task_stat_t customer_atss_stats[64];
```

每個 entry 是 56 bytes，總共占用 3584 bytes BSS，不占呼叫 task 的
stack，也不需要每次動態配置。

呼叫順序：

```c
const atss_task_stat_t *stats;
size_t count;
uint32_t sequence;
int status;

status = customer_atss_start();

/* 第一筆資料約在 ATSS_SAMPLE_PERIOD_MS（目前 2000 ms）後完成。 */
status = customer_atss_get_latest(&stats, &count, &sequence);
if (status == ATSS_OK) {
	size_t i;
	for (i = 0; i < count; i++) {
		/* 使用 stats[i]，此處不需要 printf。 */
	}
}

status = customer_atss_stop();
```

如果第一筆取樣尚未完成，`get` 回傳 `ATSS_NOT_READY`。如果實際 task
數量超過固定 buffer，回傳 `ATSS_BUFFER_TOO_SMALL`，而 `task_count`
會帶回需要的 entry 數量。

## 欄位

```c
typedef struct atss_task_stat {
	char task_name[32];
	uint32_t priority;
	union {
		uint32_t runtime_us;
		uint32_t runtime_ticks; /* Deprecated compatibility alias. */
	};
	uint32_t cpu_utilization_x10;
	uint32_t stack_size_bytes;
	uint32_t stack_used_bytes;
	uint32_t stack_peak_bytes;
} atss_task_stat_t;
```

`runtime_us` 是 task 在最近一個取樣區間內的執行時間，單位為微秒。
舊程式仍可讀取 `runtime_ticks`，但它只是同一欄位的相容別名，數值單位也已是微秒。

`cpu_utilization_x10` 使用 0.1% 為單位，例如 `277` 表示 `27.7%`。

## 注意事項

- API 只能從 task context 呼叫，不可從 ISR 呼叫。
- `get` 回傳的是最近一個完整的 2 秒區間，不是從 start 以來的累積百分比。
- 底層 32-bit 1 us hardware counter 約每 71.6 分鐘回繞一次；FreeRTOS
  runtime accounting 與 ATSS 取樣均使用 unsigned modulo subtraction，跨越
  回繞點仍可正確計算。單次取樣或兩次 runtime accounting 更新的間隔必須小於
  一個完整 counter 週期；正常 task scheduling 與 ATSS 的 2 秒取樣符合此條件。
- `sequence` 每完成一次有效取樣增加 1，可用來判斷資料是否更新。
- 範例回傳的 `stats` 指向範例自己的 static buffer；下一次呼叫
  `customer_atss_get_latest()` 時內容會被覆寫。
- FreeRTOS 必須啟用 `configGENERATE_RUN_TIME_STATS` 和
  `configUSE_TRACE_FACILITY`。
- Stack Size／Used 與長 task name 使用此套件內的 FreeRTOS
  `TaskDebugInfo_t`／`xTaskGetDebugInfo()` 擴充。
