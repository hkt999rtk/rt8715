# WLAN RX zero-copy working baseline

Validated on hardware on 2026-08-16.

- Git base: `68f442740aaa01c2bcbbd1f97b21bbb5bcc3664c`
- Firmware build ID: `20260816112739_44A3`
- `application_is/flash_is.bin` SHA-256:
  `37f6dcf2b5a80da150d6acccd736532d866fdacdb26c53bdd7f945636e1f1a89`
- Hardware result: CarPlay pairing, video, and audio operational.
- RX DMA result: `zero_copy` increased from 18432 through 22528,
  `adopted=24`, `live=24`, and `fallback=0/0`.

Recovered source SHA-256 values at the validated build:

- `rtl8195b_recv_recovered.c`:
  `78de0a9fa7d438b20f5636dd48d7a20be0fb976984aa0f03f6bd08ec24a39b6a`
- `freertos_skbuff_recovered.c`:
  `e4c281da222589c572d0c705b26986e897bc945a0d0438d6806fd3428f22f86b`
- `wlan_rx_dma_mgr.c`:
  `435f06bfb55816297c1fe231bbee3f6802e5ff7fb6892333df0fd680d120c2e4`
- `wlan_rx_dma_mgr.h`:
  `97be9eeade673685bcede4031faa9fe3448f2bc6018c8b435e78bd08363ee64a`

The validated ownership rule is deliberately narrow: ordinary dynamically
allocated skbs retain vendor `dyalloc_flag == 1` behavior. Only DMA descriptor
skbs use `dyalloc_flag == 2`, which enables shared reference counting across
`skb_clone()` and `kfree_skb()`.

## Canonical archive validation

The same validated object composition was promoted from the temporary
`lib_wlan_zero_copy_minimal.a` name to the canonical
`build/lib_wlan_zero_copy.a` target in commit `54b9f82`.

- Firmware build ID: `20260816120335_2909`
- `application_is/flash_is.bin` SHA-256:
  `60443e2b13ea6b389b38df857319c5cef23d4b30335b143d9df3b84c07cab512`
- `build/lib_wlan_zero_copy.a` SHA-256:
  `575225c264f0d4fb855bb33b69c74c535bb18b84d6b8a2a60fbc02129291a0f6`
- Hardware result: CarPlay pairing, video, and audio operational under load.
- RX DMA result: `zero_copy` increased from 2048 to 3072,
  `adopted=24`, `live=24`, and `fallback=0/0`.
- Observed TCP backpressure recovered normally: no pending screen-block
  episode, queue overflow, zero advertised window, or zero-copy fallback.
