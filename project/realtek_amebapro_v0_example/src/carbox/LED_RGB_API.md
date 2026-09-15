# Customer RGB LED API (HP to LP)

Include `led_rgb.h` in the HP customer source. The firmware exports the C ABI
below; the header also supports C++. No Realtek headers are required by callers.

```c
#include "led_rgb.h"

void customer_led_example(void)   /* call from a normal application task */
{
    led_rgb(1.0f, 0.0f, 0.0f);  /* red, maximum brightness */
    /* Other independent examples:
     * led_rgb(0.0f, 1.0f, 0.0f);  green
     * led_rgb(0.0f, 0.0f, 1.0f);  blue
     * led_rgb(0.5f, 0.5f, 0.5f);  half-brightness white
     * led_rgb(0.0f, 0.0f, 0.0f);  off
     */
    int status = led_rgb_get_status();
    if (status != LED_RGB_OK) {
        /* Handle or retry later according to application policy. */
    }
}
```

Each component is a finite float in `[0,1]`. Out-of-range, NaN and infinite
values are rejected without sending a command; they are not clamped. Conversion
is nearest 8-bit brightness: 0 -> 0, 0.5 -> 128, 1 -> 255. There is no gamma
correction or automatic animation.

## Calling contract

- `void led_rgb(float r, float g, float b)` waits for an LP acknowledgement
  after LP has invoked PWM updates for all three channels. `LED_RGB_OK` means
  those calls completed, not that physical light output has been measured.
- Call after normal system/RTOS startup, from an ordinary HP task with
  interrupts enabled. No separate initialization call is needed on HP.
- Not ISR-safe; do not call while the scheduler is suspended or interrupts
  are masked. It may sleep. Concurrent calls return without submitting and
  record `LED_RGB_BUSY`; there is no unbounded request queue.
- LP acknowledgement wait is 50 ms, plus ICC transport and scheduling latency;
  this is not a hard real-time bound. Timeout does not cancel a command already
  sent, so it may take effect later.
- `led_rgb_get_status()` returns the status of the most recently completed or
  rejected call. It is global, not a per-task result; serialize application
  callers if associating a status with one request matters.
- Errors are `INVALID_ARGUMENT`, `BUSY`, `NOT_READY`, `TIMEOUT`,
  `TRANSPORT_ERROR`, or `INVALID_CONTEXT`; see the header for their values.
- No logging, dynamic allocation, new HP task, or changes to customer archives.
  Old `CSystemSetup_SetLED()`/`SetLED2()` stubs are not automatically redirected;
  the customer must explicitly call `led_rgb()` from their code.

## Firmware integration

`led_rgb_hp.c` sends two ICC words (no shared buffer/cache maintenance): command
`0xBA`, 24-bit sequence, protocol version 1, and 8-bit R/G/B. LP registers `0xBA`
in its HAL command table. Its IRQ callback only publishes the pending request;
`led_rgb_lp_poll()` in the LP main loop updates PWM and sends ACK `0xBB` with
the same sequence/payload. HP ignores unrelated/stale replies. A busy ACK
transport is retried by the LP main loop. Pending requests use latest-value
semantics; this is not a lossless animation/event queue.

The HP HAL command table has one extra slot for the LED ACK handler, without
replacing the existing reset, power-management or eFuse handlers. Reserve ICC
IDs `0xBA/0xBB` for this protocol in customer code as well.

LP pins and polarity match the previous test: R=`PA_6`, G=`PA_13`, B=`PA_4`,
PWM period 1000 us (1 kHz), active-high by default. Common-anode hardware needs
the LP build define `RGB_LED_ACTIVE_LOW=1`. Do not define it only in the HP
customer application. Rebuild LP after changing polarity.

LP boots with all three channels off and retains the last requested values.
The old rainbow/breathing test source is preserved but no longer linked or
called. Main-loop servicing replaces its infinite animation loop.

**Flash the combined firmware containing BOTH updated HP and LP images.** An
old LP image has no LED receiver and the HP API will time out. SDK ICC should
remain initialized during use; runtime ICC teardown/reinitialization requires
re-registration and is not supported by this first API version.

## Verification

The HP and LP firmware builds/link checks and host mocked tests are performed;
actual pin polarity, brightness and end-to-end hardware timing still require
on-board validation. Check red/green/blue/off and confirm the API reports
`LED_RGB_OK`. In particular, LP being alive does not prove LED commands are
handled; only the application ACK completes a call successfully.

Host test from repository root:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=undefined \
  -Iproject/realtek_amebapro_v0_example/src/carbox/tests/led_rgb \
  project/realtek_amebapro_v0_example/src/carbox/tests/led_rgb/test.cpp \
  -o /tmp/rt8715-led-test
/tmp/rt8715-led-test
```

Repeat with `-DRGB_LED_ACTIVE_LOW=1` to test inverted duty. Tests cover
float packing, rejected values/contexts, PWM updates outside IRQ, ACK after
application, registration/transport failures, missing/wrong ACK, concurrent
calls, busy ACK retry, timer wrap, duplicate requests, and malformed protocol.
They do not emulate Realtek ICC hardware or PWM silicon.
