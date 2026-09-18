# Repository build rule

- Do not use parallel `make all` in `project/realtek_amebapro_v0_example/GCC-RELEASE`. Its LP object-copy/link and IS image steps race.
- Use the external Realtek toolchain prefix `/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-`.
- For a clean firmware build, run sequentially:
  1. `make clean CROSS_COMPILE=<prefix>`
  2. `make -j1 -B ram_lp CROSS_COMPILE=<prefix>`
  3. `make -j8 ram_is CROSS_COMPILE=<prefix>`
- `-B` for `ram_lp` prevents an interrupted build from leaving a source-tree object newer while its required copy under `application_lp/Debug/obj` is missing.
- The flash image is `project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`.
