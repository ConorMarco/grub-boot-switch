#!/usr/bin/env bash
# Shows the switch position live by reading the board's disk, the same way GRUB does.
# Reads bypass Linux's disk cache, so changes show up straight away.
# Needs read access to the disk: run with sudo, or install udev/70-bootswitch.rules first.
# Usage: ./scripts/watch-switch.sh   (Ctrl+C to stop)
set -euo pipefail

dev=/dev/disk/by-uuid/B007-5317
last=""
while true; do
    if [ -e "$dev" ]; then
        # Block 5 of the partition is the root folder; the second entry names the position.
        name="$(dd if="$dev" bs=512 skip=5 count=1 iflag=direct status=none | dd bs=1 skip=32 count=11 status=none | tr -s ' ' | sed 's/ $//')"
        case "$name" in
            "LINUX SW") now="Linux" ;;
            "WINDOWS SW") now="Windows" ;;
            "MENU SW") now="Menu (middle)" ;;
            *) now="Unexpected: '$name'" ;;
        esac
    else
        now="Board not found"
    fi
    if [ "$now" != "$last" ]; then
        echo "$(date +%T)  $now"
        last="$now"
    fi
    sleep 0.3
done
