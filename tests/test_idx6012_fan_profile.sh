#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/App.Native.UGreenLED/app/server/lib/hardware_profile.sh"
source "$ROOT/App.Native.UGreenLED/app/server/lib/bios_control.sh"
UGREEN_PRODUCT_NAME='Default string'
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cat > "$WORK/ctl" <<'EOF'
#!/bin/bash
[[ "$*" == *' model' ]] || exit 1
echo idx6012
EOF
chmod +x "$WORK/ctl"
BIOS_UGREENCTL="$WORK/ctl"
[[ $(bios_detected_profile) == idx6012 ]]
bios_supported_model
bios_write_confirmation_required
if bios_direct_fan_fallback_active; then exit 1; fi
bios_cli() { printf '%s\n' "$*" > "$WORK/call"; }
bios_set_fan sys 150
[[ $(cat "$WORK/call") == '--force --apply fan set sys 150' ]]
for value in 0 39 199 255; do if bios_set_fan sys "$value"; then exit 1; fi; done
if bios_set_fan cpu 100; then exit 1; fi
bios_set_fan_mode sys auto
[[ $(cat "$WORK/call") == '--force --apply fan mode sys auto' ]]
if bios_set_fan_mode sys manual; then exit 1; fi
if bios_fan_curve_start custom 10 60 40 '' '' '' '' true true; then exit 1; fi
bios_backend() { echo ugreenctl; }
bios_cli() { if [[ "$*" == *'startup get' || "$*" == *'wol get' ]]; then echo on; return; fi; printf 'sys1: pwm=unknown mode=auto tach=1370 rpm=1370\nsys2: pwm=unknown mode=auto tach=1360 rpm=1360\n'; }
bios_read_power_schedule() { :; }
bios_wake_cli() {
    case "$*" in
        'network wol models') printf 'idx6012\tiDX6011/iDX6012 OEM V3.0\twol=automatic-or-explicit\n' ;;
        'power rtc-wake models') : ;;
        'network wol get') echo on ;;
        *) printf '%s\n' "$*" > "$WORK/call" ;;
    esac
}
bios_read_thermal_snapshot() { :; }
bios_read_status
[[ "$BIOS_AVAILABLE" == true && "$BIOS_MAX_PWM" == 198 && "$BIOS_CPU_FAN_PRESENT" == false && "$BIOS_FAN_MODE_WRITABLE" == true ]]
[[ "$BIOS_STARTUP_AVAILABLE" == true && "$BIOS_WOL_AVAILABLE" == true && -z "$BIOS_CHIP_ID" ]]
[[ "$BIOS_SYS_RPM" == 1370 && "$BIOS_SYS2_RPM" == 1360 && "$BIOS_SYS_PWM" == -1 ]]
bios_cli() { printf '%s\n' "$*" > "$WORK/call"; }
bios_set_startup off
[[ $(cat "$WORK/call") == '--force --apply power startup set off' ]]
if bios_set_startup last; then exit 1; fi
bios_set_wol on
[[ $(cat "$WORK/call") == '--force --apply network wol set on' ]]
if bios_rtc_supported_model; then exit 1; fi
printf '#!/bin/bash\nexit 1\n' > "$WORK/ctl"
[[ $(bios_detected_profile) == unknown ]]
echo 'iDX6012 adapter: upstream matching, paired fan bounds, auto restore and power isolation passed'
