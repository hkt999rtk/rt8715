# carbox_pro1 Migration Baseline

See `MIGRATION_CHANGELOG.md` for the full migration change history and the
current customer-facing status notes.

## Purpose

`carbox_pro1` is a full copy of `sdk-ameba-v5.2j` and is the Pro1 SDK baseline for the CarBox migration.

The first milestone is intentionally narrow: verify that the copied Pro1 SDK can build clean LP and IS images in a Linux container. No `carbox_smart` application logic or Smart SDK components are ported in this milestone.

## Source

- Source SDK: `sdk-ameba-v5.2j`
- Pro1 SDK source revision: `a3491e39395ec8ccc96d21a2353e40cb753e275f`
- Baseline project: `project/realtek_amebapro_v0_example/GCC-RELEASE`

## Build Environment

The supported baseline build environment is Linux through Docker. macOS native build is not part of this milestone because the vendor Makefiles and bundled tools target Linux, Cygwin, and MinGW.
The Docker image is pinned to `linux/amd64` because the bundled Realtek Linux GCC toolchain is x86_64.

For a customer native Linux host, use the SDK-bundled compiler rather than a
compiler from the host `PATH`:

```bash
carbox_pro1/tools/carbox/build_baseline_host_linux.sh
```

That wrapper explicitly sets:

```bash
CROSS_COMPILE=/path/to/carbox_pro1/tools/arm-none-eabi-gcc/asdk/linux/newlib/bin/arm-none-eabi-
```

This is the intended customer-side behavior. Do not pass a host
`CROSS_COMPILE` unless intentionally testing a different compiler version.

The native Linux wrapper defaults to the full SDK build path:

```bash
CARBOX_SKIP_IMAGE=0
```

This means the final vendor image manipulation stage, including
`elf2bin.linux`, is expected to run on the customer Linux machine. For
engineering link-only diagnostics, the same wrapper can still be overridden:

```bash
CARBOX_SKIP_IMAGE=1 carbox_pro1/tools/carbox/build_baseline_host_linux.sh
```

Build the container:

```bash
docker build -t carbox-pro1-build carbox_pro1/docker
```

Run the baseline link build:

```bash
docker run --rm -v "$PWD":/work carbox-pro1-build
```

The Docker wrapper defaults to `CARBOX_SKIP_IMAGE=1`, so it verifies LP/IS compile and link outputs without running the final image manipulation step.

Run the full SDK image build in Docker only when the vendor `elf2bin.linux`
environment is known-good:

```bash
docker run --rm -v "$PWD":/work -e CARBOX_SKIP_IMAGE=0 carbox-pro1-build
```

If a Pro1-compatible replacement for the vendor image tool is available, mount it into the container and override `ELF2BIN` through `CARBOX_ELF2BIN`:

```bash
docker run --rm -v "$PWD":/work -e CARBOX_ELF2BIN=/work/path/to/elf2bin carbox-pro1-build
```

Equivalent link-only command inside the container:

```bash
cd /work/carbox_pro1/project/realtek_amebapro_v0_example/GCC-RELEASE
make clean
make all CARBOX_SKIP_IMAGE=1
```

## Acceptance Criteria

- Native Linux customer build: `tools/carbox/build_baseline_host_linux.sh`
  exits with status 0 and runs the final image manipulation stage.
- Docker/macOS build: `make all CARBOX_SKIP_IMAGE=1` exits with status 0.
- `project/realtek_amebapro_v0_example/GCC-RELEASE/application_lp/Debug/bin/application_lp.axf` exists.
- `project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/Debug/bin/application_is.axf` exists.
- Full image manipulation must be accepted on native Linux, not from the current
  macOS/Docker emulation result.

## Current Verification Note

The link-only baseline has been verified on the current Apple Silicon host through Docker `linux/amd64` emulation:

```bash
docker build -t carbox-pro1-build carbox_pro1/docker
docker run --rm -v "$PWD":/work carbox-pro1-build
```

The wrapper completed with `carbox_pro1 baseline build passed` and generated:

- `project/realtek_amebapro_v0_example/GCC-RELEASE/application_lp/Debug/bin/application_lp.axf`
- `project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/Debug/bin/application_is.axf`

Full image generation remains unverified on this host. When `CARBOX_SKIP_IMAGE=0`, the vendor `elf2bin.linux` tool exits with a segmentation fault during:

```bash
elf2bin.linux convert amebapro_bootloader.json PARTITIONTABLE secure_bit=0
```

This failure is isolated to the final image manipulation step. Re-run the full image build on native x86_64 Linux before treating image generation as accepted.

The `elf2bin` binary found in `carbox_smart` was tested against the Pro1 `amebapro_bootloader.json` command. It returned success but did not generate `partition.bin`, so it is not treated as a compatible replacement for this Pro1 baseline.

## Next Phases

After the baseline is stable:

1. Inventory `carbox_smart` features from commit history, readme files, menuconfig, AT commands, OTA, USB, Bluetooth/Wi-Fi, and factory/MP flows.
2. Classify each feature as Pro1 SDK native, adapter required, Smart-only/not ported, or hardware-confirmation required.
3. Port product startup, logging, console, and required configuration first.
4. Port Wi-Fi, OTA, factory/MP, USB, Bluetooth, CarPlay, and Android Auto flows as separate verified slices.
