#!/bin/sh
set -eu
cd "$(dirname "$0")"
TEST_DIR=$(pwd)
LWIP_DIR=$(cd ../.. && pwd)
TEST_BIN=$(mktemp /tmp/lwip-recovery.XXXXXX)
trap 'rm -f "$TEST_BIN"' EXIT
cc -std=c99 -g -O1 -Wall -Wextra -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
  ${TEST_CFLAGS:-} -I"$TEST_DIR" -I"$LWIP_DIR/src/include" \
  test_recovery.c "$LWIP_DIR"/src/core/*.c "$LWIP_DIR"/src/core/ipv4/*.c \
  -o "$TEST_BIN"
if [ "$#" -eq 0 ]; then
  set -- limited partial rto busy many_partial queued_busy window_reorder partial_busy wrap handshake_idle boundary moving_clock output_failure karn timer_cadence zero_window partial_dupack
fi
for scenario in "$@"; do "$TEST_BIN" "$scenario"; done
