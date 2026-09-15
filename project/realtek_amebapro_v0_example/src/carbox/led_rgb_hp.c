#include "led_rgb.h"
#include "led_rgb_protocol.h"
#include "hal_icc.h"
#include "hal_timer.h"
#include "FreeRTOS.h"
#include "task.h"

extern hal_icc_adapter_t icc_hal_adp;

static volatile int last_status = LED_RGB_NOT_READY;
static uint8_t active, registered;
static uint32_t sequence;
static volatile uint32_t expected_sequence, expected_payload, acknowledged;

static void led_rgb_ack(uint32_t command, uint32_t payload, uint32_t arg)
{
    (void)arg;
    if ((command >> 24) == LED_RGB_ICC_ACK &&
        (command & LED_RGB_SEQUENCE_MASK) == expected_sequence &&
        expected_sequence != 0 && payload == expected_payload) {
        __DMB();
        acknowledged = 1;
    }
}

int led_rgb_get_status(void)
{
    return last_status;
}

void led_rgb(float r, float g, float b)
{
    uint32_t payload, start;
    hal_status_t status;
    int result;

    if (!led_rgb_pack(r, g, b, &payload)) {
        last_status = LED_RGB_INVALID_ARGUMENT;
        return;
    }
    if (__get_IPSR() || __get_PRIMASK() || __get_BASEPRI() ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        last_status = LED_RGB_INVALID_CONTEXT;
        return;
    }
    taskENTER_CRITICAL();
    if (active) {
        last_status = LED_RGB_BUSY;
        taskEXIT_CRITICAL();
        return;
    }
    active = 1;
    if (!registered) {
        /* Boot already initializes ICC. Do not reinitialize/reset queues. */
        status = hal_icc_hal_cmd_register(&icc_hal_adp, LED_RGB_ICC_ACK,
                                          led_rgb_ack, 0);
        if (status != HAL_OK) {
            active = 0;
            last_status = LED_RGB_NOT_READY;
            taskEXIT_CRITICAL();
            return;
        }
        registered = 1;
    }
    sequence = (sequence + 1U) & LED_RGB_SEQUENCE_MASK;
    if (!sequence) sequence = 1;
    expected_sequence = sequence;
    expected_payload = payload;
    acknowledged = 0;
    __DMB();
    taskEXIT_CRITICAL();

    status = hal_icc_h2l_cmd_send(&icc_hal_adp,
                                 (LED_RGB_ICC_SET << 24) | sequence,
                                 payload, 1000);
    if (status != HAL_OK) {
        result = status == HAL_BUSY ? LED_RGB_BUSY :
                 status == HAL_TIMEOUT ? LED_RGB_TIMEOUT : LED_RGB_TRANSPORT_ERROR;
    } else {
        start = hal_read_curtime_us();
        while (!acknowledged && (uint32_t)(hal_read_curtime_us() - start) < 50000U)
            vTaskDelay(1);
        result = acknowledged ? LED_RGB_OK : LED_RGB_TIMEOUT;
    }
    taskENTER_CRITICAL();
    expected_sequence = 0; /* Ignore late ACKs after this call completes. */
    last_status = result;
    active = 0;
    taskEXIT_CRITICAL();
}
