#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"

int FreeRTOS_errno;

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

void vAssertCalled( uint32_t ulLine, const char *pcfile )
{
	//
    volatile int lock_assert = 1;
    rt_printf("line %d file: %s\n\r", ulLine, pcfile);
	// let watchdog reset work if assert happen
	__disable_irq();
	while(lock_assert);
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
	vTaskList(task_list_buf);
	rt_printf("%s", task_list_buf);

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
void osUsageFaultHook(void)
{
    vPortUsageFaultHandler();
}
