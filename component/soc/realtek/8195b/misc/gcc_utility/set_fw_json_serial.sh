#!/bin/sh

set -e

src="$1"
dst="$2"
serial="${3:-${FW_SERIAL_NO:-$(date +%s)}}"

if [ -z "$src" ] || [ -z "$dst" ]; then
	echo "usage: $0 <input.json> <output.json> [serial_no]" >&2
	exit 1
fi

if [ ! -f "$src" ]; then
	echo "set_fw_json_serial: input not found: $src" >&2
	exit 1
fi

case "$serial" in
	''|*[!0-9]*)
		echo "set_fw_json_serial: serial_no must be a decimal integer: $serial" >&2
		exit 1
		;;
esac

if [ "$serial" -gt 4294967294 ]; then
	echo "set_fw_json_serial: serial_no out of range: $serial" >&2
	exit 1
fi

awk -v serial="$serial" '
BEGIN {
	in_header = 0;
	update_header = 0;
	updated = 0;
}
/^[[:space:]]*"header"[[:space:]]*:[[:space:]]*\{/ {
	in_header = 1;
	update_header = 0;
}
in_header && /^[[:space:]]*"type"[[:space:]]*:/ {
	# The SDK keeps the HP firmware headers on one generation while FWLS
	# remains at serial 0.  ISP is removed from the CarBox image, so changing
	# either FWLS or ISP only creates header combinations the vendor build
	# never emits.
	if ($0 ~ /"(CINIT|XIP|FWHS_S)"/) {
		update_header = 1;
	}
}
in_header && update_header && /^[[:space:]]*"serial"[[:space:]]*:/ {
	sub(/"serial"[[:space:]]*:[[:space:]]*[0-9]+/, "\"serial\": " serial);
	updated++;
}
in_header && /^[[:space:]]*\}/ {
	in_header = 0;
	update_header = 0;
}
{ print }
END {
	if (updated != 3) {
		exit 2;
	}
}
' "$src" > "$dst" || {
	rc=$?
	rm -f "$dst"
	if [ "$rc" -eq 2 ]; then
		echo "set_fw_json_serial: failed to locate firmware header serials in $src" >&2
	else
		echo "set_fw_json_serial: failed to update $src" >&2
	fi
	exit 1
}

echo "set_fw_json_serial: $dst firmware header serials=$serial"
