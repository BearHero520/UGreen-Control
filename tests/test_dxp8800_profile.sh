#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/App.Native.UGreenLED/app/server/lib/hardware_profile.sh"
source "$ROOT/App.Native.UGreenLED/app/server/lib/bios_control.sh"
for model in 'DXP8800' 'DXP8800 Plus' 'DXP8800 Pro' 'DXP8800 Ultra'; do
    UGREEN_PRODUCT_NAME="$model"
    [[ $(hardware_profile_key) == dxp8800 ]]
    [[ $(hardware_profile_display_name) == "UGREEN $model" ]]
    [[ $(hardware_disk_count) == 8 ]]
    # Only the exact Pro DMI is authorized by the new independent plugin.
    if [[ "$model" == 'DXP8800 Pro' ]]; then
        bios_supported_model
        [[ $(bios_detected_profile) == dxp8800pro ]]
        [[ $(bios_fan_curve_stock_profile_for_model) == stock-8800pro ]]
    elif bios_supported_model; then echo 'Different BIOS model enabled' >&2; exit 1; fi
done
UGREEN_PRODUCT_NAME='UGREEN DXP8800 Pro'
[[ $(hardware_profile_display_name) == 'UGREEN DXP8800 Pro' ]]
UGREEN_PRODUCT_NAME='unrelated'
[[ $(hardware_profile_display_name dxp8800) == 'UGREEN DXP8800 系列' ]]
echo 'DXP8800 labels preserve identity and exact Pro BIOS/fan capability routing'
