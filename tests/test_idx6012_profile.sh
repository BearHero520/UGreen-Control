#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/App.Native.UGreenLED/app/server/lib/hardware_profile.sh"
UGREEN_PRODUCT_NAME='Default string'
UGREEN_PRODUCT_VERSION='iDX6011_iDX6012 V3.0 241112'
UGREEN_SYS_VENDOR=UGREEN
[[ $(hardware_profile_key) == idx6012 ]]
[[ $(hardware_disk_count) == 6 ]]
[[ $(hardware_hctl_profile) == upstream-managed ]]
[[ $(hardware_netdev_count) == 1 ]]
[[ $(hardware_network_leds) == netdev ]]
[[ $(hardware_profile_display_name) == 'UGREEN iDX6012' ]]
[[ $(hardware_support_level) == experimental ]]
hardware_profile_options | grep -q '^idx6012|'
UGREEN_SYS_VENDOR=Other
[[ $(hardware_profile_key) == unknown ]]
UGREEN_SYS_VENDOR=UGREEN
UGREEN_PRODUCT_VERSION+='-other'
[[ $(hardware_profile_key) == unknown ]]
UGREEN_PRODUCT_NAME=iDX6012
[[ $(hardware_profile_key) == idx6012 ]]
# Existing explicit selections remain user-owned; select auto/idx6012 on the NAS.
SETTINGS_FILE=fixture
settings_get() { echo idx6011; }
[[ $(hardware_profile_key) == idx6011 ]]
settings_get() { echo idx6012; }
[[ $(hardware_netdev_count) == 1 ]]
echo 'iDX6012 application profile and single network layout passed'
