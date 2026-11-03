#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=$(mktemp -d /tmp/wcam-test.XXXXXX)
trap 'rm -f "$out/test_pattern" "$out/test_control" "$out/test_pacer"; rmdir "$out"' EXIT
src="$repo/project/realtek_amebapro_v0_example/src/wcam"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -I"$src" "$repo/wcam/tests/test_pattern.c" "$src/wcam_pattern.c" \
    -o "$out/test_pattern"
"$out/test_pattern"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -I"$src" "$repo/wcam/tests/test_control.c" "$src/wcam_uvc_control.c" \
    -o "$out/test_control"
"$out/test_control"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -I"$src" "$repo/wcam/tests/test_pacer.c" -o "$out/test_pacer"
"$out/test_pacer"
