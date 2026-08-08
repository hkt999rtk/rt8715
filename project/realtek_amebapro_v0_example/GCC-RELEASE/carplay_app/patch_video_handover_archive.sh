#!/bin/sh
set -eu

if [ "$#" -ne 6 ]; then
	echo "usage: $0 MODE AR OBJCOPY INPUT OUTPUT MEMBER" >&2
	exit 2
fi

mode=$1
ar_tool=$2
objcopy_tool=$3
input_archive=$4
output_archive=$5
member=$6
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

ar_tool=$(cd "$(dirname "$ar_tool")" && pwd)/$(basename "$ar_tool")
objcopy_tool=$(cd "$(dirname "$objcopy_tool")" && pwd)/$(basename "$objcopy_tool")
input_archive=$(cd "$(dirname "$input_archive")" && pwd)/$(basename "$input_archive")
output_directory=$(cd "$(dirname "$output_archive")" && pwd)
output_archive=$output_directory/$(basename "$output_archive")

case "$mode" in
	accessory)
		malloc_hook=carbox_video_handover_destination_malloc
		free_hook=carbox_video_handover_consumer_free
		memcpy_option="--redefine-sym memcpy=carbox_airplay_screen_memcpy"
		;;
	receiver)
		malloc_hook=carbox_video_handover_source_malloc
		free_hook=carbox_video_handover_producer_free
		memcpy_option=
		;;
	*)
		echo "unknown video handover patch mode: $mode" >&2
		exit 2
		;;
esac

# Work only on a derived archive.  The Realtek/customer supplied input archive
# remains byte-for-byte untouched, so disabling the feature restores the exact
# legacy binary without relying on an inverse objcopy operation.
cp "$input_archive" "$output_archive"
(
	cd "$temporary"
	"$ar_tool" x "$input_archive" "$member"
	test -f "$member"
	"$objcopy_tool" \
		--redefine-sym "malloc=$malloc_hook" \
		--redefine-sym "free=$free_hook" \
		$memcpy_option \
		"$member" "patched.o"
	mv "patched.o" "$member"
	"$ar_tool" r "$output_archive" "$member"
)
"$ar_tool" s "$output_archive"
