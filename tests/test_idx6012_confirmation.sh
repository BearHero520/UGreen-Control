#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
API="$ROOT/App.Native.UGreenLED/app/ui/api.cgi"
for fn in bios_write_risk_acknowledgement_value bios_write_risk_acknowledged bios_remember_write_risk; do
    source /dev/stdin <<< "$(sed -n "/^${fn}()/,/^}/p" "$API")"
done
fixture_model=idx6012
saved=''
SETTINGS_FILE=fixture
bios_detected_profile() { echo "$fixture_model"; }
bios_write_confirmation_token() { echo firmware-reversed; }
bios_write_confirmation_required() { return 0; }
settings_get() { printf '%s' "$saved"; }
settings_set() { saved="$4"; }
if bios_write_risk_acknowledged; then exit 1; fi
bios_remember_write_risk
[[ "$saved" == v1:firmware-reversed:idx6012 ]]
bios_write_risk_acknowledged
fixture_model=dxp4800
if bios_write_risk_acknowledged; then exit 1; fi
fixture_model=unknown
if bios_remember_write_risk; then exit 1; fi
echo 'iDX6012 confirmation persistence and model isolation passed'
