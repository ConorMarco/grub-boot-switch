#!/usr/bin/env bash
# Flashes the boot switch firmware to a Pro Micro.
# Uses firmware/bootswitch.hex if you built it, otherwise prebuilt/bootswitch.hex.
# Works whether the board runs its original program (USB ID 1b4f:9206) or the
# boot switch firmware (03eb:2068). Both restart into the bootloader when their
# serial port is opened at 1200 baud and closed.
# If neither is found, double-tap the board's RST pin to GND and run this again
# within 8 seconds.
# Usage: sudo ./scripts/flash-firmware.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this with sudo." >&2
    exit 1
fi

project="$(cd "$(dirname "$0")/.." && pwd)"
hex="$project/firmware/bootswitch.hex"
[ -f "$hex" ] || hex="$project/prebuilt/bootswitch.hex"
echo "Firmware: $hex"

# Prints the /dev/ttyACM* port whose USB ID is one of the arguments (vendor:product).
find_port() {
    local dev id
    for dev in /dev/ttyACM*; do
        [ -e "$dev" ] || continue
        id="$(udevadm info -q property -n "$dev" | awk -F= '/^ID_VENDOR_ID=/{v=$2} /^ID_MODEL_ID=/{p=$2} END{print v":"p}')"
        for want in "$@"; do
            [ "$id" = "$want" ] && { echo "$dev"; return 0; }
        done
    done
    return 1
}

bootloader_ids=(1b4f:9205 1b4f:0036 2341:0036)
app_ids=(1b4f:9206 03eb:2068)

if ! port="$(find_port "${bootloader_ids[@]}")"; then
    if app_port="$(find_port "${app_ids[@]}")"; then
        echo "Restarting the board into its bootloader via $app_port..."
        stty -F "$app_port" 1200
        : < "$app_port" || true
    else
        echo "No Pro Micro found. Waiting 8 seconds; double-tap RST to GND now if you like."
    fi
    for _ in $(seq 1 40); do
        sleep 0.2
        port="$(find_port "${bootloader_ids[@]}")" && break
    done
fi

if [ -z "${port:-}" ]; then
    echo "The bootloader did not appear. Nothing was written." >&2
    echo "Double-tap the RST pin to GND, then run this again straight away." >&2
    exit 1
fi

echo "Bootloader found at $port. Flashing..."
avrdude -p atmega32u4 -c avr109 -P "$port" -b 57600 -D -U "flash:w:$hex:i"

echo "Waiting for the boot switch to start..."
for _ in $(seq 1 50); do
    sleep 0.2
    if lsusb -d 03eb:2068 >/dev/null; then
        echo "Done. The boot switch is running."
        exit 0
    fi
done
echo "Flashing finished, but the boot switch did not appear on USB within 10 seconds." >&2
exit 1
