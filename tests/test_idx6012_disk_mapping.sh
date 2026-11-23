#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIB="$ROOT/App.Native.UGreenLED/app/server/lib"
source "$LIB/hardware_profile.sh"
source "$LIB/disk_map.sh"
UGREEN_PRODUCT_NAME='Default string'
UGREEN_PRODUCT_VERSION='iDX6011_iDX6012 V3.0 241112'
UGREEN_SYS_VENDOR=UGREEN
disk_available_slots() { printf '%s\n' disk1 disk2 disk3 disk4 disk5 disk6; }

# User's two SATA SSDs occupy physical bays 1/2 despite HCTL hosts 2/3.
# /dev names are intentionally shuffled: neither names nor occupied-disk rank
# may select the bay number.
disk_inventory_rows() {
    printf '%s\n' \
        '/dev/sdz|3:0:0:0|SSD2|SSD|1T|sata' \
        '/dev/sdb|2:0:0:0|SSD1|SSD|1T|sata'
}
expected=$'/dev/sdb|2:0:0:0|disk1|SSD1\n/dev/sdz|3:0:0:0|disk2|SSD2'
[[ $(disk_build_mapping) == "$expected" ]]

# Check all six bays, retaining empty positions and excluding unsupported HCTL.
disk_inventory_rows() {
    printf '%s\n' \
        '/dev/sda|0:0:0:0|S5|MODEL|1T|sata' \
        '/dev/sdb|1:0:0:0|S6|MODEL|1T|sata' \
        '/dev/sdc|2:0:0:0|S1|MODEL|1T|sata' \
        '/dev/sdd|3:0:0:0|S2|MODEL|1T|sata' \
        '/dev/sde|4:0:0:0|S3|MODEL|1T|sata' \
        '/dev/sdf|5:0:0:0|S4|MODEL|1T|sata' \
        '/dev/sdg|6:0:0:0|OTHER|MODEL|1T|usb'
}
expected=$'/dev/sda|0:0:0:0|disk5|S5\n/dev/sdb|1:0:0:0|disk6|S6\n/dev/sdc|2:0:0:0|disk1|S1\n/dev/sdd|3:0:0:0|disk2|S2\n/dev/sde|4:0:0:0|disk3|S3\n/dev/sdf|5:0:0:0|disk4|S4'
[[ $(disk_build_mapping) == "$expected" ]]
DISK_LED_MAP=()
disk_load_auto_mapping_live
[[ ${DISK_LED_MAP[/dev/sdc]} == disk1 && ${DISK_LED_MAP[/dev/sda]} == disk5 ]]
[[ -z ${DISK_LED_MAP[/dev/sdg]:-} ]]

# Keep existing explicit selections and other models' routing intact.
SETTINGS_FILE=fixture
settings_get() { echo dxp6800; }
[[ $(disk_build_mapping) == "$expected" ]]
settings_get() { echo idx6012; }
[[ $(disk_build_mapping) == "$expected" ]]
settings_get() { echo dxp4800; }
[[ $(disk_guess_led_by_hctl 2:0:0:0) == disk3 ]]
echo 'iDX6012 sparse SSD, six-bay HCTL routing and model isolation passed'
