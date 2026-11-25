#include "wcam/wcam_config.h"
#include "wcam/wcam_net.h"
#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "platform_autoconf.h"

extern void console_init(void);
extern void wcam_start(void);
extern void wcam_tx_start(void);

void main(void)
{
    console_init();
    rt_printf("[wcam][boot] diag=4 role=%s build=%s %s\r\n",WCAM_ROLE_NAME,__DATE__,__TIME__);
#if WCAM_ROLE_TX || WCAM_ROLE_RX
    if(wcam_net_start()<0) { rt_printf("[wcam] network task creation failed\r\n"); for(;;){} }
#endif
#if WCAM_ROLE_TX
    wcam_tx_start();
#else
    wcam_start();
#endif

#if defined(CONFIG_KERNEL) && !TASK_SCHEDULER_DISABLED
#ifdef PLATFORM_FREERTOS
    vTaskStartScheduler();
#endif
#endif

    for (;;) {
    }
}
