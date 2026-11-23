#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
app="$root/App.Native.UGreenLED"

for file in \
    "$app/cmd/main" \
    "$app/cmd/install_callback" \
    "$app/app/server/lib/led_api.sh" \
    "$app/app/server/lib/disk_map.sh" \
    "$app/app/server/lib/bios_control.sh"; do
    if rg -n -i '(i2cset|i2cget|/dev/port|/dev/i2c|ioctl|ioperm|outb|inb|modprobe|insmod|rmmod|tee[[:space:]]+/sys|>[[:space:]]*/sys|dd[[:space:]].*of=)' "$file"; then
        printf 'forbidden local hardware access in %s\n' "$file" >&2
        exit 1
    fi
done

rg -n 'disk_upstream_map_tsv|disk map --tsv' "$app/app/server/lib/disk_map.sh" >/dev/null
rg -n 'bios_cli|UGREEN-NAS-Hardware' "$app/app/server/lib/bios_control.sh" >/dev/null
rg -n 'UGREEN_CLI|ugreen_leds_cli' "$app/app/server/lib/led_api.sh" >/dev/null

printf 'backend boundary checks passed\n'
