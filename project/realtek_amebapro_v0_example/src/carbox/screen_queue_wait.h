#ifndef CARBOX_SCREEN_QUEUE_WAIT_H
#define CARBOX_SCREEN_QUEUE_WAIT_H

#include <stdint.h>

#include "FreeRTOS.h"

void carbox_screen_queue_publish_begin(void);
void carbox_screen_queue_publish_end(void);
void carbox_screen_queue_push_result(void *vector, int pushed);
void carbox_screen_queue_after_erase(void *vector, int remaining);
void carbox_screen_queue_wait(TickType_t timeout_ticks);
void carbox_screen_queue_wait_report(uint32_t sequence);

#endif /* CARBOX_SCREEN_QUEUE_WAIT_H */
