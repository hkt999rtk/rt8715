#include "device.h"
#include "pwmout_api.h"
#include "hal_icc.h"
#include "led_rgb_lp.h"
#include "led_rgb_protocol.h"

extern hal_icc_adapter_t icc_hal_adp;
extern void icc_hal_app_init(void);

#ifndef RGB_LED_ACTIVE_LOW
#define RGB_LED_ACTIVE_LOW 0
#endif

static pwmout_t red, green, blue;
static volatile uint32_t pending_sequence, pending_payload;
static volatile uint8_t pending;
static uint32_t applied_sequence, applied_payload;
static int initialized, ack_pending;

static void led_rgb_request(uint32_t command, uint32_t payload, uint32_t arg)
{
    (void)arg;
    if ((command >> 24) != LED_RGB_ICC_SET ||
        (payload & 0xff000000UL) != LED_RGB_VERSION ||
        !(command & LED_RGB_SEQUENCE_MASK))
        return;
    /* IRQ callback publishes only. PWM and ACK are handled by the main loop. */
    pending_payload = payload;
    __DMB();
    pending_sequence = command & LED_RGB_SEQUENCE_MASK;
    pending = 1;
}

static void set_channel(pwmout_t *channel, uint8_t value)
{
    float duty = (float)value / 255.0f;
#if RGB_LED_ACTIVE_LOW
    duty = 1.0f - duty;
#endif
    pwmout_write(channel, duty);
}

int led_rgb_lp_init(void)
{
    if (initialized) return 0;
    icc_hal_app_init(); /* Idempotent SDK boot initialization. */
    pwmout_init(&red, PA_6);
    pwmout_init(&green, PA_13);
    pwmout_init(&blue, PA_4);
    pwmout_period_us(&red, 1000);
    pwmout_period_us(&green, 1000);
    pwmout_period_us(&blue, 1000);
    set_channel(&red, 0);
    set_channel(&green, 0);
    set_channel(&blue, 0);
    if (hal_icc_hal_cmd_register(&icc_hal_adp, LED_RGB_ICC_SET,
                                 led_rgb_request, 0) != HAL_OK)
        return -1;
    initialized = 1;
    return 0;
}

void led_rgb_lp_poll(void)
{
    uint32_t seq, payload, primask;
    uint8_t have_request;
    if (!initialized) return;
    primask = __get_PRIMASK();
    __disable_irq();
    seq = pending_sequence;
    payload = pending_payload;
    have_request = pending;
    pending = 0;
    __set_PRIMASK(primask);
    if (have_request && seq) {
        set_channel(&red, (uint8_t)(payload >> 16));
        set_channel(&green, (uint8_t)(payload >> 8));
        set_channel(&blue, (uint8_t)payload);
        applied_sequence = seq;
        applied_payload = payload;
        ack_pending = 1;
    }
    if (ack_pending && hal_icc_l2h_cmd_send(&icc_hal_adp,
            (LED_RGB_ICC_ACK << 24) | applied_sequence, applied_payload, 0) == HAL_OK)
        ack_pending = 0;
}
