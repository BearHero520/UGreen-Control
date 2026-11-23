#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fixture=$(mktemp -d /tmp/llled-telemetry.XXXXXXXX)
fixture=$(cd "$fixture" && pwd -P)
[[ "$fixture" == /tmp/llled-telemetry.* ]] || exit 1
trap '[[ "$fixture" == /tmp/llled-telemetry.* ]] && rm -rf -- "$fixture"' EXIT
export UGREEN_DISK_MODE=stats UGREEN_NETWORK_MODE=carrier
export DISK_SYS_CLASS_DIR="$fixture/block" DISK_IO_CACHE_DIR="$fixture/cache"
export DISKSTATS_FILE="$fixture/diskstats" NET_SYS_CLASS_DIR="$fixture/net"
mkdir -p "$DISK_SYS_CLASS_DIR/sda/device" "$DISK_IO_CACHE_DIR" "$NET_SYS_CLASS_DIR/eth0/statistics"
printf 'fixture disk\n' > "$DISK_SYS_CLASS_DIR/sda/device/model"
printf '100000\n' > "$DISK_SYS_CLASS_DIR/sda/size"
printf '1\n' > "$NET_SYS_CLASS_DIR/eth0/carrier"
printf '100\n' > "$NET_SYS_CLASS_DIR/eth0/statistics/rx_bytes"
printf '8 0 sda 1 0 100 0 1 0 100 0 0 0 0\n' > "$DISKSTATS_FILE"
source "$root/app/server/lib/disk_map.sh"
source "$root/app/server/lib/disk_state.sh"
source "$root/app/server/lib/net_state.sh"
# Fixture the topology resolver. No devices, host sysfs or raw disk commands.
readlink() { echo /fixture/devices/0:0:0:0; }
ip() { return 0; }
hdparm() { echo 'unexpected raw disk probe' >&2; exit 99; }
net_can_ping_domestic() { echo 'unexpected network probe' >&2; exit 99; }
net_can_ping_overseas() { echo 'unexpected network probe' >&2; exit 99; }
[[ "$(disk_inventory_rows)" == '/dev/sda|0:0:0:0||fixture disk|100000|' ]]
[[ "$(disk_detect_smart_sample /dev/sda)" == 'idle|0|0|0' ]]
printf '100 100 %s 0\n' "$(($(date +%s) - 2))" > "$DISK_IO_CACHE_DIR/sda.io"
printf '8 0 sda 1 0 200 0 1 0 200 0 0 0 0\n' > "$DISKSTATS_FILE"
[[ "$(disk_detect_smart_sample /dev/sda)" == active\|* ]]
[[ "$(disk_detect_smart_sample /dev/sdb)" == 'offline|0|0|0' ]]
[[ "$(net_probe_state)" == 'connected|0|0' ]]
printf '0\n' > "$NET_SYS_CLASS_DIR/eth0/carrier"
[[ "$(net_probe_state)" == 'disconnected|0|0' ]]
echo 'platform telemetry fixtures passed'
