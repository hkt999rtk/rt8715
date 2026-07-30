# CarBox Pro1 Migration Summary

Date: 2026-05-26

## Purpose

`carbox_pro1` is the Pro1 SDK migration baseline for the existing
`carbox_smart` product. The current delivery focuses on establishing a clean
Pro1 build baseline and moving the CarBox/CarPlay dependency investigation into
a controlled, traceable structure.

This package is not yet a runtime-complete CarPlay firmware release. It is a
build and migration baseline for the next integration phase.

## What Has Been Done

### Pro1 SDK Baseline

- Created `carbox_pro1` from Pro1 SDK `sdk-ameba-v5.2j`.
- Preserved the original Pro1 SDK layout, including:
  - `component/common`
  - `component/soc/realtek/8195b`
  - `project/realtek_amebapro_v0_example/GCC-RELEASE`
  - `tools/arm-none-eabi-gcc`
- Recorded the Pro1 SDK source revision:

```text
a3491e39395ec8ccc96d21a2353e40cb753e275f
```

### Build Baseline

- Added a Docker build flow for repeatable validation on macOS or other hosts.
- Added a native Linux build wrapper that explicitly uses the SDK-bundled
  Realtek cross compiler.
- Verified the Docker link-only Pro1 baseline build:
  - `application_lp/Debug/bin/application_lp.axf`
  - `application_is/Debug/bin/application_is.axf`

The Docker baseline intentionally uses `CARBOX_SKIP_IMAGE=1`, so it verifies
LP/IS compile and link output first. The customer native Linux wrapper defaults
to `CARBOX_SKIP_IMAGE=0`, so it runs the final SDK image manipulation stage.

### CarBox Product Hooks

- Added a minimal CarBox product entry structure on the Pro1 side.
- Kept Pro1 SDK startup flow intact.
- Did not overwrite Pro1 `main.c` with Smart SDK `main.c`.
- Added early migration hooks only where they can be controlled and verified by
  build.

### Smart CarPlay / AndroidAuto Libraries

- Staged Smart CarPlay/AndroidAuto reference libraries under:

```text
third_party/carbox_smart/carplay_app/
```

- These libraries are kept as reference artifacts and dependency-discovery
  inputs.
- They are not added to the normal Pro1 SDK firmware link path.
- Current finding: the Smart CarPlay libraries are built for the Smart AP side
  architecture, while the Pro1 firmware target is Cortex-M. Runtime compatibility
  is therefore not assumed.

### Link Dependency Investigation

- Added a standalone CarPlay link probe under `tools/carbox/`.
- The probe is separate from the normal Pro1 SDK build.
- The probe is used to identify missing symbols, duplicate symbols, and required
  Pro1-side wrappers without pulling Pro1 example applications into the CarPlay
  dependency check.
- Where Pro1 SDK providers exist, wrappers were added under:

```text
tools/carbox/smart_wrapper/
```

- Approved link-only placeholders are isolated under:

```text
tools/carbox/experimental_stubs/
```

These placeholders are not production implementations. They mark API gaps that
must be reviewed before runtime firmware validation.

## Current Build Commands

### Customer Native Linux Build

Use this command on a native Linux host:

```bash
carbox_pro1/tools/carbox/build_baseline_host_linux.sh
```

This wrapper uses the SDK-bundled compiler:

```text
carbox_pro1/tools/arm-none-eabi-gcc/asdk/linux/newlib/bin/arm-none-eabi-
```

Do not override `CROSS_COMPILE` unless intentionally testing a different toolchain.

The native Linux wrapper runs the full SDK build by default, including
`elf2bin.linux`:

```text
CARBOX_SKIP_IMAGE=0
```

For engineering diagnostics only, link-only mode can be forced with:

```bash
CARBOX_SKIP_IMAGE=1 carbox_pro1/tools/carbox/build_baseline_host_linux.sh
```

### Docker Build

Use this command on macOS or when a reproducible Linux container is preferred:

```bash
docker build -t carbox-pro1-build carbox_pro1/docker
docker run --rm -v "$PWD":/work carbox-pro1-build
```

Expected result:

```text
carbox_pro1 baseline build passed
```

Expected artifacts:

```text
carbox_pro1/project/realtek_amebapro_v0_example/GCC-RELEASE/application_lp/Debug/bin/application_lp.axf
carbox_pro1/project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/Debug/bin/application_is.axf
```

## Current Known Limitations

- Docker/macOS validation is link-only and does not accept full image
  packaging. Full image packaging must be validated on the customer native
  Linux environment through `build_baseline_host_linux.sh`.
- The Smart CarPlay/AndroidAuto libraries are staged for analysis, but they are
  not treated as production-compatible Pro1 runtime libraries.
- CarPlay runtime behavior, USB role/class behavior, Wi-Fi behavior, OTA image
  behavior, factory/MP behavior, and Bluetooth behavior still require hardware
  validation on Pro1.
- Some compatibility APIs are currently link-only placeholders. These are
  isolated and documented so they can be replaced with real Pro1
  implementations as requirements are confirmed.

## Customer Next Steps

1. Run the native Linux baseline build:

```bash
carbox_pro1/tools/carbox/build_baseline_host_linux.sh
```

2. Confirm both `.axf` files are generated:

```bash
test -f carbox_pro1/project/realtek_amebapro_v0_example/GCC-RELEASE/application_lp/Debug/bin/application_lp.axf
test -f carbox_pro1/project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/Debug/bin/application_is.axf
```

3. Confirm the native Linux wrapper completes the full image build and reports:

```text
carbox_pro1 Linux host full image build passed
```

4. Confirm the Pro1 hardware requirements for:
   - USB device/host role and CarPlay/NCM expectations
   - Wi-Fi AP/station behavior
   - Bluetooth profile requirements
   - OTA image layout and boot-slot behavior
   - Factory/MP command flow

5. Provide one of the following for CarPlay/AndroidAuto runtime integration:
   - Pro1/Cortex-M compatible CarPlay/AndroidAuto libraries, or
   - source code and build settings that can rebuild the required libraries for
     the Pro1 target.

## Recommended Packaging

Recommended customer-facing files:

- `CUSTOMER_MIGRATION_SUMMARY.md`
- `MIGRATION_BASELINE.md`
- `MIGRATION_PHASE2_INVENTORY.md`
- `tools/carbox/build_baseline_host_linux.sh`
- `docker/`

Internal engineering files that do not need to be included in a customer status
package unless detailed debug traceability is requested:

- `MIGRATION_CHANGELOG.md`
- `third_party/carbox_smart/reports/`
- `build/carbox/`

## Status

Current status: Pro1 SDK baseline build is established. CarBox/CarPlay
dependency migration is in progress. The next milestone is to validate full
image packaging on native Linux and replace remaining link-only compatibility
placeholders with confirmed Pro1 implementations or customer-provided runtime
libraries.
