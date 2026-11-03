#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "platform_autoconf.h"

extern void console_init(void);
extern void wcam_start(void);

void main(void)
{
    console_init();
    rt_printf("[wcam] H.264 UVC color bar POC\r\n");
    wcam_start();

#if defined(CONFIG_KERNEL) && !TASK_SCHEDULER_DISABLED
#ifdef PLATFORM_FREERTOS
    vTaskStartScheduler();
#endif
#endif

    for (;;) {
    }
}
