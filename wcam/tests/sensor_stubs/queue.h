#include "FreeRTOS.h"
typedef struct test_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned count, unsigned item_size);
void vQueueDelete(QueueHandle_t queue);
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
#define xQueueSendFromISR(q, item, wake) xQueueSend(q, item, 0)
#define xQueueReceiveFromISR(q, item, wake) xQueueReceive(q, item, 0)
