#include "device.h"
#include "gpio_api.h"
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
static gpio_t red_off, green_off, blue_off;
static volatile uint32_t pending_sequence, pending_payload;
static volatile uint8_t pending;
static uint32_t applied_sequence, applied_payload;
static int initialized, pwm_ready, ack_pending;
static int pins_prepared;

#if RGB_LED_ACTIVE_LOW
#define LED_RGB_OFF_LEVEL 1
#define LED_RGB_OFF_DUTY 1.0f
#define LED_RGB_OFF_PULL PullUp
#else
#define LED_RGB_OFF_LEVEL 0
#define LED_RGB_OFF_DUTY 0.0f
#define LED_RGB_OFF_PULL PullDown
#endif

static void hold_channel_off(gpio_t *gpio, PinName pin)
{
    gpio_init(gpio, pin);
    gpio_mode(gpio, LED_RGB_OFF_PULL);
    /* Load the output latch before changing direction. */
    gpio_write(gpio, LED_RGB_OFF_LEVEL);
    gpio_dir(gpio, PIN_OUTPUT);
}

void led_rgb_lp_prepare_pins_off(void)
{
    if (pins_prepared) return;
    hold_channel_off(&red_off, PA_6);
    hold_channel_off(&green_off, PA_13);
    hold_channel_off(&blue_off, PA_4);
    pins_prepared = 1;
}

static int init_channel_off(pwmout_t *channel, gpio_t *off_gpio, PinName pin)
{
    gpio_deinit(off_gpio);
    if (pwmout_init_with_duty(channel, pin, 1000, LED_RGB_OFF_DUTY) != 0) {
        hold_channel_off(off_gpio, pin);
        return -1;
    }
    return 0;
}

static int start_pwm(void)
{
    if (pwm_ready) return 0;
    if (init_channel_off(&red, &red_off, PA_6) != 0 ||
        init_channel_off(&green, &green_off, PA_13) != 0 ||
        init_channel_off(&blue, &blue_off, PA_4) != 0) {
        initialized = -1;
        dbg_printf("[LED_RGB][LP] PWM init failed\r\n");
        return -1;
    }
    pwm_ready = 1;
    return 0;
}

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
    if (initialized > 0) return 0;
    if (initialized < 0) return -1;
    led_rgb_lp_prepare_pins_off();
    icc_hal_app_init(); /* Idempotent SDK boot initialization. */
    /* Keep all channels as off-level GPIOs through startup. The PWM HAL may
     * change pinmux/output during initialization, so defer it until a color
     * other than off is actually requested. */
    if (hal_icc_hal_cmd_register(&icc_hal_adp, LED_RGB_ICC_SET,
                                 led_rgb_request, 0) != HAL_OK) {
        dbg_printf("[LED_RGB][LP] ICC register failed\r\n");
        return -1;
    }
    initialized = 1;
    dbg_printf("[LED_RGB][LP] ready pins=PA_6,PA_13,PA_4 active_low=%u\r\n",
               (unsigned)RGB_LED_ACTIVE_LOW);
    return 0;
}

void led_rgb_lp_poll(void)
{
    uint32_t seq, payload, primask;
    uint8_t have_request;
    if (initialized <= 0) return;
    primask = __get_PRIMASK();
    __disable_irq();
    seq = pending_sequence;
    payload = pending_payload;
    have_request = pending;
    pending = 0;
    __set_PRIMASK(primask);
    if (have_request && seq) {
        if ((payload & 0x00ffffffUL) && start_pwm() != 0)
            return; /* No ACK: the requested color was not applied. */
        if (pwm_ready) {
            set_channel(&red, (uint8_t)(payload >> 16));
            set_channel(&green, (uint8_t)(payload >> 8));
            set_channel(&blue, (uint8_t)payload);
        }
        applied_sequence = seq;
        applied_payload = payload;
        ack_pending = 1;
    }
    if (ack_pending && hal_icc_l2h_cmd_send(&icc_hal_adp,
            (LED_RGB_ICC_ACK << 24) | applied_sequence, applied_payload, 0) == HAL_OK) {
        ack_pending = 0;
        dbg_printf("[LED_RGB][LP] applied seq=%lu rgb=%lu,%lu,%lu ack=ok\r\n",
                   (unsigned long)applied_sequence,
                   (unsigned long)((applied_payload >> 16) & 0xffU),
                   (unsigned long)((applied_payload >> 8) & 0xffU),
                   (unsigned long)(applied_payload & 0xffU));
    }
}
