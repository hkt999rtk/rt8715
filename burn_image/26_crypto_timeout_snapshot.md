# Crypto IRQ timeout snapshot

`src/carbox/crypto_priority_lock.c` captures diagnostic state when the unified
AES/ChaCha completion semaphore times out. The snapshot happens before returning
failure to the caller, hence before ChaCha engine recovery resets the registers.
Timeout duration, error return and recovery policy are unchanged.

Only a timeout emits the following four lines, correlated by `generation`:

- `[CRYPTOIRQ][TIMEOUT]`: configured `timeout_ms`, measured `wait_us`, HAL
  `at_us`, RTOS `at_tick` / `tick_hz`, waiting task name/handle, owner kind
  (`1=AES`, `2=ChaCha`) and current priority.
- `[CRYPTOIRQ][HW]`: secure engine CSR, DMA busy, command completion, completion
  count, interrupt mask, 16-bit error status and source/destination FIFO status.
  `cmd_ok_masked=1` means the hardware completion interrupt is masked.
- `[CRYPTOIRQ][NVIC]`: secure Crypto IRQ 29 enable/pending/active, logical IRQ
  priority, target state (`0=secure`, `1=nonsecure`), caller PRIMASK/BASEPRI/
  FAULTMASK/IPSR and SCB ICSR. BASEPRI is a raw encoded priority value; do not
  compare its numeric value directly with the logical IRQ priority.
- `[CRYPTOIRQ][SW]`: active operation flag, accepted completion callbacks,
  waits/timeouts/spurious callbacks, last callback generation/tick and callback
  arguments, adapter initialization/interrupt mode/hook ownership and algorithm
  identifiers. These are callback counters, not raw NVIC interrupt counts.

`generation` counts hardware operations, not frames. Last callback generation
records which generation was active at callback entry; hardware supplies no
generation tag, so this cannot prove ownership of a late interrupt. A zero
last callback generation means no callback has been recorded yet. Callback
time uses the RTOS tick to avoid the HAL timer synchronization loop in an ISR.
Measured `wait_us` includes time until the waiting task actually runs again;
it is not a measurement of hardware execution alone. Unsigned 32-bit times wrap.

The normal path adds one HAL timestamp before waiting and a tick/metadata write
at callback entry, with no new successful-operation printing. On timeout,
interrupts are briefly masked only around bounded register/scalar reads and
timeout bookkeeping; the original PRIMASK is restored before RTOS calls and
printing. No Crypto register is written by the snapshot. Status can still
change as hardware progresses between reads. Keys, IVs, descriptor contents
and payload buffers are not captured.

The latest raw snapshot is also retained in `crypto_irq_timeout_detail` in
LPDDR across engine recovery. This is a handled operation failure, separate
from the fatal exception dump described in `25_fault_dump.md`.

## Interpreting a failure

- `cmd_ok=1` / nonzero completion count with no current callback points toward
  completion interrupt handling/delivery. Check the hardware mask and NVIC
  enable/pending/target state together.
- `dma_busy=1` with no completion suggests the engine was still busy at the
  snapshot. FIFO and error status provide evidence for further DMA diagnosis.
- A current-generation callback together with a semaphore timeout suggests a
  late completion or a callback/semaphore timing race, not necessarily lost IRQ.
- `wait_us` much greater than the configured timeout also includes scheduler
  delay and time during which the task could not run.

These are investigative clues, not definitive diagnoses. Masks are sampled
after the semaphore wait returns, so zero masks do not exclude earlier IRQ
masking. The vendor ISR may have acknowledged status before this callback;
clear status does not prove that the engine never completed. Diagnostic output
runs before recovery and adds UART/printf latency on the failing path only.

Build/link and linked-instruction inspection validate integration. Actual
timeout capture and recovery behavior still require a board reproduction.
