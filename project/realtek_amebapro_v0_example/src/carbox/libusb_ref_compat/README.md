# Pro1 USB Compatibility Layer

This directory contains compatibility code used while bringing the newer USB
reference source drop into Pro1.

## Current Path

The active `CARBOX_USB_REF_TEST=1` source-only path uses only:

```text
os_wrapper.h
libusb_ref_compat_os.c
```

These files provide the Smart-style `rtos_*` API names expected by the USB
source under:

```text
third_party/amebapro_sz_usb_lib/usb/
third_party/carbox_smart/product_logic/reference/carbox_smart/component/usb/
```

They are a Pro1 OS port, not a USB protocol wrapper.

Important behavior:

```text
rtos_task_create stack_size follows Smart semantics: bytes, converted to FreeRTOS words
rtos_mem_malloc/rtos_mem_zmalloc return 32-byte aligned buffers for USB DMA
ISR context is detected with __get_IPSR()
critical sections use ISR-safe FreeRTOS APIs when entered from ISR
semaphore give and queue send use FromISR variants when called from ISR
time getters use xTaskGetTickCountFromISR() in ISR context
```

## Historical Files

The other files in this directory are from the older
`CARBOX_EXPERIMENTAL_SMART_A_LINK=1` experiment that attempted to link the
Smart archive/full-app path:

```text
libusb_ref_compat_hal.c
libusb_ref_compat_lwip_bridge.c
libusb_ref_compat_ipv6.c
```

They are not compiled by the current `CARBOX_USB_REF_TEST=1` path. Keep them
only if the historical Smart archive/full-app experiment still needs to remain
buildable.

## Boundary

Passing link with this module proves only compile/link integration. Runtime
USB validation still requires board testing of CDC ACM enumeration and echo.
