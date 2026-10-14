# Repository build rule

- Do not use parallel `make all` in `project/realtek_amebapro_v0_example/GCC-RELEASE`. Its LP object-copy/link and IS image steps race.
- Use the external Realtek toolchain prefix `/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-`.
- For a clean firmware build, run sequentially:
  1. `make clean CROSS_COMPILE=<prefix>`
  2. `make -j1 -B ram_lp CROSS_COMPILE=<prefix>`
  3. `make -j8 ram_is CROSS_COMPILE=<prefix>`
- `-B` for `ram_lp` prevents an interrupted build from leaving a source-tree object newer while its required copy under `application_lp/Debug/obj` is missing.
- The flash image is `project/realtek_amebapro_v0_example/GCC-RELEASE/application_is/flash_is.bin`.

## Python tools used while packing the IS image

- `gnu_utility/vfs.py` needs the Python packages `pyfatfs` and `littlefs-python`. Check them before a rebuild with `python3 -c 'from pyfatfs.PyFat import PyFat; import littlefs'`.
- If either is missing, `ram_is` can compile and link successfully, then stop at `Image manipulating` with `make ... Error 1`. The make recipe redirects `vfs.py` output to `/dev/null`, so run `python3 gnu_utility/vfs.py -t FATFS -s 512 -c 2 -dir rootfs -out /tmp/rt8715-fatfs-check.bin` from `GCC-RELEASE` to see the actual import error.
- For a temporary, unprivileged fix: `python3 -m pip install --target /tmp/rt8715-python-deps pyfatfs littlefs-python`, then run the build with `PYTHONPATH=/tmp/rt8715-python-deps`. Do not treat an older `flash_is.bin` as the output of a failed build; verify its timestamp and SHA-256 after a successful `ram_is`.

## Patching customer ELF archive members

- Do not use `objcopy --update-section` on an ELF function section that has relocations: it removes that section's relocation records. The resulting image can link while its `BL` instructions still point to themselves. For a fixed-size instruction change, edit the validated section bytes in the ELF object directly, then compare `objdump -r` before and after and inspect the final linked call targets. See `src/carbox/tools/patch_carplay_ui565_archive.py`.
