#include "screen_rx_poly_buffer.h"
#include <stdlib.h>

#ifndef CONFIG_SCREEN_RX_POLY_INPLACE
#define CONFIG_SCREEN_RX_POLY_INPLACE 0
#endif

#if CONFIG_SCREEN_RX_POLY_INPLACE
#include "FreeRTOS.h"
#include "task.h"

#define RX_POLY_SLOTS 64U
#define RX_POLY_AAD 128U
#define RX_POLY_ALIGNMENT 32U
#define RX_POLY_MIN_DATA 4096U
#define RX_POLY_MAX_INPUT 65536U

typedef struct {
    void *allocation;
    uint8_t *payload;
    size_t wire_length;
    TaskHandle_t task;
} rx_poly_owner;

/* Independent of the optional handover tracking table: even a copy fallback
 * or a disabled handover transaction must release the original malloc base. */
static rx_poly_owner rx_poly_owners[RX_POLY_SLOTS];
#endif

void *carbox_screen_rx_poly_alloc(size_t wire_length)
{
    if (wire_length > SIZE_MAX - 15U) return NULL;
#if CONFIG_SCREEN_RX_POLY_INPLACE
    /* Only lengths that can use standalone hardware Poly1305 need a prefix.
     * wire_length includes the original tag. Rounded data + AAD + lengths
     * must fit the hardware's 64 KiB one-shot input limit. */
    if (wire_length >= RX_POLY_MIN_DATA + 16U &&
        wire_length <= RX_POLY_MAX_INPUT - RX_POLY_AAD &&
        ((wire_length - 16U) & 15U) != 0U) {
        /* Retain the old readable wire_length + 15 contract as well as the
         * Poly1305 suffix, and keep the last cache line inside this malloc. */
        size_t span = RX_POLY_AAD + wire_length + 15U;
        size_t aligned_span = (span + RX_POLY_ALIGNMENT - 1U) &
                              ~(size_t)(RX_POLY_ALIGNMENT - 1U);
        void *allocation = malloc(aligned_span + RX_POLY_ALIGNMENT - 1U);
        unsigned i;
        if (allocation != NULL) {
            uint8_t *input = (uint8_t *)(((uintptr_t)allocation +
                RX_POLY_ALIGNMENT - 1U) & ~(uintptr_t)(RX_POLY_ALIGNMENT - 1U));
            TaskHandle_t task = xTaskGetCurrentTaskHandle();
            taskENTER_CRITICAL();
            for (i = 0; i < RX_POLY_SLOTS; ++i) {
                if (rx_poly_owners[i].allocation == NULL) {
                    rx_poly_owners[i].allocation = allocation;
                    rx_poly_owners[i].payload = input + RX_POLY_AAD;
                    rx_poly_owners[i].wire_length = wire_length;
                    rx_poly_owners[i].task = task;
                    taskEXIT_CRITICAL();
                    return input + RX_POLY_AAD;
                }
            }
            taskEXIT_CRITICAL();
            /* Never return an offset pointer without a release record. */
            free(allocation);
        }
    }
#endif
    return malloc(wire_length + 15U);
}

void carbox_screen_rx_poly_free(void *payload)
{
    void *allocation = payload;
#if CONFIG_SCREEN_RX_POLY_INPLACE
    unsigned i;
    if (payload == NULL) return;
    taskENTER_CRITICAL();
    for (i = 0; i < RX_POLY_SLOTS; ++i) {
        if (rx_poly_owners[i].payload == payload) {
            allocation = rx_poly_owners[i].allocation;
            rx_poly_owners[i] = (rx_poly_owner){0};
            break;
        }
    }
    taskEXIT_CRITICAL();
#endif
    free(allocation);
}

uint8_t *carbox_screen_rx_poly_input(const void *payload, size_t data_length,
                                    size_t aad_length, size_t input_length)
{
#if CONFIG_SCREEN_RX_POLY_INPLACE
    uint8_t *input = NULL;
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    unsigned i;
    if (payload == NULL || aad_length != RX_POLY_AAD ||
        data_length < RX_POLY_MIN_DATA || data_length > RX_POLY_MAX_INPUT ||
        input_length > RX_POLY_MAX_INPUT ||
        input_length != RX_POLY_AAD + ((data_length + 15U) & ~(size_t)15U) + 16U)
        return NULL;
    taskENTER_CRITICAL();
    for (i = 0; i < RX_POLY_SLOTS; ++i) {
        if (rx_poly_owners[i].payload == payload &&
            rx_poly_owners[i].task == task &&
            rx_poly_owners[i].wire_length == data_length + 16U) {
            input = rx_poly_owners[i].payload - RX_POLY_AAD;
            break;
        }
    }
    taskEXIT_CRITICAL();
    return input;
#else
    (void)payload;
    (void)data_length;
    (void)aad_length;
    (void)input_length;
    return NULL;
#endif
}
