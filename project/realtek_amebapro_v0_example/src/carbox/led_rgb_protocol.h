#ifndef CARPLAY_LED_RGB_PROTOCOL_H
#define CARPLAY_LED_RGB_PROTOCOL_H
#include <stdint.h>

/* Project-reserved ICC command IDs, registered in the HAL command table so
 * LP can receive them without a message queue/RTOS task. No shared pointers.
 * word0: command[31:24], nonzero sequence[23:0].
 * word1: version[31:24]=1, red[23:16], green[15:8], blue[7:0].
 * ACK echoes the request sequence/payload AFTER applying all three PWMs.
 */
#define LED_RGB_ICC_SET 0xBAU
#define LED_RGB_ICC_ACK 0xBBU
#define LED_RGB_VERSION 0x01000000UL
#define LED_RGB_SEQUENCE_MASK 0x00ffffffUL

static inline int led_rgb_pack(float r, float g, float b, uint32_t *word)
{
    /* Ordered comparisons also reject NaN and infinity without libm. */
    if (!(r >= 0.0f && r <= 1.0f && g >= 0.0f && g <= 1.0f &&
          b >= 0.0f && b <= 1.0f))
        return 0;
    *word = LED_RGB_VERSION | ((uint32_t)(r * 255.0f + 0.5f) << 16) |
            ((uint32_t)(g * 255.0f + 0.5f) << 8) |
            (uint32_t)(b * 255.0f + 0.5f);
    return 1;
}
#endif
