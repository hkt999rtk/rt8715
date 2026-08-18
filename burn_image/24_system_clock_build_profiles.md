# System clock build profiles

The firmware has one public build selector for the CPU and PLL_SYS clock:

```text
SYSTEM_CLOCK_PROFILE=300|400
```

Do not select a release clock by overriding `SYS_PLL_OVERCLOCK`,
`SYS_PLL_TARGET_HZ`, `SYS_PLL_ISOLATED_*`, or `SYS_CLK_SWITCH_PROBE`. Those are
implementation or investigation controls. The Makefile rejects a contradictory
combination instead of producing an ambiguously labelled binary.

Run the commands below from:

```text
project/realtek_amebapro_v0_example/GCC-RELEASE
```

## 300 MHz profile

Build with:

```bash
make ram_is SYSTEM_CLOCK_PROFILE=300
```

This profile sets:

```text
SYS_PLL_OVERCLOCK=0
SYS_PLL_TARGET_HZ=300000000
```

The firmware retains the PLL_SYS and CPU `/1` configuration established by the
boot ROM. It does not execute the sustained overclock sequence. Expected
10-second profile output includes:

```text
[PCPROF][n][CLOCKBOOT] status=0 requested/runtime=300000000/300000000Hz
```

## 400 MHz profile

Build with:

```bash
make ram_is SYSTEM_CLOCK_PROFILE=400
```

This is the default profile and sets:

```text
SYS_PLL_OVERCLOCK=1
SYS_PLL_TARGET_HZ=400000000
```

Early boot selects the experimentally qualified PLL_SYS 400 MHz preset while
the CPU remains on the PLL_SYS `/1` path. Before returning to XIP execution,
the SPIC divider is changed from 2 to 4 and SPIC is calibrated at 50 MHz.
Software-I2C SCL pacing remains enabled to preserve its effective 300 MHz edge
timing.

Expected profile output includes values close to:

```text
[PCPROF][n][CLOCKBOOT] status=1 requested/runtime=400000000/400000000Hz ... spic ... actual=50000000Hz
[PCPROF][n][CLOCK] valid=1 measured=400000000Hz ...
```

The measured clock normally differs slightly from the nominal value. A result
inside the firmware's 2% verification tolerance is accepted.

## Clean rebuilds and binary identification

Changing `SYSTEM_CLOCK_PROFILE` changes the clock configuration stamp, so the
clock-sensitive objects are rebuilt even during an incremental build. For a
release candidate, use a clean build and record its hash:

```bash
make clean_is
make ram_is SYSTEM_CLOCK_PROFILE=400
sha256sum application_is/flash_is.bin
```

At the beginning of every build, the Makefile prints the resolved selection:

```text
Clock profile: 400 MHz (PLL overclock=1, target=400000000 Hz)
```

At runtime, use `[CLOCKBOOT]` and `[CLOCK]` as the authoritative confirmation
of the programmed and measured CPU clock. Do not infer the clock from the
filename or from an older binary's timestamp.

## Investigation-only controls

The following switches are disabled for normal 300/400 MHz release builds and
must not be used as clock selectors:

```text
SYS_PLL_ISOLATED_PROBE
SYS_PLL_ISOLATED_TARGET_HZ
SYS_PLL_ISOLATED_FREQ_SEL
SYS_PLL_ISOLATED_DIRECT
SYS_CLK_SWITCH_PROBE
ROM_CLOCK_DUMP
```

They characterize or dump clock behavior and may restore the original clock
before normal execution. In particular, an isolated probe target does not mean
the released firmware continues running at that target frequency.
