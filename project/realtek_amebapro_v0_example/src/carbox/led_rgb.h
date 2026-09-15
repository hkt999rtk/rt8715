#ifndef CARPLAY_LED_RGB_H
#define CARPLAY_LED_RGB_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LED_RGB_OK = 0,
    LED_RGB_INVALID_ARGUMENT = -1,
    LED_RGB_BUSY = -2,
    LED_RGB_NOT_READY = -3,
    LED_RGB_TIMEOUT = -4,
    LED_RGB_TRANSPORT_ERROR = -5,
    LED_RGB_INVALID_CONTEXT = -6
};

/* HP API: r/g/b are finite linear brightness values in [0.0f, 1.0f].
 * led_rgb(0, 0, 0) turns all channels off. Invalid values are rejected, not
 * clamped. Values are rounded to 8 bits and sent to LP; LP drives the PWM.
 * Call from an ordinary task after system startup, with interrupts enabled.
 * May sleep waiting up to about 50 ms (+ scheduling/transport delay) for LP
 * to acknowledge applying PWM. Concurrent calls are rejected as BUSY.
 * No log, allocation, animation, or customer library replacement.
 * Timeout does NOT cancel a command: LP may still apply it later.
 */
void led_rgb(float r, float g, float b);

/* Status of the most recently completed/rejected call; initially NOT_READY.
 * Global diagnostic, not a per-task completion handle. Serialize callers if
 * the status must be associated with a particular request.
 */
int led_rgb_get_status(void);

#ifdef __cplusplus
}
#endif
#endif
