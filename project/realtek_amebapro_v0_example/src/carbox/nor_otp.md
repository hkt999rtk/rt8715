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

`main()` now calls `carbox_nor_identity_cache_init()` immediately after UART
and fault-dump setup, before filesystem/customer initialization, WLAN and the
scheduler. It reads the 12-byte UID and **all 512 OTP bytes** once into static
SRAM caches. The existing public read APIs only copy from these caches, with
unchanged names, arguments, bounds checks and return conventions. No customer
call issues flash commands, takes a flash lock, or retries a failed boot read.
UUID and OTP validity/error status are independent. Before initialization the
APIs return NOT_READY. Failure leaves the caller's buffer unchanged. Repeated
cache initialization is a no-op, including after failure; retry occurs next boot.

`offset` is relative to OTP, not a physical address. Valid nonempty reads are
contained in offsets 0..511. Length can be 1..512 subject to that boundary.
Zero length at offset <= 512 returns 0 without hardware access. All-zero and
all-FF contents are valid user data; this reader does not assert that OTP has
been programmed or permanently locked.

This is **not** the 12-byte factory UID and not the SoC eFuse.

The current customer archive discards errors in both `spinor_read_otp()` and
`CarApi_GetFlashOTP()`. `application.is.mk` now uses `--wrap` for both symbols;
the replacement implementations in `nor_uuid.c` read 16 bytes from this cache,
return **1 on success**, and propagate the negative error on failure without
modifying the output buffer. Customer API names/arguments stay unchanged and
the vendor archive is not edited. The replacement `CarApi_GetFlashOTP()` also
avoids the old unconditional dump of the buffer after a failed read.

Callers must test `ret == 1`, not `if (ret)` (negative errors are also nonzero):

```c
int ret = spinor_read_otp(offset, buffer); /* or CarApi_GetFlashOTP */
if (ret == 1) {
    /* Use the 16 valid bytes. */
} else {
    printf("OTP read failed: %d\n", ret);
}
```

GNU linker wrapping redirects undefined symbol references, not calls already
resolved within the defining object or inlined by the customer compiler.
Re-audit the call sites if the customer replaces the archive. In the current
archive `CarApi.o` calls `spinor_read_otp` via an external relocation in
`spinor.o`; no source call sites are present in this workspace.

## Implementation

The implementation shares the internal SRAM transport in `nor_uuid.c` with the
UID reader. EN25S64A Rev.H (2020-10-22), p.62 specifies this read-only sequence:

1. In early boot only, save PRIMASK/cache enable state, mask interrupts, clean
   and disable enabled data cache, disable enabled instruction cache, and save
   the touched SPIC registers. This avoids RTOS tick compensation and does not
   start peripherals while flash temporarily leaves XIP mode.
2. Derive the calibrated sampling delay by subtracting the active XIP command's
   protocol dummy clocks (adaptor metadata times FBAUDR times 2) from
   AUTO_LENGTH.rd_dummy_length. Reject inconsistent metadata before commands.
   Use that residual delay and BAUDR divider 10 for UUID/OTP commands, then
   switch from QPI/enhanced read to SPI, if applicable. Restore AUTO_LENGTH and
   BAUDR with the other saved registers before returning to normal traffic.
3. Reject busy/write-enabled flash (WIP or WEL set), and verify JEDEC `1C 38 17`.
4. Send `3A` to enter OTP.
5. Send `03` + 3-byte address `0x7FF000 + offset`, without a dummy byte. Read
   chunks of at most 16 bytes into a local staging buffer, bounded to `0x7FF1FF`.
6. Send `04` (WRDI) to leave OTP, including after entry/read failure; restore
   QPI if necessary, restore controller registers and the saved cache/IRQ state.

The caller's buffer is updated only when the read and cleanup both succeed.
There is no write-enable, status-register write, program, erase or OTP-lock
operation. All polling is bounded. If hardware times out, cleanup is attempted
but restoration of a faulty device and continued XIP cannot be guaranteed.

The private hardware readers run only during early initialization. They use a
512-byte OTP staging buffer and diagnostic snapshots on the boot stack. Runtime
public getters use only the immutable SRAM cache and copy to writable RAM.
Any external OTP provisioning after initialization becomes visible next boot.

## Validation

The host protocol/control-flow harness in `tests/nor_uuid/test.cpp` also tests
this reader. Run using the command in `tests/nor_uuid/README.md`. It covers full
512-byte reads, nonaligned offsets/chunks, boundary and overflow rejection,
all-zero/all-FF data, incorrect IDs, busy/write-enabled flash, a timeout at each
transaction, partial-read timeout, cleanup ordering, register restoration and
unchanged output on failure. UID regression tests use the same transport.
Cache tests also cover pre-init errors, idempotence, full-region offset reads,
independent UID/OTP failure, unchanged outputs and zero hardware transactions
on subsequent getters after the hardware becomes unavailable. Every initial
PRIMASK and I/D-cache enable combination is checked for restoration.

These tests are not a hardware FIFO/clock emulator. On-board testing remains
required: verify known OTP contents, compare normal flash/filesystem/XIP reads
before and after OTP access, and check both SPI and QPI configurations. Merely
reading all FF does not prove OTP mode was entered on the physical device.

## Startup diagnostics (2026-09-17)

`CARBOX_NOR_BOOT_DIAG=1` (default) controls summary logging, **not cache
initialization**. Turning it off still populates both caches in early `main()`.
The success summary is:

```text
[NORBOOT] early cache begin build=... scheduler=not-started
[NORCACHE] source=boot uuid_ret=12 uuid_valid=1 otp_ret=512 otp_valid=1 bytes=12/512 api=cache-only
[NORBOOT] cache complete; customer APIs do not access flash
```

On failure, the corresponding `*_valid=0` and saved negative `*_ret` identify
which cache is unavailable. `cache complete` means the attempts have finished,
not that both succeeded. UID success includes the existing JEDEC/SFDP/repeated
UID checks. OTP success covers the full region and successful mode restoration.
All-FF/all-zero flags cover 512 bytes; OTP contents are not printed. No repeated
physical OTP read is made by the summary. Cache logic does not resolve the
underlying SPIC timing problem: verify actual boot return codes on the board.

`CARBOX_NOR_UUID_DIAG=1` now controls error diagnostics for BOTH readers.
`[nor-uuid]` / `[nor-otp]` include stage, request offset/length, original metadata
and register configuration, observed JEDEC/SFDP/status with validity flags,
operation result before cleanup and cleanup result. Timeout snapshots retain
at most the first two timeouts, including best-effort OTP/QPI exit. All logging
occurs after restoration and unlock. A `cleanup_ret=0` means no cleanup command
reported a timeout; it is not independent verification of restored flash mode.
`mismatch_index=4294967295` means no UID mismatch index was recorded.
Set both diagnostic macros to 0 to remove the boot probe and API error logs.

### Winbond comparison

The SDK `flash_api.c:flash_read_unique_id()` documents Winbond-only SPI `4B`,
8-byte UID, initializes an adaptor and locks around the HAL call. The HAL
implementation delegates to a ROM stub, whose source body is not in this tree.
The EN25S64A reader instead uses `5A` + address `000080` + one dummy byte for
12-byte UID, and `3A` / `03` / `04` for user OTP. Commands are chip-specific.
The EON readers preserve the boot adaptor rather than reinitializing flash.
The initial logging change preserved clock behavior (UUID retained BAUDR and
OTP used divider 10). The later RX timing fix below uses divider 10 for both.
Compare entry and timeout timing snapshots when investigating failures.

The local reference is `/home/kevin/EN25S64A-104HIP.PDF`; the implementation's
page references are to EN25S64A Rev.H. Host tests exercise logging enabled and
disabled, mismatched JEDEC/SFDP, partial reads and independent cleanup failures.
Real board results are still required to diagnose the reported failure.

### Command-only completion fix (2026-09-17)

The reported FF and QPI-restore/38 timeouts both show SR=0x06, TXFLR=0 and
SSIENR=1: the FIFO is empty and BUSY is clear, but the old helper waits for
enable to clear as well. Command-only TX now waits for TFE=1 and BUSY=0,
then explicitly disables SPIC. An enable-write barrier precedes polling.
RX retains the expected-byte count and enable/BUSY completion checks.
This applies to UUID and OTP mode commands; opcode sequences, clock settings,
locking, SRAM placement and configuration restoration remain unchanged.
Host regression reproduces the old failure with persistent TX enable and
checks that undrained FIFO and busy-shifter conditions still time out.
This is a board-test fix: FIFO/idle status does not prove flash command
acceptance, so JEDEC/SFDP/duplicate UID checks and real-board validation remain
required. The target SDK's wait-ready implementation is a ROM stub.

The 20260917112356_95AE board test still failed: UUID 9F received 0/3 bytes,
OTP 05 received 0/1, both with SR=0x06 and empty FIFOs. Thus the command-only
completion change is not a confirmed fix; cleanup_ret=0 also does not establish
that 38 was transmitted. New launch snapshots record FIFO occupancy before
enable and SER to distinguish loading/selection from receive failures.
The same run overlapped the boot probe with WLAN firmware download and reported
HALMAC firmware checksum errors. Causality is not proven. With boot diagnostics
enabled, wlan_network() is now called only after the probe ends, to eliminate
that startup overlap; this does not serialize arbitrary runtime flash clients.

The subsequent 20260917114440_C211 board test, with the probe before WLAN,
received all JEDEC bytes (9F rx=3/3) and all status bytes (05 rx=1/1).
Both then timed out in complete-wait with SR=0x06 and SSIENR=1. Each launch
showed SER=1 and loaded_tx=1; 9F also showed BUSY immediately after enable.
This establishes that the RX completion wait wrongly requires enable to clear.
RX now receives all requested bytes, waits for TFE=1 and BUSY=0, and explicitly
disables SPIC, just like TX. Missing bytes, undrained TX and persistent BUSY
still time out. JEDEC/SFDP/status/UID validation follows only after transfer
success; full byte count alone does not prove valid flash contents.
The earlier no-RX failure's cause is not established by this test, since both
startup ordering and launch instrumentation changed between the builds.

### RX dummy timing correction

Board build 20260917115331_7CC0 completed transfers without timeout, but read
JEDEC C3 81 71 at BAUDR=2 and 38 70 2E at BAUDR=10, against expected 1C 38 17.
These match a four-bit shift into a repeating ID stream and a one-bit left
shift respectively. AUTO_LENGTH was 0x58030017 in both calls. This supports
excess receive delay as the next hypothesis; it is not board validation of
the correction.

SDK spic_calibration() computes the receive delay as flash protocol dummy
clocks * baud divider * 2 + calibrated path delay. The custom helper previously
inherited the whole XIP delay. It now subtracts the protocol component using
the active STR mode's dummy-cycle metadata and the original FBAUDR, preserving
the calibrated residual and DPHY configuration. Residuals outside the SDK's
[2, MAX_AUTO_LENGTH) search range are rejected before changing flash mode.
Both UUID and OTP use BAUDR=10. Commands 9F/05/03 have no protocol dummy, and
5A already sends its dummy byte explicitly, so no extra protocol delay is added.
All exits restore AUTO_LENGTH and BAUDR. Error logs include a `timing` line.
For example, if the boot adaptor specifies four XIP dummy clocks at FBAUDR=2,
the observed value 23 becomes 23 - 4*2*2 = 7 bus clocks of sampling delay.
JEDEC and SFDP checks remain mandatory; ID bytes are never corrected by shifts.
