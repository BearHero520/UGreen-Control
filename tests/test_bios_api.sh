#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
API="$ROOT/App.Native.UGreenLED/app/ui/api.cgi"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
assert_contains() { [[ "$1" == *"$2"* ]] || fail "missing [$2] in [$1]"; }
assert_json() { python -c 'import json, sys; json.load(sys.stdin)' <<< "$1" || fail "invalid JSON response"; }

BIN="$TMP/ugreenctl"; PLUGINS="$TMP/models"; CALLS="$TMP/calls"
mkdir -p "$PLUGINS"
: > "$PLUGINS/dxp2800.so"
: > "$PLUGINS/dx4600.so"
: > "$PLUGINS/dxp4800.so"
: > "$PLUGINS/dxp4800plus.so"
: > "$PLUGINS/dxp4800s.so"
: > "$PLUGINS/dxp480tplus.so"
: > "$PLUGINS/dxp6800pro.so"
: > "$PLUGINS/dxp6800plus.so"
: > "$PLUGINS/dxp6800.so"
: > "$PLUGINS/dxp8800pro.so"
: > "$PLUGINS/dxp4800gt.so"
cat > "$BIN" <<'EOF'
#!/bin/sh
printf 'CALL' >> "$UGREENCTL_CALLS"
for arg in "$@"; do printf ' <%s>' "$arg" >> "$UGREENCTL_CALLS"; done
printf '\n' >> "$UGREENCTL_CALLS"
case " $* " in
  *" network wol models "*)
    for product in 'DX4600' 'DX4600+' 'DX4600 Pro' 'DXP4800' 'DXP4800S' 'DXP4800 Plus' 'DXP4800 Pro' 'DXP480T Plus' 'DXP6800 Pro' 'DXP6800 Plus'; do
      printf 'mock\t%s\twol=automatic-or-explicit\n' "$product"
    done
    for product in 'DXP2800' 'DXP4800 GT' 'DXP6800' 'DXP8800 Pro'; do
      printf 'mock\t%s\twol=explicit-interface-required\n' "$product"
    done ;;
  *" power rtc-wake models "*)
    for product in 'DX4600' 'DX4600+' 'DX4600 Pro' 'DXP2800' 'DXP4800' 'DXP4800S' 'DXP4800 Plus' 'DXP4800 Pro' 'DXP4800 GT' 'DXP480T Plus' 'DXP6800' 'DXP6800 Plus' 'DXP6800 Pro' 'DXP8800 Pro'; do
      printf 'mock\t%s\trtc=rtc_cmos\n' "$product"
    done ;;
  *" info "*)
    if [ "$UGREEN_PRODUCT_NAME" = 'DX4600' ]; then
      echo 'model: dx4600 (UGREEN DX4600 / DX4600+ / DX4600 Pro (firmware-reversed))'
    elif [ "$UGREEN_PRODUCT_NAME" = 'DXP6800 Pro' ] || [ "$UGREEN_PRODUCT_NAME" = 'DXP6800' ] || [ "$UGREEN_PRODUCT_NAME" = 'DXP8800 Pro' ]; then
      echo 'model: dxp6800pro (DXP6800 Pro (firmware-reversed))'
    elif [ "$UGREEN_PRODUCT_NAME" = 'DXP4800' ]; then
      echo 'model: dxp4800 (UGREEN DXP4800 (firmware-reversed))'
    else
      echo 'model: dxp4800s (UGREEN DXP4800S)'
    fi
    ;;
  *" fan status "*)
    if [ "$UGREEN_PRODUCT_NAME" = 'DX4600' ]; then
      echo 'fan sys: pwm=64 mode=manual tach=750 rpm=900'
    elif [ "$UGREEN_PRODUCT_NAME" = 'DXP6800 Pro' ] || [ "$UGREEN_PRODUCT_NAME" = 'DXP6800' ] || [ "$UGREEN_PRODUCT_NAME" = 'DXP8800 Pro' ]; then
      printf '%s\n' \
        'fan cpu: pwm=120 mode=manual tach=675 rpm=1000' \
        'fan sys1: pwm=88 mode=manual tach=750 rpm=900' \
        'fan sys2: pwm=88 mode=manual tach=450 rpm=1500'
    elif [ "$UGREEN_PRODUCT_NAME" = 'DXP4800' ]; then
      echo 'fan sys: pwm=128 mode=manual tach=750 rpm=900'
    else
      echo 'fan sys: pwm=40 mode=manual tach=750 rpm=900'
    fi
    ;;
  *" power startup get "*)
    if [ "${TEST_STARTUP_FAIL:-0}" = 1 ]; then
      echo 'error: controller owner is active' >&2
      exit 1
    fi
    if [ "$UGREEN_PRODUCT_NAME" = "DXP4800 GT" ]; then echo unknown;
    elif [ -n "${TEST_STARTUP_STATE:-}" ]; then cat "$TEST_STARTUP_STATE"; else echo restore; fi
    ;;
  *" power startup set "*)
    if [ "${TEST_STARTUP_FAIL:-0}" = 1 ]; then
      echo 'error: simulated readback failure' >&2
      exit 1
    fi
    if [ -n "${TEST_STARTUP_STATE:-}" ]; then
      for policy in "$@"; do :; done
      echo "$policy" > "$TEST_STARTUP_STATE"
    fi
    ;;
  *" network wol interfaces "*)
    if [ "$UGREEN_PRODUCT_NAME" = 'DXP4800 GT' ]; then
      printf '%s\n' enp11s0 enp6s0
    else
      echo 'enp2s0'
    fi
    ;;
  *" network wol get "*)
    if [ "$UGREEN_PRODUCT_NAME" = 'DXP4800 GT' ]; then
      case " $* " in
        *" --interface enp6s0 "*|*" --interface enp11s0 "*) cat "$TEST_GT_WOL_STATE" ;;
        *) echo 'error: explicit physical Ethernet interface required' >&2; exit 1 ;;
      esac
    else
      echo 'on'
    fi
    ;;
  *" network wol set "*)
    if [ "$UGREEN_PRODUCT_NAME" = DXP2800 ]; then
      case " $* " in
        *" --interface enp2s0 "*) : ;;
        *) echo 'error: not a physical Ethernet interface' >&2; exit 1 ;;
      esac
    fi
    if [ "$UGREEN_PRODUCT_NAME" = 'DXP4800 GT' ]; then
      case " $* " in
        *" --interface enp6s0 "*|*" --interface enp11s0 "*)
          case " $* " in
            *" wol set on "*) printf 'on\n' ;;
            *" wol set off "*) printf 'off\n' ;;
          esac > "$TEST_GT_WOL_STATE"
          ;;
        *) echo 'error: not a physical Ethernet interface' >&2; exit 1 ;;
      esac
    fi
    ;;
  *" power rtc-wake get "*) echo '0' ;;
  *" power rtc-wake set "*) : ;;
  *" power rtc-wake clear "*) : ;;
esac
EOF
chmod +x "$BIN"

export TRIM_APPDEST="$ROOT/App.Native.UGreenLED/app" TRIM_PKGVAR="$TMP/var" UGREEN_RUNTIME_DIR="$TMP/run"
export UGREEN_PRODUCT_NAME="DXP4800S" BIOS_UGREENCTL="$BIN" BIOS_UGREENCTL_PLUGIN_DIR="$PLUGINS" UGREENCTL_CALLS="$CALLS" BIOS_SCHEDULE_CRON_FILE="$TMP/cron.d/power"
request() {
  local path="$1" query="${2:-}" method="${3:-GET}"
  PATH_INFO="$path" QUERY_STRING="$query" REQUEST_METHOD="$method" CONTENT_LENGTH=0 bash "$API" | tail -n 1
}

if [[ "${TEST_MODEL_ONLY:-}" != "dxp4800gt" ]]; then
json=$(request /bios/status)
assert_json "$json"
assert_contains "$json" '"available":true'
assert_contains "$json" '"min_pwm":40'
assert_contains "$json" '"sys_rpm":900'
assert_contains "$json" '"fan_mode_writable":false'

: > "$CALLS"
json=$(request /bios/fan 'channel=sys&pwm=39&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":false'
[[ ! -s "$CALLS" ]] || fail "PWM below 40 reached ugreenctl"

json=$(request /bios/fan 'channel=sys&pwm=40&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <fan> <set> <sys> <40>' "$CALLS" || fail "fan write did not use --force --apply"

: > "$CALLS"
json=$(request /bios/fan/mode 'channel=sys&mode=auto&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":false'
assert_contains "$json" '暂时只开放手动 PWM'
[[ ! -s "$CALLS" ]] || fail "fan mode write reached ugreenctl"

export UGREEN_PRODUCT_NAME="DX4600"
json=$(request /bios/status)
assert_json "$json"
assert_contains "$json" '"model":"dx4600"'
assert_contains "$json" '"available":true'
assert_contains "$json" '"startup_available":true'
assert_contains "$json" '"wol_available":true'
assert_contains "$json" '"cpu_fan_present":false'
assert_contains "$json" '"fan_write_target":"sys"'
assert_contains "$json" '"write_confirmation_required":true'
assert_contains "$json" '"profile":"stock-4600"'

: > "$CALLS"
json=$(request /bios/fan 'channel=sys&pwm=64' POST)
assert_contains "$json" '"ok":false'
assert_contains "$json" '固件逆向风扇写入需要先确认风险'
[[ ! -s "$CALLS" ]] || fail "unconfirmed DX4600 PWM reached ugreenctl"
json=$(request /bios/fan 'channel=sys&pwm=64&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <fan> <set> <sys> <64>' "$CALLS" || fail "DX4600 fan write did not use --force --apply"

: > "$CALLS"
json=$(request /bios/startup 'policy=last&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fxq 'startup_selection=DX4600|last' "$TRIM_PKGVAR/settings.conf" || fail "DX4600 startup choice was not persisted by API"
grep -Fq ' <--force> <--apply> <power> <startup> <set> <restore>' "$CALLS" || fail "DX4600 startup write did not use --force --apply"
json=$(request /bios/wol 'policy=off&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <network> <wol> <set> <off>' "$CALLS" || fail "DX4600 WOL write did not use --force --apply"

# Unknown is a successful read, not a missing capability. Recover through the
# same API as the page and persist only a successful guarded hardware write.
export TEST_STARTUP_STATE="$TMP/startup-state"
echo unknown > "$TEST_STARTUP_STATE"
json=$(request /bios/status)
assert_json "$json"
assert_contains "$json" '"startup":"unknown"'
assert_contains "$json" '"startup_available":true'
assert_contains "$json" '"startup_error":""'
json=$(request /bios/startup 'policy=unknown&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":false'
json=$(request /bios/startup 'policy=on&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
assert_contains "$json" '"startup":"on"'
grep -Fxq 'startup_selection=DX4600|on' "$TRIM_PKGVAR/settings.conf" || fail "recovered policy not persisted"

export TEST_STARTUP_FAIL=1
json=$(request /bios/status)
assert_contains "$json" '"startup_available":false'
assert_contains "$json" '"startup":"unknown"'
assert_contains "$json" 'controller owner is active'
json=$(request /bios/startup 'policy=off&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":false'
grep -Fxq 'startup_selection=DX4600|on' "$TRIM_PKGVAR/settings.conf" || fail "failed write replaced saved policy"
unset TEST_STARTUP_FAIL
echo unexpected > "$TEST_STARTUP_STATE"
json=$(request /bios/status)
assert_contains "$json" '"startup_available":false'
: > "$TEST_STARTUP_STATE"
json=$(request /bios/status)
assert_contains "$json" '"startup_available":false'
unset TEST_STARTUP_STATE

export UGREEN_PRODUCT_NAME="DXP4800"
json=$(request /bios/status)
assert_json "$json"
assert_contains "$json" '"model":"dxp4800"'
assert_contains "$json" '"wol_available":true'
assert_contains "$json" '"wol":"on"'
assert_contains "$json" '"power_schedule":{"available":true'
assert_contains "$json" '"cpu_fan_present":false'

: > "$CALLS"
json=$(request /bios/wol 'policy=off' POST)
assert_contains "$json" '"ok":false'
assert_contains "$json" '固件逆向网络唤醒写入需要先在页面确认风险'
[[ ! -s "$CALLS" ]] || fail "unconfirmed DXP4800 WOL write reached ugreenctl"

json=$(request /bios/confirmation 'confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
assert_contains "$json" '"write_confirmation_acknowledged":true'
grep -Fq 'write_risk_acknowledgement=v2:firmware-reversed:DXP4800' "$TMP/var/settings.conf" \
  || fail "BIOS risk acknowledgement was not saved per model"
json=$(request /bios/status)
assert_contains "$json" '"write_confirmation_acknowledged":true'
: > "$CALLS"
json=$(request /bios/wol 'policy=off' POST)
assert_contains "$json" '"ok":false'
[[ ! -s "$CALLS" ]] || fail "saved acknowledgement bypassed the per-write confirmation token"

json=$(request /bios/wol 'policy=off&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <network> <wol> <set> <off>' "$CALLS" || fail "DXP4800 WOL write did not use --force --apply"

: > "$CALLS"
json=$(request /bios/power-schedule 'enabled=true&days=1,2,3,4,5&wake_time=07%3A00&shutdown_time=23%3A00&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
assert_contains "$json" '"power_schedule":{"available":true,"enabled":true'
grep -Fq ' <--force> <--apply> <power> <rtc-wake> <set> ' "$CALLS" || fail "scheduled wake did not arm RTC"
[[ -f "$BIOS_SCHEDULE_CRON_FILE" ]] || fail "scheduled shutdown cron was not written"
grep -Fq '0 23 * * 1,2,3,4,5 root' "$BIOS_SCHEDULE_CRON_FILE" || fail "scheduled shutdown cron has wrong calendar"
grep -Fq 'scheduled_power.sh shutdown' "$BIOS_SCHEDULE_CRON_FILE" || fail "scheduled shutdown cron has wrong command"

export UGREEN_PRODUCT_NAME="DXP6800 Pro"
json=$(request /bios/status)
assert_contains "$json" '"model":"dxp6800pro"'
assert_contains "$json" '"cpu_rpm":1000'
assert_contains "$json" '"sys_rpm":900'
assert_contains "$json" '"sys2_rpm":1500'
assert_contains "$json" '"write_confirmation_required":true'
assert_contains "$json" '"write_confirmation_acknowledged":false'

: > "$CALLS"
json=$(request /bios/fan 'channel=sys&pwm=120' POST)
assert_contains "$json" '"ok":false'
assert_contains "$json" '固件逆向风扇写入需要先确认风险'
[[ ! -s "$CALLS" ]] || fail "unconfirmed DXP6800 Pro PWM reached ugreenctl"

json=$(request /bios/fan 'channel=sys&pwm=120&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <fan> <set> <sys> <120>' "$CALLS" || fail "DXP6800 Pro paired fan write did not use --force --apply"

: > "$CALLS"
json=$(request /bios/fan 'channel=sys1&pwm=120&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":false'
[[ ! -s "$CALLS" ]] || fail "DXP6800 Pro individual system fan write reached ugreenctl"

json=$(request /bios/startup 'policy=last&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <power> <startup> <set> <restore>' "$CALLS" || fail "DXP6800 Pro startup write did not use --force --apply"

export UGREEN_PRODUCT_NAME="DXP8800 Pro"
json=$(request /bios/status)
assert_contains "$json" '"model":"dxp8800pro"'
assert_contains "$json" '"cpu_rpm":1000'
assert_contains "$json" '"sys_rpm":900'
assert_contains "$json" '"sys2_rpm":1500'
assert_contains "$json" '"write_confirmation_required":true'
assert_contains "$json" '"write_confirmation_acknowledged":false'

: > "$CALLS"
json=$(request /bios/fan 'channel=sys&pwm=120' POST)
assert_contains "$json" '"ok":false'
assert_contains "$json" '固件逆向风扇写入需要先确认风险'
[[ ! -s "$CALLS" ]] || fail "unconfirmed DXP8800 Pro PWM reached ugreenctl"

json=$(request /bios/fan 'channel=sys&pwm=120&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <fan> <set> <sys> <120>' "$CALLS" || fail "DXP8800 Pro paired fan write did not use --force --apply"

: > "$CALLS"
json=$(request /bios/fan 'channel=sys1&pwm=120&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":false'
[[ ! -s "$CALLS" ]] || fail "DXP8800 Pro individual system fan write reached ugreenctl"

json=$(request /bios/startup 'policy=last&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <power> <startup> <set> <restore>' "$CALLS" || fail "DXP8800 Pro startup write did not use --force --apply"

export UGREEN_PRODUCT_NAME="DXP6800"
json=$(request /bios/status)
assert_contains "$json" '"model":"dxp6800"'
assert_contains "$json" '"cpu_rpm":1000'
assert_contains "$json" '"sys_rpm":900'
assert_contains "$json" '"sys2_rpm":1500'
assert_contains "$json" '"write_confirmation_required":true'
assert_contains "$json" '"write_confirmation_acknowledged":false'

: > "$CALLS"
json=$(request /bios/fan 'channel=sys&pwm=120' POST)
assert_contains "$json" '"ok":false'
assert_contains "$json" '固件逆向风扇写入需要先确认风险'
[[ ! -s "$CALLS" ]] || fail "unconfirmed DXP6800 PWM reached ugreenctl"

json=$(request /bios/fan 'channel=sys&pwm=120&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <fan> <set> <sys> <120>' "$CALLS" || fail "DXP6800 paired fan write did not use --force --apply"

: > "$CALLS"
json=$(request /bios/fan 'channel=sys1&pwm=120&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":false'
[[ ! -s "$CALLS" ]] || fail "DXP6800 individual system fan write reached ugreenctl"

json=$(request /bios/startup 'policy=last&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <power> <startup> <set> <restore>' "$CALLS" || fail "DXP6800 startup write did not use --force --apply"

export UGREEN_PRODUCT_NAME="DXP4800 Pro"
json=$(request /bios/status)
assert_json "$json"
assert_contains "$json" '"model":"dxp4800_pro"'
assert_contains "$json" '"startup_available":true'
assert_contains "$json" '"write_confirmation_required":true'

: > "$CALLS"
json=$(request /bios/startup 'policy=last&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <power> <startup> <set> <restore>' "$CALLS" || fail "DXP4800 Pro startup write did not use --force --apply"

# All currently supported exact products use the same persistent API contract.
for UGREEN_PRODUCT_NAME in 'DX4600' 'DX4600+' 'DX4600 Pro' 'DXP4800' \
    'DXP4800 Plus' 'UGREEN DXP4800 Plus' 'DXP4800 Pro' 'UGREEN DXP4800 Pro' \
    'DXP4800S' 'DXP480T Plus' 'UGREEN DXP480T Plus' 'DXP6800 Plus' 'DXP6800 Pro' 'DXP6800'; do
    : > "$CALLS"
    json=$(request /bios/startup 'policy=off&confirm=firmware-reversed' POST)
    assert_json "$json"
    assert_contains "$json" '"ok":true'
    grep -Fxq "startup_selection=$UGREEN_PRODUCT_NAME|off" "$TRIM_PKGVAR/settings.conf" \
        || fail "$UGREEN_PRODUCT_NAME startup choice was not persisted by API"
    grep -Fq ' <power> <startup> <set> <off>' "$CALLS" || fail "$UGREEN_PRODUCT_NAME startup write missing"
done

export UGREEN_PRODUCT_NAME="DXP2800" UGREEN_PRODUCT_VERSION="EM_DXP2800_V1.0.26"
json=$(request /bios/status)
assert_json "$json"
assert_contains "$json" '"model":"dxp2800"'
assert_contains "$json" '"available":true'
assert_contains "$json" '"startup_available":true'
assert_contains "$json" '"write_confirmation_required":true'
: > "$CALLS"
json=$(request /bios/fan 'channel=sys&pwm=128' POST)
assert_contains "$json" '"ok":false'
[[ ! -s "$CALLS" ]] || fail "unconfirmed EM26 fan write reached backend"
json=$(request /bios/fan 'channel=sys&pwm=128&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <fan> <set> <sys> <128>' "$CALLS" || fail "EM26 guarded PWM missing"
for query in 'channel=cpu&pwm=128' 'channel=sys&pwm=39'; do
    : > "$CALLS"
    json=$(request /bios/fan "$query&confirm=firmware-reversed" POST)
    assert_contains "$json" '"ok":false'
    [[ ! -s "$CALLS" ]] || fail "invalid EM26 write reached backend"
done
: > "$CALLS"
json=$(request /bios/startup 'policy=on&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <power> <startup> <set> <on>' "$CALLS" || fail "EM26 guarded AC recovery missing"
json=$(request /bios/status)
assert_contains "$json" '"wol_interface_required":true'
assert_contains "$json" '"wol_interfaces":["enp2s0"]'
assert_contains "$json" '"wol_available":false'
: > "$CALLS"
json=$(request /bios/wol 'policy=on&interface=enp2s0' POST)
assert_contains "$json" '"ok":false'
[[ ! -s "$CALLS" ]] || fail "unconfirmed EM26 WOL reached backend"
json=$(request /bios/wol 'policy=on&interface=br0&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":false'
! grep -Fq 'wol_interface=br0' "$TRIM_PKGVAR/settings.conf" || fail "failed WOL selection was saved"
: > "$CALLS"
json=$(request /bios/wol 'policy=on&interface=enp2s0&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
assert_contains "$json" '"wol_available":true'
assert_contains "$json" '"wol_interface":"enp2s0"'
grep -Fq ' <--force> <--apply> <network> <wol> <set> <on> <--interface> <enp2s0>' "$CALLS" || fail "EM26 explicit WOL missing"
: > "$CALLS"
json=$(request /bios/power-schedule 'enabled=true&days=1,2,3,4,5&wake_time=07%3A00&shutdown_time=23%3A00&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
assert_contains "$json" '"power_schedule":{"available":true,"enabled":true'
grep -Fq ' <--force> <--apply> <power> <rtc-wake> <set> ' "$CALLS" || fail "EM26 RTC was not armed"
json=$(request /bios/power-schedule 'enabled=false&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <power> <rtc-wake> <clear>' "$CALLS" || fail "EM26 RTC was not cleared"

export UGREEN_PRODUCT_VERSION="DXP2800 V1.2 251215"
json=$(request /bios/status)
assert_contains "$json" '"supported":false'
unset UGREEN_PRODUCT_VERSION

fi

export UGREEN_PRODUCT_NAME="DXP4800 GT" UGREEN_PRODUCT_VERSION="DXP4800 GT V1.0 20260429"
export TEST_GT_WOL_STATE="$TMP/gt-wol-state"
printf 'off\n' > "$TEST_GT_WOL_STATE"

json=$(request /bios/status)
assert_json "$json"
assert_contains "$json" '"wol_interface_required":true'
assert_contains "$json" '"wol_interfaces":["enp11s0","enp6s0"]'
assert_contains "$json" '"wol_available":false'
assert_contains "$json" '请选择用于网络唤醒的物理有线网卡'
: > "$CALLS"
json=$(request /bios/wol 'policy=on&interface=enp6s0' POST)
assert_contains "$json" '"ok":false'
[[ ! -s "$CALLS" ]] || fail "unconfirmed GT WOL reached backend"

json=$(request /bios/confirmation 'confirm=firmware-reversed' POST)
assert_contains "$json" '"write_confirmation_acknowledged":true'
grep -Fxq 'write_risk_acknowledgement=v2:firmware-reversed:DXP4800 GT' "$TRIM_PKGVAR/settings.conf" || fail 'GT confirmation was not persisted'
json=$(request /bios/status)
assert_json "$json"
assert_contains "$json" '"model":"dxp4800gt"'
assert_contains "$json" '"max_pwm":198'
assert_contains "$json" '"startup":"unknown"'
assert_contains "$json" '"startup_readback":false'
assert_contains "$json" '"startup_restore_supported":false'
assert_contains "$json" '"write_confirmation_required":true'
: > "$CALLS"
json=$(request /bios/fan 'channel=cpu&pwm=198&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <fan> <set> <cpu> <198>' "$CALLS" || fail "GT guarded PWM missing"
: > "$CALLS"
json=$(request /bios/fan 'channel=sys&pwm=199&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":false'
[[ ! -s "$CALLS" ]] || fail "out-of-range GT PWM reached backend"
json=$(request /bios/startup 'policy=last&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":false'
[[ ! -s "$CALLS" ]] || fail "GT restore reached backend"
json=$(request /bios/startup 'policy=on&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
grep -Fq ' <--force> <--apply> <power> <startup> <set> <on>' "$CALLS" || fail "GT guarded startup missing"

: > "$CALLS"
json=$(request /bios/wol 'policy=on&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":false'
[[ ! -s "$CALLS" ]] || fail "GT WOL reached backend without interface selection"
json=$(request /bios/wol 'policy=on&interface=br0&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":false'
assert_contains "$json" 'not a physical Ethernet interface'
! grep -Fq 'wol_interface=br0' "$TRIM_PKGVAR/settings.conf" || fail "rejected GT interface was saved"
json=$(request /bios/wol 'policy=on&interface=enp6s0&confirm=firmware-reversed' POST)
assert_contains "$json" '"ok":true'
assert_contains "$json" '"wol_available":true'
assert_contains "$json" '"wol":"on"'
assert_contains "$json" '"wol_interface":"enp6s0"'
grep -Fq ' <--force> <--apply> <network> <wol> <set> <on> <--interface> <enp6s0>' "$CALLS" || fail "GT explicit WOL write missing"
grep -Fq ' <network> <wol> <get> <--interface> <enp6s0>' "$CALLS" || fail "GT explicit WOL readback missing"
grep -Fxq 'wol_selection=dxp4800gt:DXP4800 GT:DXP4800 GT V1.0 20260429|on|enp6s0' "$TRIM_PKGVAR/settings.conf" || fail "GT WOL selection was not persisted"
json=$(request /bios/wol 'policy=off&confirm=firmware-reversed' POST)
assert_contains "$json" '"wol":"off"'
grep -Fxq 'wol_selection=dxp4800gt:DXP4800 GT:DXP4800 GT V1.0 20260429|off|enp6s0' "$TRIM_PKGVAR/settings.conf" || fail "GT WOL off did not retain the selected port"

echo "BIOS API tests passed"
