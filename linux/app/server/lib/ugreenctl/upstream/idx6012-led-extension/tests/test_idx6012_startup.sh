#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# Exercise the real daemon loop with all hardware/settings calls simulated.
source <(sed -n '/^daemon_loop() {/,/^}/p' "$ROOT/App.Native.UGreenLED/app/server/led_daemon.sh")
ugreen_log_info() { :; }
log_limited() { :; }
ensure_mapping() { :; }
ensure_led_backend() { return 0; }
led_cli_version() { echo fixture; }
led_backend_name() { echo cli; }
led_backend_configured() { echo cli; }
hardware_detected_product_name() { echo iDX6012; }
hardware_profile_key() { echo idx6012; }
hardware_write_protocol() { echo upstream-managed; }
settings_get() {
    if [[ "$2 $3" == 'daemon enabled' ]]; then
        if (( ticks==0 )); then echo false; else echo true; fi
    elif [[ "$2 $3" == 'daemon check_interval' ]]; then echo 1
    else echo on; fi
}
led_initialize_controller() {
    ((init_calls+=1))
    # First enabled iteration fails; normal output must not run until retry.
    ((init_calls>1))
}
tick_mode_on() {
    ((init_calls==2)) || { echo 'write before successful initialization' >&2; exit 1; }
    ((writes+=1))
}
sleep() { ((ticks+=1)); if ((ticks>=4)); then DAEMON_RUN=false; fi; }
ticks=0 init_calls=0 writes=0
DAEMON_RUN=true LAST_ENABLED='' LAST_MODE='' SETTINGS_FILE=fixture RUNTIME_DIR=fixture PID_FILE=''
daemon_loop
[[ "$init_calls $writes" == '2 2' ]] || { echo "unexpected init/write count: $init_calls $writes"; exit 1; }
echo 'Disabled daemon stays read-only; init failure blocks output; successful init runs once'
