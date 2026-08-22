#include "FreeRTOS.h"
#include "rt_printf.h"
#include "task.h"
#include "diag.h"

int FreeRTOS_errno;

extern void log_uart_flush_wait(void);

void vApplicationGetIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
                                    StackType_t ** ppxIdleTaskStackBuffer,
                                    uint32_t * pulIdleTaskStackSize )
{
    /* If the buffers to be provided to the Idle task are declared inside this
     * function then they must be declared static - otherwise they will be allocated on
     * the stack and so not exists after this function exits. */
    static StaticTask_t xIdleTaskTCB;
    static StackType_t uxIdleTaskStack[ configMINIMAL_STACK_SIZE ];

    /* Pass out a pointer to the StaticTask_t structure in which the Idle
     * task's state will be stored. */
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;

    /* Pass out the array that will be used as the Idle task's stack. */
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;

    /* Pass out the size of the array pointed to by *ppxIdleTaskStackBuffer.
     * Note that, as the array is necessarily of type StackType_t,
     * configMINIMAL_STACK_SIZE is specified in words, not bytes. */
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory( StaticTask_t ** ppxTimerTaskTCBBuffer,
                                     StackType_t ** ppxTimerTaskStackBuffer,
                                     uint32_t * pulTimerTaskStackSize )
{
    /* If the buffers to be provided to the Timer task are declared inside this
     * function then they must be declared static - otherwise they will be allocated on
     * the stack and so not exists after this function exits. */
    static StaticTask_t xTimerTaskTCB;
    static StackType_t uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];

    /* Pass out a pointer to the StaticTask_t structure in which the Idle
     * task's state will be stored. */
    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;

    /* Pass out the array that will be used as the Timer task's stack. */
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;

    /* Pass out the size of the array pointed to by *ppxTimerTaskStackBuffer.
     * Note that, as the array is necessarily of type StackType_t,
     * configMINIMAL_STACK_SIZE is specified in words, not bytes. */
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}  

/*
 * 判断一个地址是否落在已知的可执行代码段范围内（Thumb mode: bit0=1）
 * 代码段范围来自 rtl8195bhp.ld / rtl8195bhp_ram_s.ld MEMORY 布局
 */
static int prvIsCodeAddress(uint32_t addr)
{
	/* 去掉 Thumb 模式标记位 (bit0) */
	uint32_t a = addr & ~1UL;

	/* ROM (XIP)          0x10000000 - 0x100B0000 */
	if (a >= 0x10000000UL && a < 0x100B0000UL) return 1;
	/* ITCM RAM           0x00200000 - 0x00220000 */
	if (a >= 0x00200000UL && a < 0x00220000UL) return 1;
	/* DTCM ROM           0x20010000 - 0x20018000 */
	if (a >= 0x20010000UL && a < 0x20018000UL) return 1;
	/* Boot / ITCM ROM    0x00000000 - 0x00008000 */
	if (a >= 0x00000000UL && a < 0x00008000UL) return 1;
	/* SRAM code          0x20120000 - 0x20180000 (RAM mode + shared) */
	if (a >= 0x20120000UL && a < 0x20180000UL) return 1;
	/* PSRAM              0x60000000 - 0x60100000 */
	if (a >= 0x60000000UL && a < 0x60100000UL) return 1;
	/* LPDDR              0x71C00000 - 0x72000000 */
	if (a >= 0x71C00000UL && a < 0x72000000UL) return 1;

	return 0;
}

void vAssertCalled( uint32_t ulLine, const char *pcfile )
{
	const char *task_name = pcTaskGetName( NULL );
	char assert_msg[192];
	char task_list_buf[1024];
	uint32_t psp, msp, lr_val, control;
	uint32_t sp_active;
	size_t heap_free = 0;
	int i, frame;

	/* ── Step 1: 尽早捕获寄存器 ── */
	__asm volatile("mrs %0, psp"     : "=r"(psp));
	__asm volatile("mrs %0, msp"     : "=r"(msp));
	__asm volatile("mov %0, lr"      : "=r"(lr_val));
	__asm volatile("mrs %0, control" : "=r"(control));
	sp_active = (control & 2) ? psp : msp;

	/* ── Step 2: 尝试获取堆状态（可能因堆损坏失败，忽略错误） ── */
	heap_free = xPortGetFreeHeapSize();

	/* ── Step 3: 打印 ASSERT 基本信息 ── */
	rt_snprintf(assert_msg, sizeof(assert_msg),
				"\r\n========================================\r\n"
				"ASSERT FAILED: %s:%lu\r\n"
				"  current task: %s\r\n"
				"========================================\r\n",
				pcfile, (unsigned long)ulLine,
				task_name ? task_name : "(unknown)");
	rt_printf("%s", assert_msg);

	/* ── Step 4: 寄存器 dump ── */
	rt_printf("  Register dump:\r\n");
	rt_printf("    PSP    = 0x%08x\r\n", (unsigned int)psp);
	rt_printf("    MSP    = 0x%08x\r\n", (unsigned int)msp);
	rt_printf("    LR     = 0x%08x  (caller's return address)\r\n", (unsigned int)lr_val);
	rt_printf("    CTRL   = 0x%08x  (active SP: %s)\r\n",
			  (unsigned int)control, (control & 2) ? "PSP" : "MSP");

	/* ── Step 5: 堆状态 ── */
	rt_printf("  Heap free: %u bytes\r\n", (unsigned int)heap_free);

	/* ── Step 6: 调用栈回溯（从当前 SP 向下扫描） ── */
	rt_printf("----------------------------------------\r\n");
	rt_printf("  Call stack trace (scanning from SP=0x%08x, 64 words):\r\n",
			  (unsigned int)sp_active);
	{
		uint32_t *sp = (uint32_t *)sp_active;
		frame = 0;
		for (i = 0; i < 64 && frame < 16; i++) {
			if (prvIsCodeAddress(sp[i])) {
				rt_printf("    #%02d  [SP+0x%03x] 0x%08x\r\n",
						  frame++, (unsigned int)(i * 4), (unsigned int)sp[i]);
			}
		}
		if (frame == 0) {
			rt_printf("    (no return addresses found in stack window)\r\n");
		}
	}

	/* ── Step 7: 原始栈数据 dump（前 16 字，用于手动分析） ── */
	rt_printf("  Raw stack dump (SP top 16 words):\r\n");
	{
		uint32_t *sp = (uint32_t *)sp_active;
		for (i = 0; i < 16; i++) {
			rt_printf("    [%02d] 0x%08x\r\n", i, (unsigned int)sp[i]);
		}
	}

	/* ── Step 8: FreeRTOS 任务列表 ── */
	rt_printf("----------------------------------------\r\n");
	rt_printf("  Task list:\r\n");
	vTaskList(task_list_buf);
	rt_printf("%s", task_list_buf);

	/* ── Step 9: 最终 halt ── */
	rt_printf("========================================\r\n");
	rt_printf("  *** SYSTEM HALTED - Reset required ***\r\n");
	rt_printf("========================================\r\n");

	/* 等待 UART 输出完成 */
	log_uart_flush_wait();

	/* 禁用全部中断，死循环等待看门狗 / JTAG */
	__disable_irq();
	for (;;) {
	}
}

void vApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName )
{
	UBaseType_t high_water;
	extern size_t xPortGetFreeHeapSize(void);
	static char task_list_buf[512];

	asm(" nop");

	rt_printf("\r\n");
	rt_printf("========================================\r\n");
	rt_printf("  STACK OVERFLOW DETECTED\r\n");
	rt_printf("========================================\r\n");
	rt_printf("  Task name : %s\r\n", pcTaskName);
	rt_printf("  TCB       : %p\r\n", (void *)xTask);
	rt_printf("  PSP       : 0x%08x  PSPLIM : 0x%08x\r\n",
		  (unsigned int)__get_PSP(), (unsigned int)__get_PSPLIM());
	rt_printf("  MSP       : 0x%08x  MSPLIM : 0x%08x\r\n",
		  (unsigned int)__get_MSP(), (unsigned int)__get_MSPLIM());

	/* overflow task stack high water mark */
	high_water = uxTaskGetStackHighWaterMark(xTask);
	rt_printf("  Stack HWM : %u words free (%u bytes)\r\n",
		  (unsigned int)high_water,
		  (unsigned int)(high_water * sizeof(StackType_t)));

	/* heap state */
	rt_printf("  Heap free : %u bytes\r\n",
		  (unsigned int)xPortGetFreeHeapSize());

	/* all tasks snapshot */
	rt_printf("----------------------------------------\r\n");
	rt_printf("  Task list:\r\n");
	if (__get_IPSR() == 0U) {
		vTaskList(task_list_buf);
		rt_printf("%s", task_list_buf);
	} else {
		/* Stack checks can call this hook from PendSV.  vTaskList enters a
		 * task critical section and would trigger a second assertion there. */
		rt_printf("  skipped in exception context (VECT=%u)\r\n",
			  (unsigned int)__get_IPSR());
	}

	rt_printf("========================================\r\n");
	rt_printf("  System halted. Reset required.\r\n");
	rt_printf("========================================\r\n");

	/* wait for watchdog or JTAG */
	__disable_irq();
	while (1) {
		asm(" nop");
	}
}

void vApplicationTickHook( void )
{
	asm(" nop");
}

void vApplicationMallocFailedHook( void )
{
	asm(" nop");
}

// defined in port.c
void vPortUsageFaultHandler( void );

/*
 * ── 崩溃时打印当前线程完整信息 ──
 * 从 osUsageFaultHook 和 hard_fault_handler_c 回调共用
 */
static void prvFaultDumpThreadInfo(void)
{
	extern void * volatile pxCurrentTCB;
	extern size_t xPortGetFreeHeapSize(void);
	uint32_t psp_val, msp_val;
	static char task_list_buf[1024];

	__asm volatile("mrs %0, psp" : "=r"(psp_val));
	__asm volatile("mrs %0, msp" : "=r"(msp_val));

	rt_printf("\r\n========================================\r\n");
	rt_printf("  FAULT Thread Snapshot\r\n");
	rt_printf("========================================\r\n");
	rt_printf("  Current TCB   : 0x%08x\r\n", (unsigned int)pxCurrentTCB);
	rt_printf("  Task name     : %s\r\n",
		  pxCurrentTCB ? pcTaskGetName((TaskHandle_t)pxCurrentTCB) : "(null)");
	rt_printf("  PSP           : 0x%08x\r\n", (unsigned int)psp_val);
	rt_printf("  MSP           : 0x%08x\r\n", (unsigned int)msp_val);

	/* 当前任务栈高水位 */
	if (pxCurrentTCB != NULL) {
		UBaseType_t hwm = uxTaskGetStackHighWaterMark((TaskHandle_t)pxCurrentTCB);
		rt_printf("  Stack HWM     : %u words free (%u bytes)\r\n",
			  (unsigned int)hwm,
			  (unsigned int)(hwm * sizeof(StackType_t)));
	}

	/* 堆状态 */
	rt_printf("  Heap free     : %u bytes\r\n",
		  (unsigned int)xPortGetFreeHeapSize());

	/* 所有任务快照 */
	rt_printf("----------------------------------------\r\n");
	rt_printf("  All tasks:\r\n");
	vTaskList(task_list_buf);
	rt_printf("%s", task_list_buf);

	rt_printf("========================================\r\n\r\n");
}

/*
 * ── ROM hard_fault_handler_c 回调 ──
 * ROM 的故障处理器在打印完寄存器/栈/回溯后调用此函数。
 * 我们在这里补充 FreeRTOS 线程诊断信息。
 */
void carbox_hard_fault_callback(uint32_t mstack[], uint32_t pstack[],
				uint32_t lr_value, uint32_t fault_id)
{
	(void)mstack;
	(void)pstack;
	(void)lr_value;

	rt_printf("\r\n[carbox_fault] ROM fault_id=0x%08x\r\n", (unsigned int)fault_id);
	prvFaultDumpThreadInfo();

	/* 如果是 STKOF，也触发 FreeRTOS 栈溢出 hook（打印额外诊断） */
	{
		extern void * volatile pxCurrentTCB;
		uint32_t cfsr = *(volatile uint32_t *)0xE000ED28;
		uint32_t ufsr = (cfsr >> 16) & 0xFFFF;
		if (ufsr & 0x10) {
			vApplicationStackOverflowHook(
				(TaskHandle_t)pxCurrentTCB,
				pcTaskGetName((TaskHandle_t)pxCurrentTCB));
		}
	}
}

void osUsageFaultHook(void)
{
	prvFaultDumpThreadInfo();
	vPortUsageFaultHandler();
}   
