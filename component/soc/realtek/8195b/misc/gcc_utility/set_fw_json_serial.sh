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
	in_isp = 0;
	in_header = 0;
	done = 0;
}
/^[[:space:]]*"ISP"[[:space:]]*:[[:space:]]*\{/ {
	in_isp = 1;
}
in_isp && /^[[:space:]]*"header"[[:space:]]*:[[:space:]]*\{/ {
	in_header = 1;
}
in_isp && in_header && !done && /^[[:space:]]*"serial"[[:space:]]*:/ {
	sub(/"serial"[[:space:]]*:[[:space:]]*[0-9]+/, "\"serial\": " serial);
	done = 1;
}
in_isp && in_header && /^[[:space:]]*\}/ {
	in_header = 0;
}
in_isp && !in_header && /^[[:space:]]*\}/ {
	in_isp = 0;
}
{ print }
END {
	if (!done) {
		exit 2;
	}
}
' "$src" > "$dst" || {
	rc=$?
	rm -f "$dst"
	if [ "$rc" -eq 2 ]; then
		echo "set_fw_json_serial: failed to locate ISP.header.serial in $src" >&2
	else
		echo "set_fw_json_serial: failed to update $src" >&2
	fi
	exit 1
}

echo "set_fw_json_serial: $dst ISP.header.serial=$serial"
