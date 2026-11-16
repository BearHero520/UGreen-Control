#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
cleanup() { [[ ! -f "$TMP/run/fan-curve.pid" ]] || kill "$(cat "$TMP/run/fan-curve.pid")" 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT
mkdir -p "$TMP/run" "$TMP/var" "$TMP/models"
for model in dx4600 dxp4800 dxp4800plus dxp4800s dxp480tplus dxp6800pro dxp4800gt; do touch "$TMP/models/$model.so"; done
printf '#!/bin/sh\nexit 0\n' > "$TMP/cli"
printf '#!/bin/sh\nexec sleep 120\n' > "$TMP/fand"
chmod +x "$TMP/cli" "$TMP/fand"
export UGREEN_PRODUCT_NAME='DXP4800 GT'
export BIOS_UGREENCTL="$TMP/cli" BIOS_UGREENCTL_FAND="$TMP/fand" BIOS_UGREENCTL_PLUGIN_DIR="$TMP/models"
VAR_DIR="$TMP/var"; RUNTIME_DIR="$TMP/run"; SETTINGS_FILE="$TMP/var/settings.conf"
source "$ROOT/App.Native.UGreenLED/app/server/lib/settings.sh"
source "$ROOT/App.Native.UGreenLED/app/server/lib/hardware_profile.sh"
source "$ROOT/App.Native.UGreenLED/app/server/lib/bios_control.sh"
_bios_log_info() { :; }
_bios_log_error() { :; }
bios_fan_curve_start stock-4800gt 10 60 64 50,55,75,80,90 40,45,50,55,70 45,50,60,65,70 64,128,204,255 false true
bios_fan_curve_running
[[ $(settings_get "$SETTINGS_FILE" fan_curve enabled false) == true ]]
bios_fan_curve_stop true
bios_fan_curve_restore
bios_fan_curve_running
grep -Fxq 'profile=stock-4800gt' "$TMP/var/fan-curve.conf"
grep -Fxq 'failsafe_pwm=198' "$TMP/var/fan-curve.conf"
grep -Fxq 'minimum_pwm=40' "$TMP/var/fan-curve.conf"
bios_fan_curve_stop
[[ $(settings_get "$SETTINGS_FILE" fan_curve enabled true) == false ]]
echo 'GT stock curve saved configuration, restart and stop passed'
