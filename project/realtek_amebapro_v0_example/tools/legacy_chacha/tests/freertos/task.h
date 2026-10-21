#ifndef RX_POLY_TEST_TASK_H
#define RX_POLY_TEST_TASK_H
#include "FreeRTOS.h"
extern TaskHandle_t rx_poly_test_task;
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return rx_poly_test_task; }
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#endif
