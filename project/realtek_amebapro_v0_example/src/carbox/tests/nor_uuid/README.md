# EN25S64A UID and OTP readers

The same harness now covers `carplay_nor_read_otp()`; see `../../nor_otp.md`
for its interface, read-only command sequence, and additional test coverage.

`../../nor_uuid.h` exposes `carbox_nor_read_uuid(buffer, capacity)`. It returns
12 bytes in increasing UID address order, or a negative error code defined in
the header. This is a 96-bit factory UID, not an RFC 128-bit UUID. There is no
allocation, flash programming or OTP modification. The optional boot check is
described in `../../nor_otp.md`.
`carbox_nor_identity_cache_init()` runs in early main before tasks/WLAN and
loads 12-byte UID plus full 512-byte OTP. Runtime APIs only read static SRAM;
failed initialization returns its saved error, never a lazy hardware retry.
Error diagnostics occur during boot under `[nor-uuid]` and `[nor-otp]`.
`CARBOX_NOR_BOOT_DIAG=0` disables the summary, not initialization.

Example, after main has initialized the cache:

```c
#include "nor_uuid.h"

uint8_t uid[CARBOX_NOR_UUID_SIZE];
int result = carbox_nor_read_uuid(uid, sizeof(uid));
if (result == CARBOX_NOR_UUID_SIZE) {
    /* Consume uid[0..11]; format as 24 hexadecimal digits if needed. */
}
```

The implementation is specific to EN25S64A, JEDEC `1C 38 17`, as documented in
EN25S64A Rev.H (2020-10-22), pp. 15, 64 and 70. UID is read using `5A 00 00 80`
followed by one dummy byte and 12 received bytes. The controller sends the dummy
as a fourth address-phase byte, keeping CS asserted across the entire transfer.
It checks JEDEC ID, SFDP signature, two matching UID reads, and rejects all-zero
or all-FF results as suspect data (an application validation policy).

SPI, dual/quad STR and QPI STR are accepted; DTR and other chips are rejected.
QPI is temporarily exited with FF and restored with 38 by the private early
boot readers. Touched SPIC registers and incoming PRIMASK/I/D-cache enable state
are saved/restored without SDK RTOS tick compensation. Command reads temporarily
use divider 10 and the calibrated RX sampling residual, restoring XIP settings
before returning. Hardware command code and caches are linked into SRAM.

Public read APIs do not access flash or take locks. Output must be writable RAM;
failed reads leave it unchanged. Before initialization they return NOT_READY.
Bounded hardware cleanup is best effort; cache logic does not prove faulty
flash/controller restoration or correct physical timing.

## Host tests

From the repository root:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=undefined \
  -Iproject/realtek_amebapro_v0_example/src/carbox/tests/nor_uuid \
  -Icomponent/soc/realtek/8195b/fwlib/hal-rtl8195b-hp/include \
  project/realtek_amebapro_v0_example/src/carbox/tests/nor_uuid/test.cpp \
  -o /tmp/rt8715-nor-uuid-test
/tmp/rt8715-nor-uuid-test
```

Tests compile the production implementation against a scripted MMIO proxy using
the SDK register bitfield types. They check byte sequences, lengths, channels,
output bounds, unchanged output on errors, register restoration, QPI restoration,
invalid arguments/chips/modes, bad response data and per-transaction timeouts.
They do NOT emulate physical SPIC FIFO/address decoding, clocks, or flash timing.

Legacy OTP wrapper tests cover both `spinor_read_otp` and `CarApi_GetFlashOTP`
replacements: success returns 1 for exactly 16 bytes, cached errors -1..-6 pass
through, pre-init/bounds/NULL errors leave output untouched, and no hardware
transactions or guards occur. The firmware link also enables `--wrap` for both
names; archive call-site resolution must be checked when vendor libraries change.

## Customer timeout diagnostics

`CARBOX_NOR_UUID_DIAG` defaults to 1 in `nor_uuid.h` for this investigation.
Compile with `-DCARBOX_NOR_UUID_DIAG=0` (or change the header default and rebuild)
to remove capture/logging. The same host test command above also accepts that
define and verifies the silent configuration.

On an error, collect every `[nor-uuid]` line. The summary contains the return
code, cached JEDEC ID, original SDK mode/command channel, transaction count and
timeout count. No UID/OTP bytes are printed. Each timeout records:

- `seq`: transaction number; normally 1=FF, 2=9F, 3=SFDP signature,
  4=first UID read, 5=second UID read. QPI restore/38 is the next transaction
  after success or failure, so its number depends on where a failure occurred.
- `phase`: `pre-busy` (before any command; seq/op=0), `rx-wait` (FIFO empty),
  or `complete-wait` (TX FIFO did not empty or BUSY stayed set after receiving
  all requested bytes; SSIENR need not clear automatically).
- `op`, `addr`, `rx=received/expected`, `SR`, `SSIENR`, `TXFLR`, `RXFLR`.
- `CTRLR0/1/2`, `ADDR_LENGTH`, `BAUDR`, `FBAUDR`, `AUTO_LENGTH`, `VALID_CMD`.

The polling limit is an iteration count, NOT microseconds. Snapshot registers
are read before disable/flush, without reading the data FIFO or clear-on-read
interrupt registers. Up to two snapshots preserve the first two timeouts, including cleanup errors. Snapshots are per-call RAM data, not a
shared global. Printing happens only after restoring controller registers and
restoring IRQ/cache state (argument errors print before taking the lock).
Restoration after a hardware timeout remains best-effort; these logs cannot
guarantee reporting if the flash can no longer support XIP.

Host tests check command-only TX idle with enable still set (the reported
SR=0x06 case), TX with undrained FIFO or busy shifter, initial-busy, FIFO wait,
completed RX with enable still set, full RX with BUSY or pending TX,
partial UID RX, dual failure during QPI recovery, snapshot-before-clear, and
logging-after-unlock/register-restoration, in addition to existing UID/OTP tests.

## Board validation still required

- Confirm JEDEC `1C 38 17`, successful SFDP framing and repeatable 12-byte UID.
- Verify the UID persists across reboot and differs on a second physical chip.
- Check flash/XIP/filesystem reads after success, in both SPI and QPI modes used
  by the product; compare SPIC configuration before/after the call.
- If the reader reports invalid data, capture CS/CLK/IO to verify the SFDP/UID
  command, address, dummy byte and sampling timing before deploying it.

On errors, `launch` lines capture the first eight transactions: `SER`,
`loaded_tx/loaded_en` after FIFO writes but before enable, and
`enabled_SR/enabled_tx/enabled_en` immediately after enable. These are RAM
snapshots printed after restoration/unlock, not printing in the transaction.
`loaded_tx=0` points to a command-loading problem; `SER=0` points to slave
selection. Nonzero loaded FIFO does not itself prove a wire transaction.
The boot cache runs before filesystem/customer init, `wlan_network()` and the
scheduler. Look for `[NORCACHE] ... api=cache-only`. Disabling boot diagnostics
does not disable early initialization. No later API performs a physical read.

Timing regression covers multiple original FBAUDR values, calibrated residual
boundaries, rejection of missing/inconsistent calibration metadata before any
command, and restoration of AUTO_LENGTH/BAUDR on every existing failure path.
Error `timing` lines show XIP protocol dummy clocks, the subtracted bus clocks,
the residual RX delay and temporary command baud divider. A derived delay is
not physical timing validation; verify JEDEC, SFDP and repeatable UID on board.

The cache tests exercise initial IRQ masked/unmasked states and all I/D-cache
enable combinations, independent UID and OTP failures, full-region offset reads,
invalid arguments, idempotent init and getters after making hardware inaccessible.
They assert no additional MMIO transactions or guard entries after initialization.
The older protocol vectors invoke private hardware readers directly so cache
idempotence cannot hide transport failures.
