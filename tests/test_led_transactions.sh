#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
export FIXTURE="$TMP" UGREEN_CLI="$TMP/cli" LED_API_CACHE_DIR="$TMP/cache"
cat > "$UGREEN_CLI" <<'EOF'
#!/bin/bash
set -eu
mkdir "$FIXTURE/active" 2>/dev/null || { echo collision >> "$FIXTURE/collisions"; exit 5; }
trap 'rmdir "$FIXTURE/active"' EXIT
echo "$*" >> "$FIXTURE/calls"
sleep 0.03
if [[ "$*" == *-status* ]]; then
    echo 'disk1: status = on, brightness = 64, color = RGB(1, 2, 3)'
    exit 0
fi
if [[ -e "$FIXTURE/fail" ]]; then
    echo 'error [hardware_error]: state may have changed (1 operations completed before failure)'
    exit 5
fi
if [[ "$2" == -color ]]; then
    printf '%s,%s,%s,%s\n' "$3" "$4" "$5" "$7" > "$FIXTURE/state"
fi
EOF
chmod +x "$UGREEN_CLI"
source "$ROOT/App.Native.UGreenLED/app/server/lib/led_api.sh"
led_power26_profile() { return 1; }

led_set_color disk1 1 2 3 64
led_write_cached_state disk2 'unaffected'
touch "$TMP/fail"
if led_set_color disk1 9 8 7 64; then fail 'partial write reported success'; fi
[[ ! -e "$LED_API_CACHE_DIR/disk1.state" ]] || fail 'partial write retained old cache'
[[ $(led_read_cached_state disk2) == unaffected ]] || fail 'unrelated cache was removed'
before=$(wc -l < "$TMP/calls")
if led_cli_run disk2 -off; then fail 'controller cooldown ignored'; fi
[[ $(wc -l < "$TMP/calls") == "$before" ]] || fail 'cooldown still called CLI'
led_cli_run disk1 -status >/dev/null
[[ $(wc -l < "$TMP/calls") == "$((before + 1))" ]] || fail 'cooldown blocked diagnostics'
inode=$(stat -c %i "$LED_API_CACHE_DIR/.operation.lock")
led_clear_cache
[[ $(stat -c %i "$LED_API_CACHE_DIR/.operation.lock") == "$inode" ]] || fail 'cache reset replaced lock inode'
[[ -e "$LED_API_CACHE_DIR/.write-retry-at" ]] || fail 'cache reset bypassed cooldown'
rm "$TMP/fail"
printf '0\n' > "$LED_API_CACHE_DIR/.write-retry-at"
before=$(wc -l < "$TMP/calls")
led_set_color disk1 1 2 3 64
[[ $(wc -l < "$TMP/calls") -gt "$before" ]] || fail 'old state was skipped after partial write'

# Delay cache commit to expose CLI/cache ordering races between API and daemon.
led_write_cached_state() {
    sleep 0.05
    printf '%s\n' "$2" > "$(_cache_file "$1")"
}
for value in 10 30; do
    pids=()
    (led_set_color disk1 "$value" 2 3 64) & pids+=("$!")
    (led_set_color disk1 "$((value + 10))" 2 3 64) & pids+=("$!")
    (led_cli_run disk1 -status >/dev/null) & pids+=("$!")
    for pid in "${pids[@]}"; do wait "$pid" || fail 'parallel operation failed'; done
done
[[ ! -e "$TMP/collisions" ]] || fail 'CLI calls overlapped'
[[ $(led_read_cached_state disk1) == "$(cat "$TMP/state")" ]] || fail 'cache commit reordered relative to CLI'
before=$(wc -l < "$TMP/calls")
IFS=, read -r r g b brightness < "$TMP/state"
led_set_color disk1 "$r" "$g" "$b" "$brightness"
[[ $(wc -l < "$TMP/calls") == "$before" ]] || fail 'valid cache no longer deduplicates'
echo 'LED partial failure, cooldown, lock lifetime and concurrent cache transactions passed'

# Explicit startup initialization uses the same CLI authorization and app lock;
# it clears all cached LEDs because the MCU animation may affect every output.
hardware_profile_key() { echo dxp4800_plus; }
before=$(wc -l < "$TMP/calls")
led_initialize_controller
[[ $(wc -l < "$TMP/calls") == "$before" ]] || fail 'initialized an unrelated model'
hardware_profile_key() { echo idx6012; }
led_write_cached_state disk2 old
led_initialize_controller
[[ $(tail -n 1 "$TMP/calls") == 'power -initialize-idx6012 --force --apply' ]] || fail 'missing init authorization'
[[ ! -e "$LED_API_CACHE_DIR/disk2.state" ]] || fail 'initialization retained stale output cache'
touch "$TMP/fail"
if led_initialize_controller; then fail 'initialization failure reported success'; fi
echo 'iDX6012 initialization adapter, cache invalidation and failure propagation passed'
