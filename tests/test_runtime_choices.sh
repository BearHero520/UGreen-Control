#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIB="$ROOT/App.Native.UGreenLED/app/server/lib"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
source "$LIB/settings.sh"
source "$LIB/bios_control.sh"
source "$LIB/fan_telemetry.sh"
SETTINGS_FILE="$WORK/settings.conf"; VAR_DIR="$WORK"; RUNTIME_DIR="$WORK"
fixture_model=idx6012; fixture_revision=board1
bios_detected_profile() { echo "$fixture_model"; }
hardware_detected_product_name() { echo 'Default string'; }
hardware_detected_product_version() { echo "$fixture_revision"; }
bios_set_fan() { [[ ! -f "$WORK/fail" ]] || return 1; echo "fan $*" >> "$WORK/calls"; }
bios_set_wol() { [[ ! -f "$WORK/fail" ]] || return 1; echo "wol $*" >> "$WORK/calls"; }
bios_set_fan_mode() { echo "mode $*" >> "$WORK/calls"; }
bios_set_fan_saved sys 150
bios_set_wol_saved on
: > "$WORK/calls"
bios_fixed_restore; bios_wol_restore
grep -q 'fan sys 150' "$WORK/calls"; grep -q 'wol on' "$WORK/calls"
fixture_revision=other; : > "$WORK/calls"
bios_fixed_restore; bios_wol_restore
[[ ! -s "$WORK/calls" ]]
fixture_revision=board1
old=$(settings_get "$SETTINGS_FILE" bios fixed_selection '')
touch "$WORK/fail"
if bios_set_fan_saved sys 100; then exit 1; fi
[[ $(settings_get "$SETTINGS_FILE" bios fixed_selection '') == "$old" ]]
if bios_set_wol_saved off; then exit 1; fi
[[ $(settings_get "$SETTINGS_FILE" bios wol_selection '') == *'|on|' ]]
rm "$WORK/fail"
settings_set "$SETTINGS_FILE" fan_curve enabled true
: > "$WORK/calls"; bios_fixed_restore; [[ ! -s "$WORK/calls" ]]
settings_set "$SETTINGS_FILE" fan_curve enabled false
# Simulate a slow graceful shutdown; stopping must wait for completion.
cat > "$WORK/daemon" <<'EOF'
#!/bin/bash
trap 'sleep 0.2; echo stopped > "$1"; exit 0' TERM
while :; do sleep 0.05; done
EOF
bash "$WORK/daemon" "$WORK/stopped" &
pid=$!; echo "$pid" > "$(bios_fan_curve_pid_path)"
sleep 0.1
bios_fan_curve_stop true
[[ -f "$WORK/stopped" && ! -f "$(bios_fan_curve_pid_path)" ]]
(
    echo 999999 > "$(bios_fan_curve_pid_path)"
    bios_process_alive() { return 0; }
    kill() { return 0; }
    sleep() { :; }
    if bios_fan_curve_stop true; then exit 1; fi
    [[ -f "$(bios_fan_curve_pid_path)" && -n "$BIOS_LAST_ERROR" ]]
)
rm -f "$(bios_fan_curve_pid_path)"
bios_set_auto_saved
[[ -z $(settings_get "$SETTINGS_FILE" bios fixed_selection '') ]]
BIOS_SUPPORTED=true BIOS_MODEL=idx6012 BIOS_CPU_FAN_PRESENT=false
BIOS_CPU_CELSIUS=40 BIOS_CPU_PEAK_CELSIUS=45 BIOS_HDD_CELSIUS=35 BIOS_SSD_CELSIUS=50
BIOS_CPU_RPM=0 BIOS_SYS_RPM=1100 BIOS_SYS2_RPM=1090
json=$(fan_telemetry_current_json 123)
[[ "$json" == *'"sys2Rpm":1090'* && "$json" == *'"cpuRpm":-1'* ]]
fan_telemetry_append_bios_status 123
grep -q '|-1|1100|1090$' "$(fan_telemetry_history_path)"
echo 'runtime choices: persistence, identity isolation, failure preservation, curve precedence, wait and paired telemetry passed'
