#!/usr/bin/env bash
set -euo pipefail
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build="$repo/project/realtek_amebapro_v0_example/GCC-RELEASE"
prefix=/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/asdk/linux/newlib/bin/arm-none-eabi-
role_arg=${1:-all}
case "$role_arg" in
    all) roles=(unitest tx_colorbar tx_gc2053 rx);;
    both) roles=(tx_colorbar tx_gc2053 rx);;
    tx) roles=(tx_colorbar);;
    unitest|tx_colorbar|tx_gc2053|rx) roles=("$role_arg");;
    *) echo 'usage: build-pair.sh [all|both|unitest|tx|tx_colorbar|tx_gc2053|rx]' >&2; exit 2;;
esac
if ! python3 -c 'from pyfatfs.PyFat import PyFat; import littlefs' 2>/dev/null; then
    export PYTHONPATH="/tmp/rt8715-python-deps${PYTHONPATH:+:$PYTHONPATH}"
fi
python3 -c 'from pyfatfs.PyFat import PyFat; import littlefs'
mkdir -p "$repo/wcam/build-logs"
# Serialize all builds using this wrapper; the SDK shares objects across roles.
exec 9>"$build/.wcam-build.lock"
flock -n 9 || { echo 'Another wcam build is active' >&2; exit 1; }
stage=$(mktemp -d "$repo/wcam/.pair-build.XXXXXX")
sensor_json="$build/amebapro_firmware_is_gc2053.generated.json"
trap 'rm -rf -- "$stage"; rm -f -- "$sensor_json"' EXIT
cd "$build"
for role in "${roles[@]}"; do
    build_role=$role
    firmware_json=amebapro_firmware_is.json
    if [[ $role == unitest ]]; then
        build_role=local
    elif [[ $role == tx_colorbar ]]; then
        build_role=tx
    elif [[ $role == tx_gc2053 ]]; then
        firmware_json=$(basename "$sensor_json")
    fi
    echo "[wcam build] clean / LP / IS: $role"
    make clean WCAM_ROLE="$build_role" CROSS_COMPILE="$prefix" > "$repo/wcam/build-logs/$role-clean.log" 2>&1
    make -j1 -B ram_lp WCAM_ROLE="$build_role" CROSS_COMPILE="$prefix" > "$repo/wcam/build-logs/$role-lp.log" 2>&1
    if [[ $role == tx_gc2053 ]]; then
        python3 - "$sensor_json" <<'PY'
import json
import sys

with open('amebapro_firmware_is.json') as f:
    config = json.load(f)
config['ISP']['sram']['file'] = '../../../component/soc/realtek/8195b/misc/bsp/image/isp_gc2053.bin'
config['FIRMWARE']['images'].insert(0, {'img': 'ISP', 'offset': '0x00'})
with open(sys.argv[1], 'w') as f:
    json.dump(config, f)
assert config['ISP']['sram']['file'].endswith('/isp_gc2053.bin')
assert any(image['img'] == 'ISP' for image in config['FIRMWARE']['images'])
PY
        echo '[wcam build] GC2053 ISP firmware: isp_gc2053.bin'
    fi
    touch "$stage/$role-start"
    make -j8 ram_is WCAM_ROLE="$build_role" WCAM_FIRMWARE_JSON="$firmware_json" CROSS_COMPILE="$prefix" > "$repo/wcam/build-logs/$role-is.log" 2>&1
    test application_is/flash_is.bin -nt "$stage/$role-start"
    python3 "$repo/wcam/tests/verify_isp_image.py" application_is/flash_is.bin "$build_role"
    python3 "$repo/wcam/tests/verify_elf.py" application_is/Debug/bin/application_is.axf "$build_role"
    cp application_is/flash_is.bin "$stage/flash_wcam_$role.bin"
    cp application_is/Debug/bin/application_is.axf "$stage/wcam_$role.axf"
    cp application_is/Debug/bin/application_is.map "$stage/wcam_$role.map"
done
# Publish only once all requested roles have successfully built and verified.
for role in "${roles[@]}"; do
    cp "$stage/flash_wcam_$role.bin" "$repo/wcam/flash_wcam_$role.bin"
    cp "$stage/wcam_$role.axf" "$repo/wcam/build-logs/wcam_$role.axf"
    cp "$stage/wcam_$role.map" "$repo/wcam/build-logs/wcam_$role.map"
    (cd "$repo/wcam" && sha256sum "flash_wcam_$role.bin" > "flash_wcam_$role.bin.sha256")
    cat "$repo/wcam/flash_wcam_$role.bin.sha256"
    if [[ $role == tx_colorbar ]]; then
        cp "$repo/wcam/flash_wcam_tx_colorbar.bin" "$repo/wcam/flash_wcam_tx.bin"
        (cd "$repo/wcam" && sha256sum flash_wcam_tx.bin > flash_wcam_tx.bin.sha256)
    fi
done
