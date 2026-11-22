#!/usr/bin/env bash
# Installs the boot switch check into GRUB.
#
#   sudo ./scripts/install-grub.sh --list
#       Lists the top-level menu entries and their IDs. Changes nothing.
#   sudo ./scripts/install-grub.sh
#       Finds the Linux entry (the first entry) and the Windows entry (the one that
#       chainloads bootmgfw.efi), installs /etc/grub.d/06_hwswitch and regenerates grub.cfg.
#   sudo ./scripts/install-grub.sh --linux-id ID --windows-id ID
#       The same, with IDs you choose from --list.
#
# grub.cfg is backed up to /root first. If any check fails afterwards, the
# backup is put back and /etc/grub.d/06_hwswitch is removed.
set -euo pipefail

usage() {
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

list_only=0
linux_id=""
windows_id=""
while [ $# -gt 0 ]; do
    case "$1" in
        --list) list_only=1 ;;
        --linux-id) linux_id="${2:?}"; shift ;;
        --windows-id) windows_id="${2:?}"; shift ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1" >&2; usage ;;
    esac
    shift
done

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this with sudo." >&2
    exit 1
fi

project="$(cd "$(dirname "$0")/.." && pwd)"
target=/etc/grub.d/06_hwswitch

# GRUB is packaged as grub-* on most distributions and grub2-* on Fedora and openSUSE.
if command -v grub-mkconfig >/dev/null; then
    mkconfig=grub-mkconfig
elif command -v grub2-mkconfig >/dev/null; then
    mkconfig=grub2-mkconfig
else
    echo "grub-mkconfig not found. Is GRUB installed?" >&2
    exit 1
fi
for cfg in /boot/grub/grub.cfg /boot/grub2/grub.cfg ""; do
    [ -f "$cfg" ] && break
done
if [ -z "$cfg" ]; then
    echo "No grub.cfg in /boot/grub or /boot/grub2." >&2
    exit 1
fi

if grep -qs '^GRUB_ENABLE_BLSCFG=true' /etc/default/grub; then
    echo "This system uses BootLoaderSpec entries (GRUB_ENABLE_BLSCFG=true), as Fedora does." >&2
    echo "Their IDs are not in grub.cfg, so they can't be found automatically." >&2
    echo "Pass --linux-id and --windows-id; an entry's ID is its file name in /boot/loader/entries" >&2
    echo "without .conf. Windows still needs a normal menuentry from os-prober or a custom script." >&2
    [ -n "$linux_id" ] && [ -n "$windows_id" ] || exit 1
fi

# Prints "ID<tab>title" for each top-level menu entry, plus "WINDOWS<tab>ID" lines
# for entries that chainload the Windows Boot Manager.
scan_entries() {
    awk '
        /^menuentry / {
            title = $0; sub(/^menuentry ['"'"'"]/, "", title); sub(/['"'"'"].*/, "", title)
            id = ""
            if (match($0, /\$menuentry_id_option ['"'"'"][^'"'"'"]+/)) {
                id = substr($0, RSTART, RLENGTH); sub(/^\$menuentry_id_option ['"'"'"]/, "", id)
            }
            print "ENTRY\t" id "\t" title
            current = id; depth = 1; next
        }
        /^submenu / { current = ""; depth = 0; next }
        current != "" && /bootmgfw\.efi/ { print "WINDOWS\t" current; current = "" }
        /^}/ { current = "" }
    ' "$1"
}

entries="$(scan_entries "$cfg")"

if [ "$list_only" -eq 1 ]; then
    echo "Top-level entries in $cfg (ID, then title):"
    printf '%s\n' "$entries" | awk -F'\t' '$1 == "ENTRY" { printf "  %-50s %s\n", ($2 == "" ? "(no ID)" : $2), $3 }'
    exit 0
fi

if [ -z "$linux_id" ]; then
    linux_id="$(printf '%s\n' "$entries" | awk -F'\t' '$1 == "ENTRY" { print $2; exit }')"
    linux_title="$(printf '%s\n' "$entries" | awk -F'\t' '$1 == "ENTRY" { print $3; exit }')"
    echo "Linux entry:   $linux_title ($linux_id)"
fi
if [ -z "$windows_id" ]; then
    mapfile -t found < <(printf '%s\n' "$entries" | awk -F'\t' '$1 == "WINDOWS" { print $2 }' | sort -u)
    if [ "${#found[@]}" -eq 0 ]; then
        echo "No Windows entry found in $cfg. Add one first (os-prober or /etc/grub.d/40_custom)," >&2
        echo "then run this again, or pass --windows-id." >&2
        exit 1
    elif [ "${#found[@]}" -gt 1 ]; then
        echo "More than one Windows entry found: ${found[*]}" >&2
        echo "Pick one with --windows-id." >&2
        exit 1
    fi
    windows_id="${found[0]}"
    echo "Windows entry: $windows_id"
fi
if [ -z "$linux_id" ] || [ -z "$windows_id" ]; then
    echo "An entry has no ID. Run with --list and pass --linux-id and --windows-id." >&2
    exit 1
fi

stamp="$(date +%F-%H%M%S)"
backup="/root/grub.cfg.bak-$stamp"
cp -a "$cfg" "$backup"

sed -e "s|@LINUX_ID@|$linux_id|" -e "s|@WINDOWS_ID@|$windows_id|" \
    "$project/grub/06_hwswitch.in" > "$target"
chmod 0755 "$target"

"$mkconfig" -o "$cfg"

fail=0
grep -q 'B007-5317' "$cfg" || { echo "The switch check is missing from $cfg." >&2; fail=1; }
grep -q "'$linux_id'" "$cfg" || { echo "No entry with ID $linux_id in the new $cfg." >&2; fail=1; }
grep -q "'$windows_id'" "$cfg" || { echo "No entry with ID $windows_id in the new $cfg." >&2; fail=1; }
if [ "$fail" -ne 0 ]; then
    cp -a "$backup" "$cfg"
    rm -f "$target"
    echo "Checks failed. The previous grub.cfg was restored and $target removed." >&2
    exit 1
fi

# Anything after the switch block that sets timeout or default would override it.
late="$(awk '/^### END \/etc\/grub.d\/06_hwswitch ###/ { seen = 1 } seen && /^[[:space:]]*set (timeout|default)=/ { n++ } END { print n + 0 }' "$cfg")"
if [ "$late" -gt 0 ]; then
    echo
    echo "Warning: $cfg sets timeout or default again after the switch check."
    echo "Those lines may override the switch. Check them, or move the check later by"
    echo "renaming $target to /etc/grub.d/99_hwswitch and running $mkconfig -o $cfg again."
fi

echo
echo "Boot switch installed. grub.cfg backup: $backup"
echo "To remove it: sudo rm $target && sudo $mkconfig -o $cfg"
