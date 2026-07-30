/*
 * carbox_diag.c — Pro1 诊断追踪框架实现
 *
 * 核心功能：
 *   1. 任务注册表：记录每个任务的名称、句柄、栈大小、优先级、当前栈水位
 *   2. 事件追踪缓冲：环形 buffer 记录关键事件（带时间戳）
 *   3. 栈使用率实时监控
 *   4. 堆空间监控
 */

#include "carbox_diag.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"

/* ---- 内部类型 ---- */

typedef struct {
	const char      *name;          /* 任务名称指针（指向 TCB 中的字符串） */
	void            *handle;        /* TaskHandle_t */
	uint32_t         stack_bytes;   /* 创建时指定的栈大小（字节） */
	uint32_t         prio;          /* 优先级 */
	unsigned int     is_alive : 1;  /* 任务是否存活 */
} carbox_diag_task_record_t;

typedef struct {
	carbox_diag_event_type_t type;
	uint32_t                 tick;   /* xTaskGetTickCount() 快照 */
	const char              *msg;    /* 附加描述 */
} carbox_diag_trace_record_t;

/* ---- 静态存储 ---- */

static carbox_diag_task_record_t task_registry[CARBOX_DIAG_MAX_TASKS];
static unsigned int              task_registry_count;

static carbox_diag_trace_record_t trace_buf[CARBOX_DIAG_TRACE_MAX];
static unsigned int               trace_head;   /* 写入位置 */
static unsigned int               trace_count;  /* 已写入总数 */

static int initialized;

/* ---- 内部辅助 ---- */

static void carbox_diag_trace_record(carbox_diag_event_type_t type, const char *msg)
{
	carbox_diag_trace_record_t *rec;

	if (!initialized) {
		return;
	}

	rec = &trace_buf[trace_head];
	rec->type = type;
	rec->tick = xTaskGetTickCount();
	rec->msg  = msg;

	trace_head = (trace_head + 1) % CARBOX_DIAG_TRACE_MAX;
	trace_count++;
}

/* ---- 初始化 ---- */

void carbox_diag_init(void)
{
	memset(task_registry, 0, sizeof(task_registry));
	task_registry_count = 0;
	memset(trace_buf, 0, sizeof(trace_buf));
	trace_head  = 0;
	trace_count = 0;
	initialized = 1;

	rt_printf("[carbox_diag] init done, max_tasks=%d trace_slots=%d\r\n",
		  CARBOX_DIAG_MAX_TASKS, CARBOX_DIAG_TRACE_MAX);
}

/* ---- 任务注册 ---- */

void carbox_diag_task_register(const char *name, void *handle,
			       uint32_t stack_bytes, uint32_t prio)
{
	carbox_diag_task_record_t *rec;

	if (!initialized || task_registry_count >= CARBOX_DIAG_MAX_TASKS) {
		return;
	}

	/* 去重：同名任务可能被重建 */
	for (unsigned int i = 0; i < task_registry_count; i++) {
		if (task_registry[i].handle == handle) {
			/* 更新已有记录 */
			task_registry[i].name        = name;
			task_registry[i].stack_bytes = stack_bytes;
			task_registry[i].prio        = prio;
			task_registry[i].is_alive    = 1;
			carbox_diag_trace_record(CARBOX_DIAG_EVT_TASK_CREATE, name);
			return;
		}
	}

	rec = &task_registry[task_registry_count];
	rec->name        = name;
	rec->handle      = handle;
	rec->stack_bytes = stack_bytes;
	rec->prio        = prio;
	rec->is_alive    = 1;
	task_registry_count++;

	carbox_diag_trace_record(CARBOX_DIAG_EVT_TASK_CREATE, name);

	rt_printf("[carbox_diag] task #%u: \"%s\" stack=%u prio=%u handle=%p\r\n",
		  task_registry_count, name, stack_bytes, prio, handle);
}

void carbox_diag_task_unregister(void *handle)
{
	if (!initialized) {
		return;
	}

	for (unsigned int i = 0; i < task_registry_count; i++) {
		if (task_registry[i].handle == handle) {
			task_registry[i].is_alive = 0;
			carbox_diag_trace_record(CARBOX_DIAG_EVT_TASK_DELETE,
						 task_registry[i].name);
			return;
		}
	}
}

void carbox_diag_task_entry(const char *name)
{
	(void)name;
	carbox_diag_trace_record(CARBOX_DIAG_EVT_ENTER, name);
}

/* ---- 栈使用率报告 ---- */

void carbox_diag_stack_report(unsigned int warn_threshold_pct)
{
	unsigned int alive = 0;

	if (!initialized) {
		return;
	}

	rt_printf("=== [carbox_diag] Stack Report (tick=%u) ===\r\n",
		  (unsigned int)xTaskGetTickCount());

	for (unsigned int i = 0; i < task_registry_count; i++) {
		carbox_diag_task_record_t *rec = &task_registry[i];
		UBaseType_t high_water;
		uint32_t used, used_pct;

		if (!rec->is_alive || rec->handle == NULL) {
			continue;
		}

		high_water = uxTaskGetStackHighWaterMark((TaskHandle_t)rec->handle);
		used       = rec->stack_bytes - (uint32_t)(high_water * sizeof(StackType_t));
		used_pct   = (used * 100U) / rec->stack_bytes;

		alive++;

		rt_printf("  [%2u] \"%s\" prio=%-2u stack=%5u used=%5u (%3u%%)",
			  i, rec->name, rec->prio, rec->stack_bytes,
			  used, used_pct);

		if (used_pct >= warn_threshold_pct) {
			rt_printf(" *** WARN: stack usage >= %u%% ***",
				  warn_threshold_pct);
		}

		rt_printf("\r\n");
	}

	rt_printf("=== %u alive tasks ===\r\n", alive);
}

/* ---- 全面诊断 dump ---- */

void carbox_diag_dump_all(void)
{
	rt_printf("\r\n");
	rt_printf("========================================\r\n");
	rt_printf("  CARBOX DIAGNOSTIC DUMP\r\n");
	rt_printf("========================================\r\n");

	/* 堆 */
	carbox_diag_heap_report();

	/* 栈 */
	carbox_diag_stack_report(70);

	/* 追踪 */
	carbox_diag_trace_dump();

	rt_printf("========================================\r\n");
}

/* ---- 堆诊断 ---- */

void carbox_diag_heap_report(void)
{
	extern size_t xPortGetFreeHeapSize(void);
	size_t free_heap = xPortGetFreeHeapSize();

	rt_printf("[carbox_diag] heap free: %u bytes\r\n", (unsigned int)free_heap);
}

/* ---- 追踪缓冲区 dump ---- */

void carbox_diag_trace_dump(void)
{
	unsigned int start, n;

	if (!initialized) {
		return;
	}

	if (trace_count == 0) {
		rt_printf("[carbox_diag] trace buffer empty\r\n");
		return;
	}

	n = (trace_count > CARBOX_DIAG_TRACE_MAX) ? CARBOX_DIAG_TRACE_MAX : trace_count;

	if (trace_count <= CARBOX_DIAG_TRACE_MAX) {
		start = 0;
	} else {
		start = trace_head; /* 环形缓冲区最旧的条目 */
	}

	rt_printf("[carbox_diag] trace buffer (%u/%u events):\r\n",
		  n, trace_count);

	for (unsigned int i = 0; i < n; i++) {
		unsigned int idx = (start + i) % CARBOX_DIAG_TRACE_MAX;
		carbox_diag_trace_record_t *rec = &trace_buf[idx];
		const char *type_str;

		switch (rec->type) {
		case CARBOX_DIAG_EVT_TASK_CREATE:    type_str = "TASK+";  break;
		case CARBOX_DIAG_EVT_TASK_DELETE:    type_str = "TASK-";  break;
		case CARBOX_DIAG_EVT_STACK_WARN:     type_str = "STKWARN";break;
		case CARBOX_DIAG_EVT_STACK_OVERFLOW: type_str = "STKOVF"; break;
		case CARBOX_DIAG_EVT_ENTER:          type_str = "ENTER";  break;
		case CARBOX_DIAG_EVT_EXIT:           type_str = "EXIT ";  break;
		case CARBOX_DIAG_EVT_HEAP_LOW:       type_str = "HEAPLO"; break;
		default:                             type_str = "UKNOWN"; break;
		}

		rt_printf("  [%5u] %s %s\r\n",
			  (unsigned int)rec->tick, type_str,
			  rec->msg ? rec->msg : "");
	}
}

/* ---- 函数追踪 ---- */

void carbox_diag_trace_enter(const char *func)
{
	carbox_diag_trace_record(CARBOX_DIAG_EVT_ENTER, func);
}

void carbox_diag_trace_exit(const char *func)
{
	carbox_diag_trace_record(CARBOX_DIAG_EVT_EXIT, func);
}
