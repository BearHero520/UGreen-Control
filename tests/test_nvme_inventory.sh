#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/App.Native.UGreenLED/app/server/lib/disk_map.sh"
disk_inventory_is_block() { [[ "$1" != /dev/missing ]]; }
lsblk() {
    [[ "$*" == '-d -n -P -o NAME,TYPE,HCTL,SERIAL,MODEL,SIZE,TRAN' ]] || return 1
    cat <<'EOF'
NAME="sda" TYPE="disk" HCTL="2:0:0:0" SERIAL="SATA-A" MODEL="SATA SSD" SIZE="1T" TRAN="sata"
NAME="nvme0n1" TYPE="disk" HCTL="" SERIAL="U2-A" MODEL="NVMe U2" SIZE="4T" TRAN="nvme"
NAME="nvme1n1" TYPE="disk" HCTL="" SERIAL="M2-A" MODEL="NVMe M2" SIZE="1T" TRAN=""
NAME="nvme0n1p1" TYPE="part" HCTL="" SERIAL="U2-A" MODEL="NVMe U2" SIZE="4T" TRAN="nvme"
NAME="loop0" TYPE="loop" HCTL="" SERIAL="" MODEL="" SIZE="1G" TRAN=""
NAME="missing" TYPE="disk" HCTL="" SERIAL="" MODEL="" SIZE="1G" TRAN=""
EOF
}
rows=$(disk_inventory_rows)
[[ $(wc -l <<< "$rows") -eq 3 ]]
grep -Fxq '/dev/nvme0n1||U2-A|NVMe U2|4T|nvme' <<< "$rows"
grep -Fxq '/dev/nvme1n1||M2-A|NVMe M2|1T|nvme' <<< "$rows"
disk_upstream_map_tsv() {
    printf 'device\tinterface\tslot_name\tled\n/dev/sda\tsata\tata3\tdisk3\n/dev/nvme0n1\tnvme\t-\t-\n/dev/nvme1n1\tnvme\t-\t-\n'
}
[[ $(disk_build_mapping) == '/dev/sda|2:0:0:0|disk3|SATA-A|sata|ata3' ]]
unset -f disk_upstream_map_tsv
source "$ROOT/App.Native.UGreenLED/app/server/lib/disk_map.sh"
disk_inventory_is_block() { [[ "$1" != /dev/missing ]]; }
echo 'SATA/NVMe inventory, partitions excluded, unknown NVMe LED mapping preserved: passed'

# Exercise the production loader and /mapping response with the known GT layout.
fixture=$(mktemp -d)
trap 'rm -f "$fixture/settings" "$fixture/status" "$fixture/ugreenctl"; rmdir "$fixture"' EXIT
SETTINGS_FILE="$fixture/settings"
DISK_STATUS_FILE="$fixture/status"
: > "$SETTINGS_FILE"
settings_get() { printf '%s\n' auto; }
hardware_detected_product_name() { printf '%s\n' 'DXP4800 GT'; }
SERVER_DIR="$ROOT/App.Native.UGreenLED/app/server"
EXPECTED_UGREENCTL="$SERVER_DIR/bin/ugreenctl"
if [[ ! -x "$EXPECTED_UGREENCTL" ]]; then
    printf '#!/bin/sh\nexit 0\n' > "$fixture/ugreenctl"
    chmod +x "$fixture/ugreenctl"
    UGREENCTL_BINARY="$fixture/ugreenctl"
    EXPECTED_UGREENCTL="$UGREENCTL_BINARY"
fi
topology_unavailable=false
timeout() {
    [[ "$*" == "5 $EXPECTED_UGREENCTL --plugin-dir $SERVER_DIR/lib/ugreenctl/models disk map --tsv" ]] || return 99
    ! $topology_unavailable || return 1
    printf 'device\tinterface\tslot_name\tled\n/dev/sda\tsata\tata3\tdisk3\n/dev/nvme0n1\tu2\tata1\tdisk1\n/dev/nvme1n1\tnvme\tnvme2\t-\n'
}
disk_load_mapping_from_settings "$SETTINGS_FILE"
[[ ${DISK_LED_MAP[/dev/nvme0n1]} == disk1 ]]
[[ -z ${DISK_LED_MAP[/dev/nvme1n1]:-} && ${DISK_MAP_SLOT[/dev/nvme1n1]} == nvme2 ]]
json_str() { printf '%s' "$1"; }
runtime_file_fresh() { [[ -f "$1" ]]; }
printf '/dev/sda|disk3|idle|0|0|0|5\n/dev/nvme0n1|disk4|active|10|20|30|5\n' > "$DISK_STATUS_FILE"
mapping_case=$(sed -n '/^    \/mapping)/,/^    \/settings)/p' "$ROOT/App.Native.UGreenLED/app/ui/api.cgi" | sed '$d')
mapping_response() { eval "case /mapping in
$mapping_case
esac"; }
response=$(mapping_response)
[[ $response == *'"device":"/dev/nvme0n1","led":"disk1","state":"unknown"'*'"transport":"u2","slot_name":"ata1"'* ]]
[[ $response == *'"device":"/dev/nvme1n1","led":"","state":"unknown","mapped":false,"transport":"nvme"'*'"slot_name":"nvme2"'* ]]
[[ $response != *'"led":"disk4"'* ]]
[[ $(grep -o '"device":"/dev/sda"' <<< "$response" | wc -l) -eq 1 ]]
topology_unavailable=true
response=$(mapping_response)
[[ $response == *'"device":"/dev/nvme0n1","led":"","state":"unknown","mapped":false'* && $response != *'"led":"disk3"'* ]]
echo 'GT U.2 LED slot, unlit M.2, stale-state rejection and no HCTL fallback: passed'
source "$ROOT/App.Native.UGreenLED/app/server/lib/disk_state.sh"
timeout() { echo 'Unexpected ATA probe' >&2; exit 99; }
[[ $(disk_read_power_state_cached /dev/nvme0n1) == idle ]]
