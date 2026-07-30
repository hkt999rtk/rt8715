/*
 * carbox_diag.h — Pro1 诊断追踪框架
 *
 * 用途：零侵入式任务栈监控和故障追踪。
 * 用法：
 *   1. 在 main.c 中调用 carbox_diag_init() 初始化
 *   2. rtos_task_create 会自动注册每个任务
 *   3. 栈溢出 hook 自动 dump 完整诊断信息
 *   4. 通过 carbox_diag_stack_report() 可主动查询
 */

#ifndef CARBOX_DIAG_H
#define CARBOX_DIAG_H

#include <stdint.h>

/* 任务注册表最大容量 */
#define CARBOX_DIAG_MAX_TASKS 32

/* 追踪缓冲区最大条目数（环形） */
#define CARBOX_DIAG_TRACE_MAX 64

/* 追踪事件类型 */
typedef enum {
	CARBOX_DIAG_EVT_TASK_CREATE = 0,   /* 任务创建 */
	CARBOX_DIAG_EVT_TASK_DELETE,       /* 任务删除 */
	CARBOX_DIAG_EVT_STACK_WARN,        /* 栈使用率超过阈值 */
	CARBOX_DIAG_EVT_STACK_OVERFLOW,    /* 栈溢出 */
	CARBOX_DIAG_EVT_ENTER,             /* 函数进入 */
	CARBOX_DIAG_EVT_EXIT,              /* 函数退出 */
	CARBOX_DIAG_EVT_HEAP_LOW,          /* 堆空间不足 */
} carbox_diag_event_type_t;

/* ---- 初始化 ---- */

void carbox_diag_init(void);

/* ---- 任务注册（由 rtos_task_create 内部调用） ---- */

void carbox_diag_task_register(const char *name, void *handle,
			       uint32_t stack_bytes, uint32_t prio);

void carbox_diag_task_unregister(void *handle);

/* ---- 任务入口标记（在任务函数第一行调用，记录栈基线） ---- */

void carbox_diag_task_entry(const char *name);

/* ---- 栈使用率报告 ---- */

/**
 * @brief 打印所有已注册任务的栈使用情况
 * @param warn_threshold_pct 栈使用率超过此百分比时打印警告标记（0-100）
 */
void carbox_diag_stack_report(unsigned int warn_threshold_pct);

/* ---- 全面诊断 dump ---- */
void carbox_diag_dump_all(void);

/* ---- 函数追踪 ---- */

void carbox_diag_trace_enter(const char *func);
void carbox_diag_trace_exit(const char *func);

/* ---- 堆诊断 ---- */

void carbox_diag_heap_report(void);

/* ---- 追踪缓冲区 dump ---- */

void carbox_diag_trace_dump(void);

#endif /* CARBOX_DIAG_H */
