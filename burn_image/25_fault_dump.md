# Fatal exception dump

The IS application installs `src/carbox/fault_dump.c` immediately after log
UART initialization. It replaces the active secure RAM vector entries for
HardFault, MemManage, BusFault and UsageFault. It leaves configurable fault
enable bits, trap settings and watchdog policy unchanged. A disabled fault
that escalates to HardFault is decoded from CFSR there.

Boot confirmation:

```text
[FAULT] dump install status=0 (0=enabled)
```

Negative status means installation failed (RAM VTOR or UART unavailable).
Faults before this initialization, LP-core faults and SecureFault are outside
this handler's coverage.

On a fatal exception the handler captures R4-R11, EXC_RETURN, MSP/PSP, stack
limits and interrupt masks before any C prologue, then switches to a dedicated
1 KiB SRAM emergency stack. It prints directly to the initialized UART with
bounded polling, without printf, allocation, locks or RTOS list traversal.
Code and literals also reside in SRAM; the object uses the soft float ABI to
avoid generating FPU instructions and triggering lazy FP stacking.

Output begins with `[FAULT] BEGIN build=... type=...` and includes:

- CFSR/UFSR, HFSR, SHCSR, ICSR, DFSR, AFSR and decoded CFSR causes.
- MMFAR/BFAR only when their valid bits are set.
- Entry R4-R11, MSP/PSP, limits, EXC_RETURN, CONTROL and interrupt masks.
- Hardware-stacked R0-R3, R12, LR, PC and xPSR when the frame is readable.
- Reconstructed pre-exception SP, including FP frame size and alignment padding.
- Up to 256 bytes of raw stack words, bounded to this LPDDR IS image's RAM map.
- Current TCB address, without dereferencing potentially damaged task metadata.

Basic and floating-point extended frames have their core words at the same
offset: PC is word 6. FP words follow the core frame; the handler does not
interpret lazy FP contents. See the
[Armv8-M Exception Model User Guide](https://documentation-service.arm.com/static/64c7832738511951cb7a246e).
Stacking/unstacking/lazy-state errors, STKOF, invalid pointers/limits and
additional-state frames (`DCRS=0`) cause frame/stack reading to be skipped;
the status registers and entry capture are still printed. The raw stack is
not an unwound backtrace. Range checks do not guarantee that damaged or
MPU-protected memory can actually be read; a second fault or failed UART can
still truncate output. The handler halts after dumping; a watchdog can reset it.

## Decode addresses

Keep the **matching** `application_is/Debug/bin/application_is.axf` with each
tested `flash_is.bin`; rebuilding can change all addresses. From GCC-RELEASE:

```sh
/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-addr2line \
  -e application_is/Debug/bin/application_is.axf -a -f -C -i \
  0xPC_ADDRESS 0xLR_ADDRESS
```

Replace the placeholders with the dump values. For a Thumb return address,
clear bit 0 before resolving it; inspect disassembly around the return address
to identify the calling instruction. Do not blindly subtract a fixed instruction
size. Vendor library code without debug information may resolve to a function
name only. The dump is a UART diagnostic snapshot, not a complete GDB core file.

## Validation

Host frame/range validation (basic, FP, nonsecure caller, stacking errors,
invalid EXC_RETURN, alignment, limits and RAM boundaries):

```sh
python3 project/realtek_amebapro_v0_example/src/carbox/tests/fault_dump/test_frame.py
```

Build and inspect the linked ELF to ensure `fault_entry`, `fault_dump_run`,
`fault_capture` and `fault_stack` remain in SRAM. Actual UART output and fault
entry need board validation. To trigger a deliberate test in a disposable test
build, insert a temporary call site containing `__asm volatile("udf #0")`
after initialization. The firmware will halt/reset;
no deliberate fault trigger is enabled in normal builds.
