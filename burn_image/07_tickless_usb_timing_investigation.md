# FreeRTOS Tickless Impact on USB Timing

## 1. Scope

This report investigates whether the current FreeRTOS tickless configuration
can affect USB host transmit/receive timing, especially where the USB stack
contains URB state polling or timeout handling.

The investigation covers:

- the active FreeRTOS tick and tickless configuration;
- the exact tickless implementation selected by the final linker output;
- USB host core and HCD task wait mechanisms;
- USB interrupt-to-task wakeup behavior;
- URB state polling and NAK timeout handling;
- millisecond and microsecond delay wrappers;
- the boundary between the current WFI implementation and deeper PMU sleep.

## 2. Executive summary

The current tickless implementation is unlikely to introduce a meaningful USB
transfer delay or cause USB completion events to be lost.

The main reasons are:

1. USB transfer completion is interrupt/event driven.
2. The HCD interrupt wakes a semaphore-blocked USB ISR task.
3. The USB host core task blocks on a queue rather than waking every 1 ms.
4. URB polling occurs as part of the host/class state machine; it is not a
   standalone periodic polling thread that requires every SysTick interrupt.
5. The linked tickless implementation only suppresses SysTick and executes
   `WFI`. It does not currently shut down USB clocks, the system PLL, or SDRAM.
6. During active CarPlay traffic, USB and network interrupts occur frequently,
   so the system often does not remain idle long enough to enter an extended
   tickless interval.

The main future risk would be replacing the current WFI-only implementation
with a deeper Realtek PMU sleep path that disables clocks or suspends SDRAM.
That is not what the current image links.

## 3. Active FreeRTOS configuration

The project currently configures:

```c
#define configTICK_RATE_HZ       (1000)
#define configUSE_TICKLESS_IDLE  1
```

The normal scheduler tick period is therefore 1 ms.

FreeRTOS supplies the following default because this project does not override
it:

```c
#define configEXPECTED_IDLE_TIME_BEFORE_SLEEP 2
```

The idle task must predict at least two idle ticks before attempting tickless
idle. With a 1 kHz tick, this means an expected idle interval of approximately
2 ms or longer.

## 4. Tickless implementation selected by the linker

The final image contains:

```text
00011904 W vPortSuppressTicksAndSleep
```

The map file assigns it to the FreeRTOS port object:

```text
.itcm.text.vPortSuppressTicksAndSleep
    application_is/Debug/obj/port.o
```

The `W` symbol type confirms that the linked function is the weak default
implementation from the RTL8195B FreeRTOS port. No stronger PMU-specific
replacement overrides it.

The project also does not define custom `configPRE_SLEEP_PROCESSING()` or
`configPOST_SLEEP_PROCESSING()` hooks. The FreeRTOS empty defaults are used.

The current tickless sequence is therefore approximately:

```text
idle task predicts an idle interval
 -> temporarily stop SysTick
 -> program SysTick for the predicted interval
 -> confirm that sleep is still safe
 -> execute WFI
 -> wake on a pending hardware interrupt or programmed timeout
 -> restore normal SysTick operation
 -> advance xTickCount using vTaskStepTick()
```

This implementation suppresses periodic tick interrupts while idle. It does
not currently:

- stop the USB controller clock;
- disable USB interrupts;
- stop the system PLL;
- suspend SDRAM;
- invoke a board-specific deep-sleep sequence.

`platform_opts.h` contains PMU-related PLL and SDRAM settings, but those options
are not sufficient by themselves to change the current behavior because the
linked tickless function and pre/post sleep hooks do not invoke the deeper PMU
path.

## 5. USB HCD interrupt and ISR-task wakeup

Disassembly of the prebuilt USB host archive shows the USB IRQ handler doing:

```text
usbh_hcd_irq_handler
 -> usb_hal_read_interrupts()
 -> usb_os_sema_give()
```

The USB HCD ISR task waits as follows:

```c
for (;;) {
    usb_os_sema_take(hcd_semaphore, 0xFFFFFFFF);
    /* read and process USB controller interrupts */
}
```

`usb_os_sema_take()` maps to the FreeRTOS semaphore wait implementation. A
timeout of `0xFFFFFFFF` means that the task normally remains blocked until the
USB interrupt path gives the semaphore.

The effective completion path is:

```text
USB controller completes or changes channel state
 -> hardware USB interrupt becomes pending
 -> WFI wakes the CPU, if it was idle
 -> USB IRQ handler gives the HCD semaphore
 -> usbh_hcd_isr_task becomes ready
 -> task processes channel interrupt and transfer state
 -> URB state is updated
 -> USB core/class state machine is notified
```

USB interrupts remain capable of waking the CPU during the current tickless
idle. The SysTick interrupt does not need to run first.

## 6. USB host core task behavior

Disassembly of `usbh_core_task` shows an infinite queue wait:

```c
for (;;) {
    usb_os_queue_receive(core_queue, &message, 0xFFFFFFFF);
    usbh_core_process(host, message);
}
```

This is also event driven. The host core does not depend on a task that wakes
once per millisecond to call the entire host state machine.

Enumeration and class processing are performed when the queue receives state
change or processing messages. Some enumeration states intentionally sleep for
defined intervals, but those delays use normal RTOS delay semantics and are not
continuous polling.

## 7. Meaning of URB polling statistics

The USB stack contains APIs such as:

- `usbh_get_urb_state()`;
- `usbh_hcd_hc_get_urb_state()`;
- `usbh_check_nak_timeout()`;
- `usbh_cdc_ncm_process()`;
- `usbh_cdc_ncm_process_bulk_out()`.

The USB profiler can consequently report a large number of `urb polls`.

This count means that the host or CDC-NCM state machine queried a channel's URB
state. It does not by itself prove that a dedicated thread wakes every 1 ms and
polls the controller.

The normal model is closer to:

```text
submit transfer
 -> wait for interrupt/state notification
 -> class/core state machine runs
 -> query URB state
 -> submit the next transfer, retry, or wait for another event
```

Some state-machine passes can query the URB state multiple times, which is why
the polling counter can exceed the number of completed transfers. The essential
completion path remains interrupt driven.

## 8. Millisecond delays

The USB OS wrapper implements millisecond sleep as:

```text
usb_os_sleep_ms(ms)
 -> vTaskDelay(ms)
```

With a 1 kHz scheduler tick, the intended resolution is 1 ms.

Tickless idle does not leave `xTickCount` frozen from the perspective of task
timeouts. On wakeup, FreeRTOS calculates the elapsed complete tick periods and
calls `vTaskStepTick()`. A task blocked by `vTaskDelay()` therefore becomes
ready at the same logical tick deadline it would have used with periodic
SysTick interrupts.

USB enumeration and reset code contains intentional sleeps such as 5, 100, or
200 ms. Tickless may suppress the intermediate tick interrupts, but it does not
intentionally extend these delays. Their precision remains limited to scheduler
tick resolution plus small entry/exit and scheduling latency.

## 9. Microsecond delays

The USB OS wrapper implements microsecond delay as:

```text
usb_os_delay_us(us)
 -> rtos_time_delay_us(us)
```

This does not depend on the periodic FreeRTOS SysTick. The current tickless
configuration therefore has no direct effect on such microsecond delays.

These calls may still consume CPU if implemented as a busy delay, but that is a
separate performance question from tickless correctness.

## 10. NAK and timeout processing

The USB CDC-NCM host code uses tick/elapsed-time APIs and
`usbh_check_nak_timeout()` for retry and NAK handling.

When tickless wakes, the kernel advances the tick count to account for the
suppressed interval. Code measuring elapsed scheduler ticks therefore continues
to observe elapsed logical time rather than seeing time permanently stop.

Timeout resolution remains approximately 1 ms. Boundary behavior may differ by
a fraction of a tick, as it can even without tickless, but no evidence suggests
that a timeout would be delayed for the full suppressed interval.

## 11. Active traffic behavior

Tickless only executes when no non-idle task is ready and the predicted idle
time is at least two ticks.

During active CarPlay operation, the following events occur frequently:

- USB bulk completion interrupts;
- CDC-NCM IN/OUT processing;
- Wi-Fi interrupts and RX task wakeups;
- TCP/IP mailbox work;
- screen receive and send work;
- audio and timing socket activity.

These events either keep tasks ready or wake the CPU quickly. Consequently,
extended tickless intervals are much more likely during idle or low-traffic
periods than during sustained video forwarding.

Hardware USB SOF or channel interrupts, when enabled, can also wake the CPU and
thereby limit how long WFI remains active.

## 12. Expected timing impact

For the current implementation, the added latency between a USB interrupt and
task execution consists mainly of:

- exiting WFI;
- completing the tickless critical-section exit;
- entering the USB IRQ handler;
- waking and scheduling the HCD ISR task.

This is expected to be a small interrupt/scheduler latency, normally in the
microsecond range rather than a millisecond-scale delay. It is not equivalent
to waiting for the next suppressed 1 ms tick.

If the CPU is already busy with higher-priority interrupts or tasks, USB task
latency can be larger, but that scheduling contention also exists with normal
periodic ticks and should not be attributed solely to tickless idle.

## 13. Risks and limitations

### 13.1 Low risk: WFI wakeup latency

WFI adds a small entry/exit cost. This can slightly change the minimum and tail
latency of a USB completion when the system was completely idle immediately
before the interrupt.

### 13.2 Low risk: tick-boundary drift

The standard FreeRTOS implementation notes that tiny kernel-time drift is
possible while suppressing SysTick. Millisecond timeouts retain their intended
logical behavior but are not microsecond-precision deadlines.

### 13.3 Low risk: scheduler contention after wakeup

Several interrupts or tasks may become ready at the same time. USB tasks still
obey normal interrupt and task priorities, so another higher-priority workload
can delay them.

### 13.4 High future risk: deep PMU integration

If a future implementation replaces the weak WFI-only function or adds pre/post
sleep hooks that:

- stop the USB clock;
- disable the relevant PLL;
- suspend SDRAM;
- alter USB PHY state;
- mask USB as a wake source;

then USB correctness must be reviewed again. That design would require explicit
USB wake locks, clock/PHY restoration, controller-state validation, and likely
restrictions while transfers are outstanding.

This risk does not describe the currently linked image.

## 14. Recommended validation

Although the code structure indicates low risk, board testing should still
cover:

1. Repeated USB device enumeration and detach/attach cycles.
2. Sustained CDC-NCM video traffic in both directions.
3. Low-traffic periods followed by an immediate burst, exercising WFI wakeup.
4. Control and interrupt endpoints while video bulk traffic is active.
5. NAK, timeout and retry cases.
6. Long-running playback to detect rare missed wakeups or state-machine stalls.
7. Comparison of USB error, partial transfer, late interrupt and queue-depth
   counters with tickless enabled and disabled.

Useful existing observations include:

- USB HCD submit/error counters;
- URB state distribution;
- USB ISR semaphore wake/error counters;
- network RX/TX queue depth;
- CDC-NCM throughput and error logs;
- screen queue depth and `lwip_write()` latency.

## 15. Conclusion

The current FreeRTOS tickless implementation should not materially disrupt USB
send/receive timing. USB completion is driven by hardware interrupts, the HCD
task waits on a semaphore, and the core task waits on a queue. URB polling is
part of an event-driven state machine rather than a mandatory 1 ms periodic
poller.

The linked tickless function suppresses SysTick and uses WFI only. USB clocks,
PLL state, PHY state, and SDRAM remain active. USB interrupts wake the CPU
directly, while FreeRTOS advances the logical tick count after wakeup so task
delays and timeouts continue to function.

The conclusion must be revisited if deeper PMU sleep is introduced. In the
current image, however, the remaining effect is small WFI and scheduling
latency, not loss of USB timing or transfer completion.

