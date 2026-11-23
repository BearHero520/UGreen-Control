#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/App.Native.UGreenLED/app/server/lib/hardware_profile.sh"
source "$ROOT/App.Native.UGreenLED/app/server/lib/bios_control.sh"
UGREEN_PRODUCT_NAME='DXP4800 GT'
[[ $(bios_detected_profile) == dxp4800gt ]]
bios_supported_model
bios_write_confirmation_required
if bios_direct_fan_fallback_active; then exit 1; fi
CAPTURE=$(mktemp)
trap 'rm -f "$CAPTURE"' EXIT
bios_cli() { printf '%s\n' "$*" > "$CAPTURE"; printf '%s\n' "$*"; }
_bios_log_info() { :; }
_bios_log_error() { :; }
bios_set_fan cpu 40
[[ $(cat "$CAPTURE") == "--force --apply fan set cpu 40" ]]
bios_set_fan sys 198
if bios_set_fan sys 199; then exit 1; fi
if bios_set_fan cpu 39; then exit 1; fi
if bios_set_fan all 100; then exit 1; fi
bios_set_startup on
[[ $(cat "$CAPTURE") == "--force --apply power startup set on" ]]
bios_set_startup off
if bios_set_startup last; then exit 1; fi
if bios_fan_curve_start custom 10 60 40 50,55,75,80,90 40,45,50,55,70 45,50,60,65,70 64,128,204,255 true true; then exit 1; fi
bios_cli() { printf 'cpu: pwm=100 rpm=1234 mode=manual\nsys: pwm=120 rpm=2345 mode=manual\n'; }
bios_read_cli_fans
[[ -z "$BIOS_CHIP_ID" ]]
UGREEN_PRODUCT_NAME='DXP4800 GT-other'
if bios_supported_model; then exit 1; fi
echo 'DXP4800 GT adapter: exact DMI, ranges, policies and EC/IT8613 separation passed'
