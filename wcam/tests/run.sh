#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=$(mktemp -d /tmp/wcam-test.XXXXXX)
trap 'rm -f "$out/test_sensor" "$out/test_pattern" "$out/test_control" "$out/test_pacer" "$out/test_stats" "$out/test_source" "$out/test_rtp" "$out/test_capture" "$out/capture.h264" "$out/pack.o" "$out/unpack.o" "$out/bitstream.o" "$out/packet.o"; rmdir "$out"' EXIT
src="$repo/project/realtek_amebapro_v0_example/src/wcam"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src" "$repo/wcam/tests/test_pattern.c" "$src/wcam_pattern.c" \
    -o "$out/test_pattern"
"$out/test_pattern"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src" "$repo/wcam/tests/test_control.c" "$src/wcam_uvc_control.c" \
    -o "$out/test_control"
"$out/test_control"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src" "$repo/wcam/tests/test_pacer.c" -o "$out/test_pacer"
"$out/test_pacer"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src" "$repo/wcam/tests/test_stats.c" "$src/wcam_stats.c" -o "$out/test_stats"
"$out/test_stats"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src" "$repo/wcam/tests/test_source.c" -o "$out/test_source"
"$out/test_source"

# The sensor SDK uses 32-bit DMA addresses. Non-PIE keeps the test buffers
# below 4 GiB while exercising the real GC2053 capture and teardown code.
cc -std=c99 -Wall -Wextra -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
    -no-pie -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$repo/wcam/tests/sensor_stubs" -I"$src" \
    -I"$repo/component/common/drivers/video/realtek/common" \
    "$repo/wcam/tests/test_sensor.c" -o "$out/test_sensor"
"$out/test_sensor"

sh "$repo/wcam/tests/test_net.sh"

vendor="$src/third_party/ireader"
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-variable -Wno-unused-parameter -DNDEBUG \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g -I"$src" -I"$vendor" -include "$src/wcam_rtp.h" \
    -c "$vendor/rtp-h264-pack.c" -o "$out/pack.o"
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-variable -Wno-unused-parameter -DNDEBUG \
    -fsanitize=address,undefined -fno-sanitize-recover=all -g -I"$src" -I"$vendor" -include "$src/wcam_rtp.h" \
    -c "$vendor/rtp-h264-unpack.c" -o "$out/unpack.o"
cc -std=c99 -Wall -Wextra -Werror -DNDEBUG -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src" -I"$vendor" -include "$src/wcam_rtp.h" \
    -c "$vendor/rtp-h264-bitstream.c" -o "$out/bitstream.o"
cc -std=c99 -Wall -Wextra -Werror -DNDEBUG -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src" -I"$vendor" -include "$src/wcam_rtp.h" \
    -c "$vendor/rtp-packet.c" -o "$out/packet.o"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src" "$repo/wcam/tests/test_rtp.c" "$src/wcam_rtp.c" "$out/pack.o" \
    "$out/unpack.o" "$out/bitstream.o" "$out/packet.o" -o "$out/test_rtp"
"$out/test_rtp"
cc -std=c99 -Wall -Wextra -Werror -Wno-misleading-indentation -fsanitize=address,undefined -fno-sanitize-recover=all -g \
    -I"$src" "$repo/wcam/tests/test_capture.c" "$src/wcam_rtp.c" "$out/pack.o" \
    "$out/unpack.o" "$out/bitstream.o" "$out/packet.o" -o "$out/test_capture"
"$out/test_capture" "$repo/wcam/evidence/board-capture-2026-11-03/wcam-debug.h264" "$out/capture.h264"
if command -v ffmpeg >/dev/null 2>&1; then
    ffmpeg -hide_banner -loglevel error -xerror -f h264 -i "$out/capture.h264" -f null -
fi
rm "$out/pack.o" "$out/unpack.o" "$out/bitstream.o" "$out/packet.o"
