/*
 * Minimal Smart-style RTOS wrapper declarations for the Pro1 USB source test.
 *
 * Keep this header ahead of the old Smart stub include path. The runtime
 * implementation lives in libusb_ref_compat_os.c.
 */
#ifndef CARBOX_LIBUSB_REF_COMPAT_OS_WRAPPER_H
#define CARBOX_LIBUSB_REF_COMPAT_OS_WRAPPER_H

#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#ifndef SUCCESS
#define SUCCESS 0
#endif

#ifndef FAIL
#define FAIL (-1)
#endif

#ifndef RTK_SUCCESS
#define RTK_SUCCESS SUCCESS
#endif

#ifndef RTOS_MAX_TIMEOUT
#define RTOS_MAX_TIMEOUT 0xffffffffU
#endif

#ifndef RTOS_SEMA_MAX_COUNT
#define RTOS_SEMA_MAX_COUNT RTOS_MAX_TIMEOUT
#endif

#ifndef MUTEX_WAIT_TIMEOUT
#define MUTEX_WAIT_TIMEOUT RTOS_MAX_TIMEOUT
#endif

#ifndef RTOS_CRITICAL_USB
#define RTOS_CRITICAL_USB 0U
#endif

typedef TaskHandle_t rtos_task_t;
typedef TaskHandle_t rtos_task_handle;
typedef SemaphoreHandle_t rtos_sema_t;
typedef SemaphoreHandle_t rtos_mutex_t;
typedef QueueHandle_t rtos_queue_t;
typedef void (*rtos_task_function_t)(void *);

uint32_t rtos_mem_get_free_heap_size(void);
void *rtos_mem_malloc(uint32_t size);
void *rtos_mem_zmalloc(uint32_t size);
void rtos_mem_free(void *pbuf);

void rtos_time_delay_us(uint32_t us);
void rtos_time_delay_ms(uint32_t ms);
uint32_t rtos_time_get_current_system_time_ms(void);
uint64_t rtos_time_get_current_system_time_us(void);

int rtos_task_create(rtos_task_t *p_handle, const char *name,
			     rtos_task_function_t task_func, void *arg,
			     uint32_t stack_size, uint32_t priority);
void rtos_task_delete(void *p_handle);

int rtos_sema_create(rtos_sema_t *p_handle, uint32_t init_count, uint32_t max_count);
int rtos_sema_delete(void *p_handle);
int rtos_sema_take(void *p_handle, uint32_t wait_ms);
int rtos_sema_give(void *p_handle);

int rtos_mutex_create(rtos_mutex_t *p_handle);
int rtos_mutex_delete(void *p_handle);
int rtos_mutex_take(void *p_handle, uint32_t wait_ms);
int rtos_mutex_give(void *p_handle);

int rtos_queue_create(rtos_queue_t *p_handle, uint32_t msg_num, uint32_t msg_size);
int rtos_queue_delete(void *p_handle);
int rtos_queue_send(void *p_handle, void *p_msg, uint32_t wait_ms);
int rtos_queue_receive(void *p_handle, void *p_msg, uint32_t wait_ms);

void rtos_critical_enter(uint32_t critical_id);
void rtos_critical_exit(uint32_t critical_id);
int rtos_critical_is_in_interrupt(void);

#endif /* CARBOX_LIBUSB_REF_COMPAT_OS_WRAPPER_H */
