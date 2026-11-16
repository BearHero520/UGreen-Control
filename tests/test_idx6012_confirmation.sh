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
hardware_detected_product_name() { echo 'Default string'; }
hardware_detected_product_version() { echo 'iDX6011_iDX6012 V3.0 241112'; }
bios_supported_model() { [[ "$fixture_model" != unknown ]]; }
bios_wol_supported_model() { return 1; }
bios_rtc_supported_model() { return 1; }
bios_write_confirmation_token() { echo firmware-reversed; }
bios_write_confirmation_required() { return 0; }
settings_get() { printf '%s' "$saved"; }
settings_set() { saved="$4"; }
if bios_write_risk_acknowledged; then exit 1; fi
bios_remember_write_risk
[[ "$saved" == 'v2:firmware-reversed:idx6012:iDX6011_iDX6012 V3.0 241112' ]]
bios_write_risk_acknowledged
fixture_model=dxp4800
if bios_write_risk_acknowledged; then exit 1; fi
fixture_model=unknown
if bios_remember_write_risk; then exit 1; fi
echo 'iDX6012 confirmation persistence and model isolation passed'
