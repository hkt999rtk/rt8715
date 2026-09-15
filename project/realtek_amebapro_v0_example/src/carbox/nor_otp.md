# EN25S64A user OTP reader

```c
#include "nor_otp.h"

uint8_t otp[16];
int result = carplay_nor_read_otp(0, otp, sizeof(otp));
if (result == (int)sizeof(otp)) {
    /* Successfully read user OTP bytes 0..15. */
} else {
    /* A negative CARPLAY_NOR_OTP_* error; otp is unchanged. */
}
```

`offset` is relative to OTP, not a physical address. Valid nonempty reads are
contained in offsets 0..511. Length can be 1..512 subject to that boundary.
Zero length at offset <= 512 returns 0 without hardware access. All-zero and
all-FF contents are valid user data; this reader does not assert that OTP has
been programmed or permanently locked.

This is **not** the 12-byte factory UID and not the SoC eFuse. It also does not
automatically replace `nor_read_otp()`, `spinor_read_otp()`, or
`CarApi_GetFlashOTP()` inside the customer's archive. Call this API explicitly;
no archive edits, startup calls, or additional logging were introduced.

## Implementation

The implementation shares the internal SRAM transport in `nor_uuid.c` with the
UID reader. EN25S64A Rev.H (2020-10-22), p.62 specifies this read-only sequence:

1. Acquire the SDK flash resource lock and save the touched SPIC registers.
2. Switch from QPI/enhanced read to SPI, if applicable; use conservative BAUDR
   divider 10 for command/READ traffic while preserving fast/XIP calibration.
3. Reject busy/write-enabled flash (WIP or WEL set), and verify JEDEC `1C 38 17`.
4. Send `3A` to enter OTP.
5. Send `03` + 3-byte address `0x7FF000 + offset`, without a dummy byte. Read
   chunks of at most 16 bytes into a local staging buffer, bounded to `0x7FF1FF`.
6. Send `04` (WRDI) to leave OTP, including after entry/read failure; restore
   QPI if necessary, restore controller registers and release the lock.

The caller's buffer is updated only when the read and cleanup both succeed.
There is no write-enable, status-register write, program, erase or OTP-lock
operation. All polling is bounded. If hardware times out, cleanup is attempted
but restoration of a faulty device and continued XIP cannot be guaranteed.

Use from a normal initialized application task, with a writable RAM buffer;
not from an ISR, with interrupts disabled, or under an existing flash lock.
The current ARM build reports a 608-byte stack frame for this API, plus the
transport/SDK call stack. Allow adequate task stack headroom.

## Validation

The host protocol/control-flow harness in `tests/nor_uuid/test.cpp` also tests
this reader. Run using the command in `tests/nor_uuid/README.md`. It covers full
512-byte reads, nonaligned offsets/chunks, boundary and overflow rejection,
all-zero/all-FF data, incorrect IDs, busy/write-enabled flash, a timeout at each
transaction, partial-read timeout, cleanup ordering, register restoration and
unchanged output on failure. UID regression tests use the same transport.

These tests are not a hardware FIFO/clock emulator. On-board testing remains
required: verify known OTP contents, compare normal flash/filesystem/XIP reads
before and after OTP access, and check both SPI and QPI configurations. Merely
reading all FF does not prove OTP mode was entered on the physical device.
