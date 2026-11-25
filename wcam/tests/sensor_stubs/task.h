#include "FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
#define taskYIELD() ((void)0)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
