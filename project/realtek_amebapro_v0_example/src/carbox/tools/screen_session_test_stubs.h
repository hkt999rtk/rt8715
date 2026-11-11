#ifndef TEST_STUBS_H
#define TEST_STUBS_H
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <stdio.h>
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef int StaticSemaphore_t;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(x) (x)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
void vTaskDelay(unsigned);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *);
int xSemaphoreTake(SemaphoreHandle_t,unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
int lwip_write(int,const void*,size_t);
int lwip_poll(struct pollfd*,unsigned,int);
int lwip_shutdown(int,int);
int lwip_errno(void);
int lwip_usb_screen_rto_enable(int);
uint32_t hal_read_curtime_us(void);
#define rt_printf printf
#endif
