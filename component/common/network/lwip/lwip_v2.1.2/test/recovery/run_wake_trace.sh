#!/bin/sh
set -eu
cd "$(dirname "$0")"
bin=$(mktemp /tmp/lwip-wake.XXXXXX)
trap 'rm -f "$bin"' EXIT
cc -std=c99 -g -O1 -Wall -Wextra ${TEST_CFLAGS:-} \
  -DCONFIG_SCREEN_WAKE_TRACE=1 -I. -I../../src/include \
  test_wake_trace.c ../../src/core/tcp_wake_trace.c -o "$bin"
"$bin"
