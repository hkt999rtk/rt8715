#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="$repo_root/project/realtek_amebapro_v0_example/GCC-RELEASE"
wrapper_src="$repo_root/project/realtek_amebapro_v0_example/src/carbox/usb_phy_driver_wrap.c"
out_dir="$repo_root/USB_PHY/boost_bins"
toolchain=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-
python_deps=${USB_BOOST_PYTHONPATH:-/tmp/rt8715-vfsdeps}

test -f "$build_dir/application_lp/Debug/bin/application_lp.axf"
mkdir -p "$out_dir"

names=(0dB 3p3dB 6p9dB 9p3dB)
for level in 0 1 2 3; do
	name=${names[$level]}
	log="$out_dir/build_rxboost_${name}.log"
	image="$out_dir/flash_is_rxboost_${name}.bin"

	# The level is a compiler argument.  Force just this source to rebuild
	# even when a previous variant already produced its object file.
	touch "$wrapper_src"
	if ! (cd "$build_dir" &&
		PYTHONPATH="$python_deps${PYTHONPATH:+:$PYTHONPATH}" \
		make -j8 ram_is CARBOX_USB_RX_BOOST_LEVEL="$level" \
		CROSS_COMPILE="$toolchain") >"$log" 2>&1; then
		tail -60 "$log" >&2
		exit 1
	fi
	if ! grep -aFq "[USB BOOST] candidate level=$level " \
		"$build_dir/application_is/Debug/bin/application_is.axf"; then
		printf 'Variant %s was not compiled into the ELF\n' "$level" >&2
		exit 1
	fi
	cp "$build_dir/application_is/flash_is.bin" "$image"
	printf '%s -> %s\n' "$level" "$image"
done

(cd "$out_dir" && sha256sum flash_is_rxboost_*.bin > SHA256SUMS)
if test "$(cut -d' ' -f1 "$out_dir/SHA256SUMS" | sort -u | wc -l)" -ne 4; then
	printf 'Expected four distinct flash images\n' >&2
	exit 1
fi

# Leave the ordinary build output in its default pass-through state.  Make
# does not track changes to compiler arguments as source dependencies.
touch "$wrapper_src"
if ! (cd "$build_dir" &&
	PYTHONPATH="$python_deps${PYTHONPATH:+:$PYTHONPATH}" \
	make -j8 ram_is CARBOX_USB_RX_BOOST_LEVEL=-1 \
	CROSS_COMPILE="$toolchain") >"$out_dir/build_default_restore.log" 2>&1; then
	tail -60 "$out_dir/build_default_restore.log" >&2
	exit 1
fi
if grep -aFq '[USB BOOST] candidate level=' \
	"$build_dir/application_is/Debug/bin/application_is.axf"; then
	printf 'Default pass-through ELF still contains an RX boost variant\n' >&2
	exit 1
fi
printf 'Four distinct images built; hashes: %s\n' "$out_dir/SHA256SUMS"
