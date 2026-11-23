#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIB="$ROOT/App.Native.UGreenLED/app/server/lib"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

SETTINGS_FILE="$TMP/settings.conf"
source "$LIB/settings.sh"
settings_init "$SETTINGS_FILE"
source "$LIB/hardware_profile.sh"
source "$LIB/led_api.sh"

mkdir -p "$TMP/bin"
cat > "$TMP/bin/flock" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$TMP/bin/flock"
export PATH="$TMP/bin:$PATH"

settings_set "$SETTINGS_FILE" hardware profile dh2600
CLI_LOG="$TMP/cli.log"
MOCK_CLI="$TMP/ugreen_leds_cli"
cat > "$MOCK_CLI" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$CLI_LOG"
case "$*" in
    *'power -status --n76-direct 4:0x31'*)
        printf '%s\n' 'power: mode=off brightness=unknown color=unknown timing=unknown'
        exit 0
        ;;
    *'disk2 -color 0 255 0 -blink 250 250 -brightness 128 --n76-direct 4:0x31 --force --apply'*)
        printf '%s\n' 'ok'
        exit 0
        ;;
esac
printf '%s\n' 'unexpected mock command' >&2
exit 3
EOF
chmod +x "$MOCK_CLI"

export UGREEN_CLI="$MOCK_CLI" CLI_LOG UGREEN_N76_BUS_CANDIDATES=4
LED_API_CACHE_DIR="$TMP/cache"
mkdir -p "$LED_API_CACHE_DIR"

led_backend_select >/dev/null || {
    echo 'FAIL: DH2600 did not select the explicit N76 direct backend' >&2
    exit 1
}
[[ "$LED_N76_DIRECT_SPEC" == 4:0x31 ]] || {
    echo 'FAIL: N76 direct selector did not retain the verified candidate' >&2
    exit 1
}
led_set_blink disk1 0 255 0 500 250 128 >/dev/null || {
    echo 'FAIL: DH2600 disk blink was not sent through the direct backend' >&2
    exit 1
}
grep -Fq -- 'disk2 -color 0 255 0 -blink 250 250 -brightness 128 --n76-direct 4:0x31 --force --apply' "$CLI_LOG" || {
    echo 'FAIL: direct N76 disk command lost its selector or write acknowledgement' >&2
    exit 1
}
[[ -f "$LED_API_CACHE_DIR/.n76-direct" ]] || {
    echo 'FAIL: direct N76 selector was not persisted for CGI requests' >&2
    exit 1
}

echo 'DH2600 N76 direct test passed'
