#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIB="$ROOT/App.Native.UGreenLED/app/server/lib"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

source "$LIB/settings.sh"
source "$LIB/hardware_profile.sh"
source "$LIB/disk_map.sh"
UGREEN_PRODUCT_NAME='Default string'
UGREEN_PRODUCT_VERSION='iDX6011_iDX6012 V3.0 241112'
UGREEN_SYS_VENDOR=UGREEN
disk_available_slots() { printf '%s\n' disk1 disk2 disk3 disk4 disk5 disk6; }
disk_inventory_is_block() { return 0; }

# User's two SATA SSDs occupy physical bays 1/2 despite HCTL hosts 2/3.
disk_inventory_rows() {
    printf '%s\n' \
        '/dev/sdz|3:0:0:0|SSD2|SSD|1T|sata' \
        '/dev/sdb|2:0:0:0|SSD1|SSD|1T|sata'
}
disk_upstream_map_tsv() {
    printf 'device\tinterface\tslot_name\tled\n/dev/sdb\tsata\tata3\tdisk1\n/dev/sdz\tsata\tata4\tdisk2\n'
}
expected=$'/dev/sdb|2:0:0:0|disk1|SSD1|sata|ata3\n/dev/sdz|3:0:0:0|disk2|SSD2|sata|ata4'
[[ $(disk_build_mapping) == "$expected" ]]

# Check all six bays plus unlit M.2 NVMe and exclude USB/unmapped rows.
disk_inventory_rows() {
    printf '%s\n' \
        '/dev/sda|0:0:0:0|S5|MODEL|1T|sata' \
        '/dev/sdb|1:0:0:0|S6|MODEL|1T|sata' \
        '/dev/sdc|2:0:0:0|S1|MODEL|1T|sata' \
        '/dev/sdd|3:0:0:0|S2|MODEL|1T|sata' \
        '/dev/sde|4:0:0:0|S3|MODEL|1T|sata' \
        '/dev/sdf|5:0:0:0|S4|MODEL|1T|sata' \
        '/dev/nvme0n1||NVME1|MODEL|1T|nvme' \
        '/dev/sdg|6:0:0:0|OTHER|MODEL|1T|usb'
}
disk_upstream_map_tsv() {
    printf 'device\tinterface\tslot_name\tled\n'
    printf '/dev/sdc\tsata\tata3\tdisk1\n'
    printf '/dev/sdd\tsata\tata4\tdisk2\n'
    printf '/dev/sde\tsata\tata5\tdisk3\n'
    printf '/dev/sdf\tsata\tata6\tdisk4\n'
    printf '/dev/sda\tsata\tata1\tdisk5\n'
    printf '/dev/sdb\tsata\tata2\tdisk6\n'
    printf '/dev/nvme0n1\tnvme\tnvme1\t-\n'
}
DISK_LED_MAP=()
disk_load_auto_mapping_live
[[ ${DISK_LED_MAP[/dev/sdc]} == disk1 && ${DISK_LED_MAP[/dev/sda]} == disk5 ]]
[[ -z ${DISK_LED_MAP[/dev/nvme0n1]:-} && -z ${DISK_LED_MAP[/dev/sdg]:-} ]]

# Preserve existing [disk_map] when upstream returns empty rows.
SETTINGS_FILE="$TMP/settings.conf"
settings_init "$SETTINGS_FILE"
disk_refresh_mapping "$SETTINGS_FILE"
[[ ${DISK_LED_MAP[/dev/sdc]} == disk1 && ${DISK_LED_MAP[/dev/sdb]} == disk6 ]]
disk_upstream_map_tsv() {
    printf 'device\tinterface\tslot_name\tled\n'
}
if disk_refresh_mapping "$SETTINGS_FILE"; then
    echo 'FAIL: empty upstream disk map must return failure' >&2
    exit 1
fi
[[ ${DISK_LED_MAP[/dev/sdc]} == disk1 && ${DISK_LED_MAP[/dev/sdb]} == disk6 ]]
echo 'iDX6012 sparse SSD, six-bay upstream TSV routing and empty-map preservation passed'
