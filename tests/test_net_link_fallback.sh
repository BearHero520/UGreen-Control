#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/sys/eth0/statistics"
printf up > "$TMP/sys/eth0/operstate"
printf 1 > "$TMP/sys/eth0/carrier"
printf 100 > "$TMP/sys/eth0/statistics/rx_bytes"

export NET_SYS_CLASS_DIR="$TMP/sys"
export NET_STATE_CACHE_FILE="$TMP/state"
export NET_SPEED_CACHE_FILE="$TMP/speed"
source "$ROOT/App.Native.UGreenLED/app/server/lib/net_state.sh"
net_can_ping_domestic() { return 1; }
net_can_ping_overseas() { return 1; }

[[ "$(net_probe_state)" == 'connected|0|0' ]] || {
    echo 'FAIL: an up local carrier was still reported as disconnected' >&2
    exit 1
}

echo 'network link fallback test passed'
