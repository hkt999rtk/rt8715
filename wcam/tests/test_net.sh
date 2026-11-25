#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# The test translation unit supplies substitutes for the RTOS, radio and lwIP.
mkdir "$out/lwip"
for header in FreeRTOS.h task.h queue.h semphr.h wifi_conf.h osdep_service.h lwip_netconf.h lwip/sockets.h lwip/netifapi.h; do
    touch "$out/$header"
done
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter -DWCAM_ROLE_TX=1 \
   -I"$out" -I"$repo/project/realtek_amebapro_v0_example/src/wcam" \
   "$repo/wcam/tests/test_net.c" -o "$out/test_net"
"$out/test_net"
