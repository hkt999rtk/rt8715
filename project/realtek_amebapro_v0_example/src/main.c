#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "platform_autoconf.h"

extern void console_init(void);

void main(void)
{
    console_init();
    rt_printf("[wcam] minimal SDK sample booted\r\n");

#if defined(CONFIG_KERNEL) && !TASK_SCHEDULER_DISABLED
#ifdef PLATFORM_FREERTOS
    vTaskStartScheduler();
#endif
#endif

    for (;;) {
    }
}
