# Audio priority and video write batches

**Goal:** Give ready audio output precedence at screen write boundaries and measure the resulting write workload.

**Approved design:** User requested AudioOutput priority 5 (screen remains 4, decoder 6), screen write submissions capped at 4096 bytes, and write counts. Apply the cap at `carbox_screen_tcp_write`; preserve the customer loop's returned-byte pointer/remaining updates and native EAGAIN poll. No sleeps, encryption changes, or unlocking inside lwIP. The cap bounds submitted bytes, not all work inside tcp_output.

**Implementation:** Use the existing exact-name xTaskCreate wrapper for audio. Keep fixed-size counters in the screen wrapper; report via the existing low-priority WLAN diagnostics task with its nonblocking UART logger. Separate capped submissions from underlying short writes and failures. Keep the diagnostic switch independent of the functional changes.

- [x] Test the actual wrapper with partial writes, EAGAIN, fatal errors, zero writes and a multi-batch byte stream; verify errno and no producer logging.
- [x] Implement priority/config stamping, 4 KB submission cap and periodic counters.
- [x] Verify the existing customer archive loop/relocations and final ELF priority and write call targets.
- [x] Build IS with the external Realtek toolchain, verify flash_is.bin and SHA-256.

**Review focus:** Only AudioOutput priority changes; screen header/ciphertext order remains unchanged; EAGAIN does not advance data; diagnostics disabled still caps writes; failed/skipped reports are not audio drops. Test on hardware for mutex-wait tails, audio packet spacing and screen throughput. No commit/push requested.

## Verification result

Build `20261104153836_A312` passed with the log18 navigation flags retained. Host batch/errno/config/priority tests, 5 archive tests and final ELF NAV_TX/pacing checks passed. Independent review identified and regression tests verified fixes for poll-disabled reporter linkage and shared errno preservation across added instrumentation. Existing global errno race remains outside this change. Hardware validation pending. Build log contains existing awk escape and submake jobserver warnings, no compile/link errors.
