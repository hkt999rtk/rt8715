#!/bin/sh
set -eu

if [ "$#" -ne 7 ]; then
	echo "usage: $0 MODE AR OBJCOPY INPUT OUTPUT MEMBER SCREEN_WAIT" >&2
	exit 2
fi

mode=$1
ar_tool=$2
objcopy_tool=$3
input_archive=$4
output_archive=$5
members=$6
screen_wait=$7
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
screen_wait_patcher=$script_directory/../../src/carbox/tools/patch_screen_wait_relocation.py
redundant_copy_patcher=$script_directory/../../src/carbox/tools/patch_airplay_redundant_copy.py
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
	accessory-wait)
		malloc_hook=
		free_hook=
		memcpy_option=
		;;
	receiver)
		malloc_hook=carbox_video_handover_source_malloc
		free_hook=carbox_video_handover_producer_free
		memcpy_option=
		;;
	private-memory)
		malloc_hook=
		free_hook=
		memcpy_option=
		;;
	*)
		echo "unknown video handover patch mode: $mode" >&2
		exit 2
		;;
esac

case "$screen_wait" in
	0|1) ;;
	*)
		echo "SCREEN_WAIT must be 0 or 1: $screen_wait" >&2
		exit 2
		;;
esac

# Work only on a derived archive.  The Realtek/customer supplied input archive
# remains byte-for-byte untouched, so disabling the feature restores the exact
# legacy binary without relying on an inverse objcopy operation.
cp "$input_archive" "$output_archive"
(
	cd "$temporary"
	for member in $members; do
		"$ar_tool" x "$input_archive" "$member"
		test -f "$member"
		if [ "$mode" = accessory-wait ]; then
			cp "$member" "patched.o"
		elif [ "$mode" = private-memory ]; then
			"$objcopy_tool" \
				--redefine-sym memcpy=carbox_vendor_chacha_memcpy \
				--redefine-sym memset=carbox_vendor_chacha_memset \
				"$member" "patched.o"
		else
			"$objcopy_tool" \
				--redefine-sym "malloc=$malloc_hook" \
				--redefine-sym "free=$free_hook" \
				$memcpy_option \
				"$member" "patched.o"
		fi
		if [ "$mode" = accessory ] && [ "$member" = AirPlayScreen.o ]; then
			# The customer normal-frame function copies the complete payload
			# twice.  Strictly remove only the validated second byte loop from
			# the derived archive; a changed vendor layout stops the build.
			python3 "$redundant_copy_patcher" "patched.o"
		fi
		if [ "$screen_wait" = 1 ] && [ "$member" = AirPlayScreen.o ]; then
			# Add the new symbol without globally renaming vTaskDelay, then let
			# the strict ELF helper retarget only ScreenThread's loop delay.
			"$objcopy_tool" --add-symbol \
				carbox_screen_queue_wait=0,global \
				"patched.o" "patched-with-screen-wait.o"
			mv "patched-with-screen-wait.o" "patched.o"
			python3 "$screen_wait_patcher" "patched.o"
		fi
		mv "patched.o" "$member"
		"$ar_tool" r "$output_archive" "$member"
	done
)
"$ar_tool" s "$output_archive"
