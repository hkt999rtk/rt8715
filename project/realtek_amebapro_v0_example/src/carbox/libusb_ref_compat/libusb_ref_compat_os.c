/*
 * Smart-style OS compatibility for the Pro1 USB source test.
 *
 * Pro1 has the underlying FreeRTOS primitives, but not the Smart rtos_* API
 * names used by the imported USB source.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "irq_profiler.h"
#include "usb_hcd_profiler.h"
#include "cmsis.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "os_wrapper.h"

#ifndef SUCCESS
#define SUCCESS 0
#endif

#ifndef FAIL
#define FAIL (-1)
#endif

#ifndef RTOS_MAX_TIMEOUT
#define RTOS_MAX_TIMEOUT 0xffffffffU
#endif

#define CARBOX_LIBUSB_REF_MEM_ALIGN 32U
#define CARBOX_LIBUSB_REF_MEM_MAGIC 0x43554241U

extern void hal_delay_us(uint32_t time_us);
extern size_t xPortGetFreeHeapSize(void);
extern void *pvPortMalloc(size_t xWantedSize);
extern void vPortFree(void *pv);

typedef void (*carbox_libusb_ref_task_func_t)(void *);

struct carbox_libusb_ref_mem_header {
	void *raw;
	uint32_t magic;
	uint32_t size;
};

#define CARBOX_LIBUSB_REF_CRITICAL_NEST_MAX 8U

static uint64_t carbox_libusb_ref_time_us;
static UBaseType_t carbox_libusb_ref_critical_mask[CARBOX_LIBUSB_REF_CRITICAL_NEST_MAX];
static uint32_t carbox_libusb_ref_critical_nest;

static TickType_t carbox_libusb_ref_ms_to_ticks(uint32_t wait_ms)
{
	if (wait_ms == RTOS_MAX_TIMEOUT) {
		return portMAX_DELAY;
	}

	return (TickType_t)((wait_ms + portTICK_PERIOD_MS - 1U) / portTICK_PERIOD_MS);
}

static void carbox_libusb_ref_yield_from_isr(BaseType_t task_woken)
{
	if (task_woken != pdFALSE) {
		portYIELD_FROM_ISR(task_woken);
	}
}

static void *carbox_libusb_ref_aligned_alloc(uint32_t size, int clear)
{
	uint8_t *raw;
	uint8_t *aligned;
	uintptr_t start;
	struct carbox_libusb_ref_mem_header *header;
	size_t total;

	if (size == 0U) {
		return NULL;
	}

	total = (size_t)size + CARBOX_LIBUSB_REF_MEM_ALIGN - 1U +
		sizeof(struct carbox_libusb_ref_mem_header);
	if (total < (size_t)size) {
		return NULL;
	}
	raw = (uint8_t *)pvPortMalloc(total);
	if (raw == NULL) {
		return NULL;
	}

	start = (uintptr_t)(raw + sizeof(struct carbox_libusb_ref_mem_header));
	aligned = (uint8_t *)((start + CARBOX_LIBUSB_REF_MEM_ALIGN - 1U) &
			      ~((uintptr_t)CARBOX_LIBUSB_REF_MEM_ALIGN - 1U));
	header = (struct carbox_libusb_ref_mem_header *)
		 (aligned - sizeof(struct carbox_libusb_ref_mem_header));
	header->raw = raw;
	header->magic = CARBOX_LIBUSB_REF_MEM_MAGIC;
	header->size = size;

	if (clear) {
		memset(aligned, 0, (size_t)size);
	}

	return aligned;
}

static struct carbox_libusb_ref_mem_header *
carbox_libusb_ref_mem_header_from_user(void *pbuf)
{
	struct carbox_libusb_ref_mem_header *header;

	header = (struct carbox_libusb_ref_mem_header *)
		 ((uint8_t *)pbuf - sizeof(struct carbox_libusb_ref_mem_header));
	if (header->magic != CARBOX_LIBUSB_REF_MEM_MAGIC) {
		return NULL;
	}

	return header;
}

uint32_t rtos_mem_get_free_heap_size(void)
{
	return (uint32_t)xPortGetFreeHeapSize();
}

void *rtos_mem_malloc(uint32_t size)
{
	void *pbuf = carbox_libusb_ref_aligned_alloc(size, 0);

	dbg_printf("[carbox][usb_ref][os] malloc size=%lu ptr=0x%08lx align=0x%lx free=%lu\n\r",
		   (unsigned long)size,
		   (unsigned long)pbuf,
		   (unsigned long)((uint32_t)pbuf & 0x1fU),
		   (unsigned long)xPortGetFreeHeapSize());
	return pbuf;
}

void *rtos_mem_zmalloc(uint32_t size)
{
	void *pbuf = carbox_libusb_ref_aligned_alloc(size, 1);

	dbg_printf("[carbox][usb_ref][os] zmalloc size=%lu ptr=0x%08lx align=0x%lx free=%lu\n\r",
		   (unsigned long)size,
		   (unsigned long)pbuf,
		   (unsigned long)((uint32_t)pbuf & 0x1fU),
		   (unsigned long)xPortGetFreeHeapSize());
	return pbuf;
}

void rtos_mem_free(void *pbuf)
{
	if (pbuf != NULL) {
		struct carbox_libusb_ref_mem_header *header =
			carbox_libusb_ref_mem_header_from_user(pbuf);
		void *raw = (header != NULL) ? header->raw : pbuf;

		dbg_printf("[carbox][usb_ref][os] free ptr=0x%08lx align=0x%lx free_before=%lu\n\r",
			   (unsigned long)pbuf,
			   (unsigned long)((uint32_t)pbuf & 0x1fU),
			   (unsigned long)xPortGetFreeHeapSize());
		if (header != NULL) {
			header->magic = 0U;
		}
		vPortFree(raw);
		dbg_printf("[carbox][usb_ref][os] free done free_after=%lu\n\r",
			   (unsigned long)xPortGetFreeHeapSize());
	} else {
		dbg_printf("[carbox][usb_ref][os] free null\n\r");
	}
}

void rtos_time_delay_us(uint32_t us)
{
	hal_delay_us(us);
}

void rtos_time_delay_ms(uint32_t ms)
{
	vTaskDelay(carbox_libusb_ref_ms_to_ticks(ms));
}

uint32_t rtos_time_get_current_system_time_ms(void)
{
	TickType_t tick;

	if (rtos_critical_is_in_interrupt()) {
		tick = xTaskGetTickCountFromISR();
	} else {
		tick = xTaskGetTickCount();
	}

	return (uint32_t)((tick * 1000U) / configTICK_RATE_HZ);
}

uint64_t rtos_time_get_current_system_time_us(void)
{
#if configTICK_RATE_HZ != 0
	uint64_t tick_us;

	TickType_t tick;

	if (rtos_critical_is_in_interrupt()) {
		tick = xTaskGetTickCountFromISR();
	} else {
		tick = xTaskGetTickCount();
	}

	tick_us = ((uint64_t)tick * 1000000ULL) /
		  (uint64_t)configTICK_RATE_HZ;
	if (tick_us > carbox_libusb_ref_time_us) {
		carbox_libusb_ref_time_us = tick_us;
	}
#endif

	return carbox_libusb_ref_time_us;
}

int rtos_task_create(rtos_task_t *p_handle, const char *name,
		     carbox_libusb_ref_task_func_t task_func, void *arg,
		     uint32_t stack_size, uint32_t priority)
{
	TaskHandle_t task_handle = NULL;
	BaseType_t ret;
	uint32_t stack_words;

	if (task_func == NULL) {
		return FAIL;
	}

	/*
	 * Smart rtos_task_create() receives stack size in bytes. FreeRTOS
	 * xTaskCreate() receives stack depth in StackType_t words.
	 */
	stack_words = (stack_size + (uint32_t)sizeof(StackType_t) - 1U) /
		      (uint32_t)sizeof(StackType_t);
	if (stack_words < configMINIMAL_STACK_SIZE) {
		stack_words = configMINIMAL_STACK_SIZE;
	}

	dbg_printf("[carbox][usb_ref][os] task_create name=%s stack_bytes=%lu stack_words=%lu pri=%lu\n\r",
		   name != NULL ? name : "(null)",
		   (unsigned long)stack_size,
		   (unsigned long)stack_words,
		   (unsigned long)priority);
	ret = xTaskCreate(task_func, name, (configSTACK_DEPTH_TYPE)stack_words, arg,
			  (UBaseType_t)priority, &task_handle);
	if (ret != pdPASS) {
		return FAIL;
	}

	if (p_handle != NULL) {
		*p_handle = task_handle;
	}

	return SUCCESS;
}

void rtos_task_delete(void *p_handle)
{
	vTaskDelete((TaskHandle_t)p_handle);
}

int rtos_sema_create(rtos_sema_t *p_handle, uint32_t init_count, uint32_t max_count)
{
	if (p_handle == NULL) {
		return FAIL;
	}

	*p_handle = xSemaphoreCreateCounting((UBaseType_t)max_count,
					      (UBaseType_t)init_count);

	return *p_handle != NULL ? SUCCESS : FAIL;
}

int rtos_sema_delete(void *p_handle)
{
	if (p_handle == NULL) {
		return FAIL;
	}

	vSemaphoreDelete((SemaphoreHandle_t)p_handle);
	return SUCCESS;
}

int rtos_sema_take(void *p_handle, uint32_t wait_ms)
{
	BaseType_t ret;

	if (p_handle == NULL) {
		return FAIL;
	}

	ret = xSemaphoreTake((SemaphoreHandle_t)p_handle,
			     carbox_libusb_ref_ms_to_ticks(wait_ms));

	return ret == pdTRUE ? SUCCESS : FAIL;
}

int rtos_sema_give(void *p_handle)
{
	BaseType_t ret;
	BaseType_t task_woken = pdFALSE;

	if (p_handle == NULL) {
		return FAIL;
	}

	if (rtos_critical_is_in_interrupt()) {
		ret = xSemaphoreGiveFromISR((SemaphoreHandle_t)p_handle, &task_woken);
		usb_hcd_profiler_isr_sema_give(ret == pdTRUE,
						 task_woken == pdTRUE);
		carbox_libusb_ref_yield_from_isr(task_woken);
	} else {
		ret = xSemaphoreGive((SemaphoreHandle_t)p_handle);
	}

	return ret == pdTRUE ? SUCCESS : FAIL;
}

int __real_usb_os_sema_give(void *p_handle);

int __wrap_usb_os_sema_give(void *p_handle)
{
	BaseType_t ret;
	BaseType_t task_woken = pdFALSE;

	if (p_handle == NULL) {
		return 4;
	}

	if (rtos_critical_is_in_interrupt()) {
		ret = xSemaphoreGiveFromISR((SemaphoreHandle_t)p_handle, &task_woken);
#if CONFIG_IRQ_PROFILE_USB_HANDOFF || CONFIG_USB_IRQ_SAFE_DEDUP
		carbox_irq_profiler_usb_sema_give(p_handle, ret == pdTRUE,
					       task_woken == pdTRUE);
#endif
		carbox_libusb_ref_yield_from_isr(task_woken);

		return ret == pdTRUE ? 0 : 4;
	}

	return __real_usb_os_sema_give(p_handle);
}

int rtos_mutex_create(rtos_mutex_t *p_handle)
{
	if (p_handle == NULL) {
		return FAIL;
	}

	*p_handle = xSemaphoreCreateMutex();
	return *p_handle != NULL ? SUCCESS : FAIL;
}

int rtos_mutex_delete(void *p_handle)
{
	if (p_handle == NULL) {
		return FAIL;
	}

	vSemaphoreDelete((SemaphoreHandle_t)p_handle);
	return SUCCESS;
}

int rtos_mutex_take(void *p_handle, uint32_t wait_ms)
{
	if (p_handle == NULL) {
		return FAIL;
	}

	return xSemaphoreTake((SemaphoreHandle_t)p_handle,
			      carbox_libusb_ref_ms_to_ticks(wait_ms)) == pdTRUE ?
	       SUCCESS : FAIL;
}

int rtos_mutex_give(void *p_handle)
{
	if (p_handle == NULL) {
		return FAIL;
	}

	return xSemaphoreGive((SemaphoreHandle_t)p_handle) == pdTRUE ?
	       SUCCESS : FAIL;
}

int rtos_queue_create(rtos_queue_t *p_handle, uint32_t msg_num, uint32_t msg_size)
{
	if (p_handle == NULL) {
		return FAIL;
	}

	*p_handle = xQueueCreate((UBaseType_t)msg_num,
				  (UBaseType_t)msg_size);
	return *p_handle != NULL ? SUCCESS : FAIL;
}

int rtos_queue_delete(void *p_handle)
{
	if (p_handle == NULL) {
		return FAIL;
	}

	vQueueDelete((QueueHandle_t)p_handle);
	return SUCCESS;
}

int rtos_queue_send(void *p_handle, void *p_msg, uint32_t wait_ms)
{
	BaseType_t ret;
	BaseType_t task_woken = pdFALSE;

	if (p_handle == NULL || p_msg == NULL) {
		return FAIL;
	}

	if (rtos_critical_is_in_interrupt()) {
		ret = xQueueSendFromISR((QueueHandle_t)p_handle, p_msg, &task_woken);
		carbox_libusb_ref_yield_from_isr(task_woken);
	} else {
		ret = xQueueSend((QueueHandle_t)p_handle, p_msg,
				 carbox_libusb_ref_ms_to_ticks(wait_ms));
	}

	return ret == pdTRUE ? SUCCESS : FAIL;
}

int rtos_queue_receive(void *p_handle, void *p_msg, uint32_t wait_ms)
{
	BaseType_t ret;

	if (p_handle == NULL || p_msg == NULL) {
		return FAIL;
	}

	ret = xQueueReceive((QueueHandle_t)p_handle, p_msg,
			    carbox_libusb_ref_ms_to_ticks(wait_ms));

	return ret == pdTRUE ? SUCCESS : FAIL;
}

void rtos_critical_enter(uint32_t critical_id)
{
	(void)critical_id;

	if (rtos_critical_is_in_interrupt()) {
		UBaseType_t mask = taskENTER_CRITICAL_FROM_ISR();

		if (carbox_libusb_ref_critical_nest < CARBOX_LIBUSB_REF_CRITICAL_NEST_MAX) {
			carbox_libusb_ref_critical_mask[carbox_libusb_ref_critical_nest++] = mask;
		}
	} else {
		taskENTER_CRITICAL();
	}
}

void rtos_critical_exit(uint32_t critical_id)
{
	(void)critical_id;

	if (rtos_critical_is_in_interrupt()) {
		UBaseType_t mask = 0U;

		if (carbox_libusb_ref_critical_nest > 0U) {
			mask = carbox_libusb_ref_critical_mask[--carbox_libusb_ref_critical_nest];
		}

		taskEXIT_CRITICAL_FROM_ISR(mask);
	} else {
		taskEXIT_CRITICAL();
	}
}

int rtos_critical_is_in_interrupt(void)
{
	return __get_IPSR() != 0U;
}
