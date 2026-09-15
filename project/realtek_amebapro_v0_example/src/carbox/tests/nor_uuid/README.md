# EN25S64A UID and OTP readers

The same harness now covers `carplay_nor_read_otp()`; see `../../nor_otp.md`
for its interface, read-only command sequence, and additional test coverage.

`../../nor_uuid.h` exposes `carbox_nor_read_uuid(buffer, capacity)`. It returns
12 bytes in increasing UID address order, or a negative error code defined in
the header. This is a 96-bit factory UID, not an RFC 128-bit UUID. There is no
automatic boot call, logging, allocation, flash programming or OTP modification.

Example, from a normal task after flash initialization:

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
QPI is temporarily exited with FF and restored with 38. Touched SPIC registers
are saved/restored, and the SDK resource lock surrounds the operation. Existing
BAUDR and PHY timing are retained; fast-read/XIP settings are not recalibrated.
The entry points and shared transport are linked into SRAM, including when
no caller exists yet.

The API is not ISR-safe and must not be called with the flash lock already
held. The output must be writable RAM. Polling is bounded; following a hardware
timeout, mode restoration is best-effort, not a guarantee that a faulty flash
or controller can resume XIP. Do not treat timeout as valid identity data.

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

## Board validation still required

- Confirm JEDEC `1C 38 17`, successful SFDP framing and repeatable 12-byte UID.
- Verify the UID persists across reboot and differs on a second physical chip.
- Check flash/XIP/filesystem reads after success, in both SPI and QPI modes used
  by the product; compare SPIC configuration before/after the call.
- If the reader reports invalid data, capture CS/CLK/IO to verify the SFDP/UID
  command, address, dummy byte and sampling timing before deploying it.
