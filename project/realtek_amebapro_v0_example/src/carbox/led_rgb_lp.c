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
static uint8_t red_pwm, green_pwm, blue_pwm;
static volatile uint32_t pending_sequence, pending_payload;
static volatile uint8_t pending;
static uint32_t applied_sequence, applied_payload;
static int initialized, ack_pending;
static int pins_prepared;

#if RGB_LED_ACTIVE_LOW
#define LED_RGB_OFF_LEVEL 1
#define LED_RGB_OFF_PULL PullUp
#else
#define LED_RGB_OFF_LEVEL 0
#define LED_RGB_OFF_PULL PullDown
#endif
#define LED_RGB_ON_LEVEL (!LED_RGB_OFF_LEVEL)

static void hold_channel_off(gpio_t *gpio, PinName pin)
{
    gpio_init(gpio, pin);
    gpio_mode(gpio, LED_RGB_OFF_PULL);
    /* Load the output latch before changing direction. */
    gpio_write(gpio, LED_RGB_OFF_LEVEL);
    gpio_dir(gpio, PIN_OUTPUT);
}

void led_rgb_lp_prepare_boot_red(void)
{
    if (pins_prepared) return;
    hold_channel_off(&red_off, PA_6);
    hold_channel_off(&green_off, PA_13);
    hold_channel_off(&blue_off, PA_4);
    gpio_write(&red_off, LED_RGB_ON_LEVEL);
    pins_prepared = 1;
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

static int set_channel(pwmout_t *channel, gpio_t *gpio, uint8_t *using_pwm,
                       PinName pin, uint8_t value)
{
    float duty;

    /* Exact off/on does not require PWM. In particular, the LP PWM HAL may
     * reject a 100% duty during initialization, leaving active-low LEDs
     * uncontrollable after the first command. */
    if (value == 0U || value == 255U) {
        int level = value ? LED_RGB_ON_LEVEL : LED_RGB_OFF_LEVEL;
        if (*using_pwm) {
            pwmout_free(channel);
            hold_channel_off(gpio, pin);
            *using_pwm = 0U;
        }
        gpio_write(gpio, level);
        return 0;
    }

    duty = (float)value / 255.0f;
#if RGB_LED_ACTIVE_LOW
    duty = 1.0f - duty;
#endif
    if (!*using_pwm) {
        gpio_write(gpio, LED_RGB_OFF_LEVEL);
        gpio_deinit(gpio);
        if (pwmout_init_with_duty(channel, pin, 1000, duty) != 0) {
            hold_channel_off(gpio, pin);
            dbg_printf("[LED_RGB][LP] PWM init failed pin=%u value=%u\r\n",
                       (unsigned)pin, (unsigned)value);
            return -1;
        }
        *using_pwm = 1U;
    } else {
        pwmout_write(channel, duty);
    }
    return 0;
}

int led_rgb_lp_init(void)
{
    if (initialized > 0) return 0;
    if (initialized < 0) return -1;
    led_rgb_lp_prepare_boot_red();
    icc_hal_app_init(); /* Idempotent SDK boot initialization. */
    /* Keep the startup color on GPIOs. Only intermediate brightness uses PWM. */
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
        if (set_channel(&red, &red_off, &red_pwm, PA_6,
                        (uint8_t)(payload >> 16)) != 0 ||
            set_channel(&green, &green_off, &green_pwm, PA_13,
                        (uint8_t)(payload >> 8)) != 0 ||
            set_channel(&blue, &blue_off, &blue_pwm, PA_4,
                        (uint8_t)payload) != 0)
            return; /* No ACK: at least one channel was not applied. */
        applied_sequence = seq;
        applied_payload = payload;
        ack_pending = 1;
    }
    if (ack_pending && hal_icc_l2h_cmd_send(&icc_hal_adp,
            (LED_RGB_ICC_ACK << 24) | applied_sequence, applied_payload, 0) == HAL_OK) {
        ack_pending = 0;
        dbg_printf("[LED_RGB][LP] applied seq=%lu rgb=%lu,%lu,%lu pwm_mask=0x%x ack=ok\r\n",
                   (unsigned long)applied_sequence,
                   (unsigned long)((applied_payload >> 16) & 0xffU),
                   (unsigned long)((applied_payload >> 8) & 0xffU),
                   (unsigned long)(applied_payload & 0xffU),
                   (unsigned)((red_pwm ? 4U : 0U) |
                              (green_pwm ? 2U : 0U) |
                              (blue_pwm ? 1U : 0U)));
    }
}
