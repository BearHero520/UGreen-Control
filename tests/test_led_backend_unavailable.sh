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
printf '%s\n' 'error [unsupported]: expected LED class node power is absent; the required hardware driver is unavailable' >&2
exit 3
EOF
chmod +x "$MOCK_CLI"

export UGREEN_CLI="$MOCK_CLI" CLI_LOG
LED_API_CACHE_DIR="$TMP/cache"
unset LED_BACKEND_UNAVAILABLE_TTL
mkdir -p "$LED_API_CACHE_DIR"

if led_backend_select; then
    echo 'FAIL: missing DH2600 N76 driver was reported as available' >&2
    exit 1
fi
if led_backend_select; then
    echo 'FAIL: backend cooldown did not keep the failed probe unavailable' >&2
    exit 1
fi
[[ "$(wc -l < "$CLI_LOG")" -eq 1 ]] || {
    echo 'FAIL: repeated backend selection re-ran the failing CLI probe' >&2
    exit 1
}
[[ -f "$LED_API_CACHE_DIR/.backend-unavailable" ]] || {
    echo 'FAIL: backend cooldown marker was not persisted' >&2
    exit 1
}
grep -Fxq disabled "$LED_API_CACHE_DIR/.backend-unavailable" || {
    echo 'FAIL: unavailable backend was not persistently disabled' >&2
    exit 1
}

led_backend_reset
if led_backend_select; then
    echo 'FAIL: reset backend still reported the failing probe as available' >&2
    exit 1
fi
[[ "$(wc -l < "$CLI_LOG")" -eq 2 ]] || {
    echo 'FAIL: backend reset did not allow a fresh probe' >&2
    exit 1
}

echo 'N76 backend cooldown test passed'
