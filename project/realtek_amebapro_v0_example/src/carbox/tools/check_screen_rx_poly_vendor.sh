#!/bin/sh
# Offset payload pointers require the audited allocation/free/callback ABI.
set -eu
if [ "$#" -ne 3 ]; then
    echo "usage: $0 AR CARPLAY_ARCHIVE ACCESSORY2_ARCHIVE" >&2
    exit 2
fi
ar_tool=$1
carplay=$2
accessory=$3
check_member() {
    archive=$1
    member=$2
    expected=$3
    actual=$("$ar_tool" p "$archive" "$member" | sha256sum)
    actual=${actual%% *}
    if [ "$actual" != "$expected" ]; then
        echo "SCREEN_RX_POLY_INPLACE: $member changed; re-audit ownership before enabling" >&2
        exit 1
    fi
}
check_member "$carplay" AirPlayReceiverSessionScreen.o 9b051d06928cfb1c7e861ebfca52fd4e902cf0542353c64430c88cf6db1d2126
check_member "$carplay" ScreenUtils.o 28cac6f8e9861922b8d879ee00f439b86ace2338cc91bd4b88e5b7dc6366ba98
check_member "$carplay" AppleCarPlay_AppStub.o beab94a28bcda7b8ad9303086cc38e8c3d74f48fe1b1d23ba5b8d2c27ac00650
check_member "$accessory" AirPlayScreen.o 8c1a6d380cd90ab4630ad1c77d232e92eafdd2e0a4b3db0c46de472f3fecd400
echo "SCREEN_RX_POLY_INPLACE: audited vendor objects match"
