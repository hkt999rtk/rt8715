# wcam TX Sources Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task by task.

**Goal:** Produce distinct color bar and GC2053 TX images while retaining one RX and one unitest image.

**Architecture:** Compile the TX task with a source selector. The sensor source exchanges aligned NV12 buffers with the SDK ISP callback; the color bar source remains as is. Build packaging chooses the matching SDK ISP firmware per role.

**Tech Stack:** AmebaPro C/FreeRTOS, Realtek ISP/H.264 APIs, RTP/RTCP, Bash build wrapper.

**Spec:** `docs/superpowers/specs/2026-11-14-wcam-tx-sources-design.md`

## Global Constraints

- Build LP with `make -j1 -B ram_lp`, then IS with `make -j8 ram_is` after clean, using the external Realtek toolchain.
- GC2053 output has no `TX` label or counter.
- Keep RX and unitest behavior and firmware contents unchanged.

## Review Focus

- Sensor ID mismatch must stop the TX stream and print the observed ID.
- ISP must never overwrite a frame owned by the encoder.
- Wi-Fi reconnect must start with an IDR.
- GC2053 image must package `isp_gc2053.bin` and other images must package `isp.bin`.
- Build failures must not publish mixed image versions.

## Tasks

### Task 1: Role and image verification

- [x] Extend `wcam/tests/verify_elf.py` for `tx_gc2053` and observe a failure against the old color bar TX ELF.
- [x] Add build role, variant names, and role-specific ISP JSON selection in `application.is.mk` and `wcam/build-pair.sh`.
- [x] Verify the final flash image contains exactly one copy of the GC2053 ISP firmware.

### Task 2: Sensor source and TX integration

- [x] Add `wcam_sensor.c/h` to initialize the GC2053 ISP and exchange DMA buffers.
- [x] Select sensor frames in `wcam_tx.c` for the sensor role and leave color bar behavior intact.
- [x] Build sensor TX and check the linked image has ISP capture but no drawing path.

### Task 3: Publish and verify

- [x] Run portable tests and a clean build for unitest, TX color bar, TX GC2053, and RX.
- [x] Verify each SHA-256 and document full paths, roles, and hardware checks.
