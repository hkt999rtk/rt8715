#!/bin/sh
set -eu

if [ "$#" -ne 9 ]; then
	echo "usage: $0 MODE AR OBJCOPY INPUT OUTPUT MEMBERS SCREEN_WAIT ACK_CACHE IAP2_WAIT_FIX" >&2
	exit 2
fi

mode=$1
ar_tool=$2
objcopy_tool=$3
input_archive=$4
output_archive=$5
members=$6
screen_wait=$7
ack_cache=$8
iap2_wait_fix=$9
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
screen_wait_patcher=$script_directory/../../src/carbox/tools/patch_screen_wait_relocation.py
redundant_copy_patcher=$script_directory/../../src/carbox/tools/patch_airplay_redundant_copy.py
event_response_patcher=$script_directory/../../src/carbox/tools/patch_airplay_event_response.py
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

case "$ack_cache" in
	0|1) ;;
	*)
		echo "ACK_CACHE must be 0 or 1: $ack_cache" >&2
		exit 2
		;;
esac

case "$iap2_wait_fix" in
	0|1) ;;
	*)
		echo "IAP2_WAIT_FIX must be 0 or 1: $iap2_wait_fix" >&2
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
		if [ "$ack_cache" = 1 ] && [ "$member" = AirPlayEvent.o ]; then
			# Redirect only DealWithSendHIDReport's no-body response.  Other
			# AirPlay response builders remain byte-for-byte vendor code.
			"$objcopy_tool" --add-symbol \
				carbox_airplay_event_send_fast_response=0,global \
				"patched.o" "patched-with-ack-cache.o"
			mv "patched-with-ack-cache.o" "patched.o"
			python3 "$event_response_patcher" "patched.o"
		fi
		if [ "$iap2_wait_fix" = 1 ] && [ "$member" = iAP2Ctrl.o ]; then
			# iAP2Ctrl_MsgThread overflows its 32-bit tv_sec*1000 deadline
			# after wall-clock synchronization.  The customer object currently
			# has exactly one timed-wait relocation; stop the build if that
			# invariant changes instead of silently patching a different call.
			readelf_tool=$(dirname "$objcopy_tool")/arm-none-eabi-readelf
			test -x "$readelf_tool"
			wait_relocations=$(
				"$readelf_tool" -r "patched.o" |
				awk '$NF == "pthread_cond_timedwait" { count++ } END { print count + 0 }'
			)
			if [ "$wait_relocations" -ne 1 ]; then
				echo "iAP2Ctrl.o expected one pthread_cond_timedwait relocation, found $wait_relocations" >&2
				exit 1
			fi
			"$objcopy_tool" --redefine-sym \
				pthread_cond_timedwait=carbox_iap2_cond_timedwait \
				"patched.o" "patched-with-iap2-wait.o"
			mv "patched-with-iap2-wait.o" "patched.o"
		fi
		mv "patched.o" "$member"
		"$ar_tool" r "$output_archive" "$member"
	done
)
"$ar_tool" s "$output_archive"
