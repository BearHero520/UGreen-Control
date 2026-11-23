#!/bin/bash
# LED adapter.  Hardware access is intentionally delegated to the bundled
# ugreen_leds_cli; this file must not write sysfs, I2C, Super-I/O, or MCU nodes.

UGREEN_CLI="${UGREEN_CLI:-}"
LED_API_CACHE_DIR="${LED_API_CACHE_DIR:-/tmp/ugreen-led-api}"
LED_BACKEND_ACTIVE="${LED_BACKEND_ACTIVE:-}"
LED_LAST_ERROR="${LED_LAST_ERROR:-}"
LED_N76_DIRECT_SPEC="${LED_N76_DIRECT_SPEC:-}"
# A failed N76 preflight stays disabled until the service is restarted or an
# explicit backend reset is requested. This avoids retrying a missing driver
# every minute while keeping a deliberate recovery path.
LED_BACKEND_UNAVAILABLE_TTL="${LED_BACKEND_UNAVAILABLE_TTL:-0}"

mkdir -p "$LED_API_CACHE_DIR" 2>/dev/null || true

# Serialize application transactions (cache check, CLI and cache commit), not
# hardware access. The CLI retains its own controller lock and owner checks.
# Keep this inode across cache resets; unlinking it would split the lock.
_led_serialized() {
    if [[ "${_LED_APP_LOCK_HELD:-0}" == 1 ]]; then "$@"; return $?; fi
    local fd rc _LED_APP_LOCK_HELD=1
    if ! command -v flock >/dev/null 2>&1; then
        led_set_error "flock is required to serialize LED application operations"
        return 1
    fi
    if ! exec {fd}>"$LED_API_CACHE_DIR/.operation.lock"; then
        led_set_error "cannot open LED application lock"
        return 1
    fi
    if flock -w 6 "$fd"; then
        if "$@"; then rc=0; else rc=$?; fi
    else
        led_set_error "LED application operation is busy"
        rc=75
    fi
    exec {fd}>&-
    return "$rc"
}

_led_log() {
    if declare -F ugreen_log_debug >/dev/null; then
        ugreen_log_debug "led.detail" "$*"
    elif [[ "${LED_DEBUG:-false}" == "true" ]]; then
        printf '[led_api] %s\n' "$*" >&2
    fi
}

_led_log_failure() {
    local event="$1" message="$2"
    shift 2
    if declare -F ugreen_log_rate_limited >/dev/null; then
        ugreen_log_rate_limited "${event}:${1:-general}" 30 ERROR "$event" "$message" "$@"
    elif declare -F ugreen_log_error >/dev/null; then
        ugreen_log_error "$event" "$message" "$@"
    fi
}

led_set_error() {
    LED_LAST_ERROR="$*"
    _led_log "$LED_LAST_ERROR"
}

led_write_suppressed() {
    [[ "${LED_BACKEND_SKIP_WRITES:-false}" == true ]]
}

led_cli_available() {
    [[ -n "$UGREEN_CLI" && -x "$UGREEN_CLI" ]]
}

led_cli_version() {
    led_cli_available || { printf 'unavailable\n'; return; }
    printf '%s\n' "${UGREEN_CLI_RELEASE:-local-dxp2800-em26-20261101}"
}

ensure_cli() {
    if ! led_cli_available; then
        led_set_error "ugreen_leds_cli is missing or not executable: ${UGREEN_CLI:-<empty>}"
        _led_log_failure "led.cli_unavailable" "bundled LED CLI is unavailable" "cli=${UGREEN_CLI:-missing}"
        return 1
    fi
}

_led_backend_unavailable_file() {
    printf '%s/.backend-unavailable\n' "$LED_API_CACHE_DIR"
}

_led_backend_direct_file() {
    printf '%s/.n76-direct\n' "$LED_API_CACHE_DIR"
}

_led_backend_read_unavailable() {
    local file retry message now
    file=$(_led_backend_unavailable_file)
    [[ -f "$file" ]] || return 1
    read -r retry < "$file" || retry=0
    now=$(date +%s)
    if [[ "$retry" == disabled ]]; then
        message=$(sed -n '2p' "$file" 2>/dev/null || true)
        LED_LAST_ERROR="${message:-LED 控制后端暂不可用，等待显式重试}"
        return 0
    fi
    if [[ "$retry" =~ ^[0-9]+$ ]] && (( retry > now )); then
        message=$(sed -n '2p' "$file" 2>/dev/null || true)
        LED_LAST_ERROR="${message:-LED 控制后端暂不可用，正在等待下一次预检}"
        return 0
    fi
    rm -f "$file" 2>/dev/null || true
    return 1
}

_led_backend_mark_unavailable() {
    local message="$1" ttl="${LED_BACKEND_UNAVAILABLE_TTL:-0}" now file tmp retry
    [[ "$ttl" =~ ^[0-9]+$ ]] || ttl=0
    retry=disabled
    if (( ttl > 0 )); then
        now=$(date +%s)
        retry=$((now + ttl))
    fi
    file=$(_led_backend_unavailable_file)
    tmp="${file}.tmp.${BASHPID:-$$}"
    printf '%s\n%s\n' "$retry" "$message" > "$tmp" && mv -f "$tmp" "$file"
    LED_LAST_ERROR="$message"
}

_led_backend_clear_unavailable() {
    rm -f "$(_led_backend_unavailable_file)" 2>/dev/null || true
}

_led_backend_read_direct() {
    local file=$(_led_backend_direct_file) spec
    [[ -r "$file" ]] || return 1
    read -r spec < "$file" || return 1
    [[ "$spec" =~ ^[0-9]+:0x(31|26)$ ]] || return 1
    LED_N76_DIRECT_SPEC="$spec"
    return 0
}

_led_backend_write_direct() {
    local file=$(_led_backend_direct_file) tmp
    tmp="${file}.tmp.${BASHPID:-$$}"
    if ! printf '%s\n' "$LED_N76_DIRECT_SPEC" > "$tmp" || ! mv "$tmp" "$file"; then
        rm -f "$tmp" 2>/dev/null || true
        return 1
    fi
}

_led_backend_clear_direct() {
    LED_N76_DIRECT_SPEC=""
    rm -f "$(_led_backend_direct_file)" 2>/dev/null || true
}

led_cli_name() {
    if declare -F hardware_cli_led_name >/dev/null; then
        hardware_cli_led_name "$1"
    else
        printf '%s\n' "$1"
    fi
}

led_cli_run() {
    local logical="${1:-}" raw
    shift
    [[ -n "$logical" ]] || {
        led_set_error "LED target is empty"
        return 2
    }
    raw=$(led_cli_name "$logical")
    [[ -n "$raw" ]] || {
        led_set_error "LED target mapping is empty: $logical"
        return 2
    }
    _led_cli_run_raw "$logical" "$raw" "$@"
}

# Shared CLI execution for single LEDs and a batch of already mapped names.
_led_cli_run_raw() {
    _led_serialized _led_cli_run_raw_unlocked "$@"
}

_led_cli_run_raw_unlocked() {
    local logical="$1" raw="$2" protocol output rc action arg now retry_at=0
    local -a access=() direct_args=()
    shift 2
    ensure_cli || return 1
    protocol="upstream-managed"
    if [[ -n "$LED_N76_DIRECT_SPEC" ]]; then
        direct_args=(--n76-direct "$LED_N76_DIRECT_SPEC")
        protocol="n76-direct"
    fi
    for arg in "$@"; do
        case "$arg" in -on|-off|-color|-brightness|-blink|-breath|-initialize-idx6012) access=(--force --apply); break ;; esac
    done
    action="$*"
    if (( ${#access[@]} )); then
        now=$(date +%s)
        if [[ -f "$LED_API_CACHE_DIR/.write-retry-at" ]]; then
            read -r retry_at < "$LED_API_CACHE_DIR/.write-retry-at" || retry_at=0
        fi
        if [[ "$retry_at" =~ ^[0-9]+$ ]] && (( retry_at > now && retry_at <= now + 10 )); then
            led_set_error "LED writes cooling down after controller failure; retry in $((retry_at - now))s"
            return 75
        fi
        # A failed multi-action command may already have changed the LED.
        # Invalidate before starting, including timeout/interruption paths.
        if [[ "$logical" == all || "$logical" == snapshot ]]; then
            rm -f "$LED_API_CACHE_DIR"/*.state
        else
            rm -f "$(_cache_file "$logical")"
        fi
    fi
    if command -v timeout >/dev/null 2>&1; then
        if output=$(timeout 5 "$UGREEN_CLI" "$raw" "$@" "${direct_args[@]}" "${access[@]}" 2>&1); then rc=0; else rc=$?; fi
    else
        if output=$("$UGREEN_CLI" "$raw" "$@" "${direct_args[@]}" "${access[@]}" 2>&1); then rc=0; else rc=$?; fi
    fi
    if [[ "$rc" -ne 0 ]]; then
        if (( ${#access[@]} )) && [[ "$rc" == 3 || "$rc" == 5 || "$rc" == 124 ]]; then
            printf '%s\n' "$(( $(date +%s) + 10 ))" > "$LED_API_CACHE_DIR/.write-retry-at"
        fi
        output="${output//$'\n'/ }"
        led_set_error "LED CLI command failed: $logical $action (rc=$rc${output:+, $output})"
        if [[ "${LED_SUPPRESS_CLI_FAILURE:-0}" != 1 ]]; then
            _led_log_failure "led.cli_command_failed" "LED CLI command failed" \
                "logical_led=$logical" "raw_led=$raw" "protocol=$protocol" "action=$action" \
                "exit_code=$rc" "output=$output"
        fi
        return "$rc"
    fi
    LED_LAST_ERROR=""
    printf '%s' "$output"
}

led_cli_power26_probe() {
    led_cli_run power -status
}

# Called once by an enabled daemon before normal control. The bundled CLI owns
# identity checks, initialization, locking and restoration of the power LED.
# Do not invoke from status/backend discovery: those paths must remain read-only.
led_initialize_controller() {
    declare -F hardware_profile_key >/dev/null || return 0
    [[ "$(hardware_profile_key)" == idx6012 ]] || return 0
    _led_cli_run_raw snapshot power -initialize-idx6012 >/dev/null
}

# The bundled CLI adapter maps DH2600 RGB settings to the official white/orange
# N76 palette. Direct mode still rejects arbitrary breath timing.
led_cli_power26_apply() {
    local color="$1" effect="$2"
    if [[ "$effect" != off && "$color" != preserve ]]; then
        led_set_error "Almighty compatibility CLI does not support N76 color selection"
        return 2
    fi
    case "$effect" in
        off) led_cli_run power -off ;;
        steady) led_cli_run power -on ;;
        fast) led_cli_run power -blink 250 250 ;;
        slow) led_cli_run power -blink 1000 1000 ;;
        *) led_set_error "Almighty N76 does not support this effect: $effect"; return 2 ;;
    esac
}

led_power26_profile() {
    declare -F hardware_power26_controller >/dev/null && hardware_power26_controller
}

led_backend_configured() {
    local configured="auto"
    if declare -F settings_get >/dev/null && [[ -n "${SETTINGS_FILE:-}" ]]; then
        configured=$(settings_get "$SETTINGS_FILE" hardware backend "auto")
    fi
    case "$configured" in auto|cli) printf '%s\n' "$configured" ;; *) printf 'cli\n' ;; esac
}

led_backend_reset() {
    LED_BACKEND_ACTIVE=""
    LED_LAST_ERROR=""
    _led_backend_clear_unavailable
    _led_backend_clear_direct
}

led_backend_clear_active() {
    LED_BACKEND_ACTIVE=""
    LED_LAST_ERROR=""
}

_led_backend_probe_quiet() {
    local previous="${LED_SUPPRESS_CLI_FAILURE:-0}" rc
    LED_SUPPRESS_CLI_FAILURE=1
    if "$@"; then rc=0; else rc=$?; fi
    LED_SUPPRESS_CLI_FAILURE="$previous"
    return "$rc"
}

led_n76_direct_select() {
    local bus address spec previous
    declare -F hardware_n76_direct_supported >/dev/null || return 1
    if declare -F hardware_n76_direct_enabled >/dev/null; then
        hardware_n76_direct_enabled || return 1
    else
        hardware_n76_direct_supported || return 1
    fi
    declare -F hardware_n76_direct_bus_candidates >/dev/null || return 1
    declare -F hardware_n76_direct_address_candidates >/dev/null || return 1

    if _led_backend_read_direct; then
        if _led_backend_probe_quiet led_cli_run power -status >/dev/null; then
            return 0
        fi
        _led_backend_clear_direct
    fi
    while IFS= read -r bus; do
        [[ "$bus" =~ ^[0-9]+$ ]] || continue
        while IFS= read -r address; do
            [[ "$address" =~ ^0x(31|26)$ ]] || continue
            spec="${bus}:${address}"
            LED_N76_DIRECT_SPEC="$spec"
            if _led_backend_probe_quiet led_cli_run power -status >/dev/null; then
                _led_backend_write_direct || true
                return 0
            fi
        done < <(hardware_n76_direct_address_candidates)
    done < <(hardware_n76_direct_bus_candidates)
    _led_backend_clear_direct
    return 1
}

led_backend_select() {
    [[ -n "$LED_BACKEND_ACTIVE" ]] && { printf '%s\n' "$LED_BACKEND_ACTIVE"; return 0; }
    _led_backend_read_unavailable && return 1
    ensure_cli || return 1
    if declare -F hardware_n76_direct_enabled >/dev/null && ! hardware_n76_direct_enabled; then
        LED_N76_DIRECT_SPEC=""
    fi
    local probe_target=""
    if declare -F hardware_led_probe_target >/dev/null; then
        probe_target=$(hardware_led_probe_target)
    elif led_power26_profile; then
        probe_target=power
    fi
    if [[ -n "$probe_target" ]]; then
        if _led_backend_probe_quiet led_cli_run "$probe_target" -status >/dev/null; then
            _led_backend_clear_unavailable
            LED_BACKEND_ACTIVE="cli"
            printf '%s\n' "$LED_BACKEND_ACTIVE"
            return 0
        fi
        if led_n76_direct_select; then
            _led_backend_clear_unavailable
            LED_BACKEND_ACTIVE="n76-direct"
            printf '%s\n' "$LED_BACKEND_ACTIVE"
            return 0
        fi
        _led_backend_mark_unavailable "${LED_LAST_ERROR:-LED 控制后端预检失败}"
        return 1
    fi
    _led_backend_clear_unavailable
    LED_BACKEND_ACTIVE="cli"
    printf '%s\n' "$LED_BACKEND_ACTIVE"
}

ensure_led_backend() { led_backend_select >/dev/null; }

led_backend_name() {
    if led_backend_select >/dev/null 2>&1; then printf '%s\n' "$LED_BACKEND_ACTIVE"; else printf 'unavailable\n'; fi
}

led_backend_status_raw() {
    local state
    led_backend_select >/dev/null || return 1
    led_cli_run "$1" -status 2>/dev/null
}

led_op_mode() {
    local raw lower
    raw=$(led_backend_status_raw "$1") || return 1
    lower="${raw,,}"
    if [[ "$lower" =~ op_mode:([a-z]+) ]]; then
        printf '%s\n' "${BASH_REMATCH[1]}"
    elif [[ "$lower" =~ op_mode:([0-3]) ]]; then
        case "${BASH_REMATCH[1]}" in 0) echo off ;; 1) echo on ;; 2) echo blink ;; 3) echo breath ;; esac
    elif [[ "$lower" =~ status[[:space:]]*=[[:space:]]*(off|on|blink|breath) ]]; then
        printf '%s\n' "${BASH_REMATCH[1]}"
    elif [[ "$lower" =~ mode[[:space:]]*=[[:space:]]*(off|on|blink|breath) ]]; then
        printf '%s\n' "${BASH_REMATCH[1]}"
    elif [[ "$lower" =~ ^(off|on|blink|breath)[[:space:]] ]]; then
        printf '%s\n' "${BASH_REMATCH[1]}"
    else
        echo unknown
    fi
}

led_is_off() { [[ "$(led_op_mode "$1")" == "off" ]]; }

_cache_file() { printf '%s/%s.state\n' "$LED_API_CACHE_DIR" "$(printf '%s' "$1" | tr '/' '_')"; }

led_read_cached_state() {
    local file; file=$(_cache_file "$1"); [[ -f "$file" ]] && cat "$file"
}

led_write_cached_state() {
    local file tmp
    file=$(_cache_file "$1"); tmp="${file}.tmp.${BASHPID:-$$}"
    if ! printf '%s\n' "$2" > "$tmp" || ! mv "$tmp" "$file"; then
        rm -f "$tmp" 2>/dev/null || true
        led_set_error "unable to write LED state cache: $file"
        return 1
    fi
}

led_clear_cache() {
    _led_serialized _led_clear_cache_unlocked
}

_led_clear_cache_unlocked() {
    rm -f "$LED_API_CACHE_DIR"/*.state 2>/dev/null || true
    led_backend_clear_active
}

led_ensure_on() {
    if [[ -n "$LED_N76_DIRECT_SPEC" ]] && declare -F hardware_n76_direct_supported >/dev/null && hardware_n76_direct_supported; then
        return 0
    fi
    led_is_off "$1" || return 0
    led_cli_run "$1" -on >/dev/null || return 1
    sleep 0.08
}

led_set_color() {
    led_write_suppressed && return 0
    _led_serialized _led_set_color_unlocked "$@"
}

_led_set_color_unlocked() {
    local led="${1:-}" r="${2:-}" g="${3:-}" b="${4:-}" brightness="${5:-64}" key
    [[ -n "$led" ]] || { led_set_error "LED target is empty"; return 2; }
    led_write_suppressed && return 0
    if led_power26_profile; then
        [[ "$led" == "power" ]] || { led_set_error "DXP480T only exposes the power LED"; return 2; }
        led_set_power26_effect preserve steady
        return
    fi
    ensure_led_backend || return 1
    key="${r},${g},${b},${brightness}"
    [[ "$(led_read_cached_state "$led")" == "$key" ]] && return 0
    led_ensure_on "$led" || true
    led_cli_run "$led" -color "$r" "$g" "$b" -brightness "$brightness" -on >/dev/null || return 1
    led_write_cached_state "$led" "$key"
}

led_set_off() {
    led_write_suppressed && return 0
    _led_serialized _led_set_off_unlocked "$@"
}

_led_set_off_unlocked() {
    local led="${1:-}"
    [[ -n "$led" ]] || { led_set_error "LED target is empty"; return 2; }
    led_write_suppressed && return 0
    if led_power26_profile; then
        [[ "$led" == "power" ]] || { led_set_error "DXP480T only exposes the power LED"; return 2; }
        led_set_power26_effect preserve off
        return
    fi
    ensure_led_backend || return 1
    [[ "$(led_read_cached_state "$led")" == "off" ]] && return 0
    led_cli_run "$led" -off >/dev/null || return 1
    led_write_cached_state "$led" off
}

led_set_blink() {
    led_write_suppressed && return 0
    _led_serialized _led_set_blink_unlocked "$@"
}

_led_set_blink_unlocked() {
    local led="${1:-}" r="${2:-}" g="${3:-}" b="${4:-}" period_ms="${5:-}" on_ms="${6:-}" brightness="${7:-64}"
    local off_ms key
    [[ -n "$led" ]] || { led_set_error "LED target is empty"; return 2; }
    led_write_suppressed && return 0
    if led_power26_profile; then
        local color effect
        [[ "$led" == "power" ]] || { led_set_error "DXP480T only exposes the power LED"; return 2; }
        color=preserve
        if (( period_ms >= 1200 )); then effect=slow; else effect=fast; fi
        led_set_power26_effect "$color" "$effect"
        return
    fi
    ensure_led_backend || return 1
    (( period_ms < 2 )) && period_ms=2
    (( on_ms < 1 )) && on_ms=1
    (( on_ms >= period_ms )) && on_ms=$((period_ms / 2))
    off_ms=$((period_ms - on_ms)); (( off_ms < 1 )) && off_ms=1
    key="blink,${r},${g},${b},${on_ms},${off_ms},${brightness}"
    [[ "$(led_read_cached_state "$led")" == "$key" ]] && return 0
    led_cli_run "$led" -color "$r" "$g" "$b" -blink "$on_ms" "$off_ms" -brightness "$brightness" >/dev/null || return 1
    led_write_cached_state "$led" "$key"
}

# The DXP480T page uses supported modes of the bundled compatibility CLI. No shell
# code in the application probes buses or accesses the LED controller directly.
led_set_power26_effect() {
    led_write_suppressed && return 0
    _led_serialized _led_set_power26_effect_unlocked "$@"
}

_led_set_power26_effect_unlocked() {
    local color="${1:-preserve}" effect="${2:-steady}"
    local key
    led_write_suppressed && return 0
    led_power26_profile || { led_set_error "DXP480T power protocol is not active for this model"; return 2; }
    case "$color" in preserve) ;; *) [[ "$effect" == off ]] || { led_set_error "N76 color selection is unsupported by this release"; return 2; } ;; esac
    case "$effect" in off|steady|fast|slow) ;; *) return 2 ;; esac
    ensure_led_backend || return 1
    key="power26,${color},${effect}"
    [[ "$(led_read_cached_state power)" == "$key" ]] && return 0
    led_cli_power26_apply "$color" "$effect" >/dev/null || return 1
    led_write_cached_state power "$key"
}

led_power26_available() { led_cli_power26_probe >/dev/null; }
led_power26_bus_candidates() { return 1; }
led_power26_address_candidates() { return 1; }
led_power26_detect() { return 1; }
led_power26_signature_matches() { return 1; }
led_power26_transaction_guard() { ensure_cli; }
led_power26_lock_acquire() { return 0; }
led_power26_lock_release() { return 0; }
led_power26_apply_transaction() { return 1; }
led_power26_set_color() { return 1; }
led_power26_set_off() { return 1; }
led_power26_write() { return 1; }
led_power26_write_u16() { return 1; }

led_sysfs_available() { return 1; }
led_driver_loaded() {
    if declare -F driver_module_loaded >/dev/null && driver_module_loaded; then return 0; fi
    if declare -F driver_vendor_conflict >/dev/null && driver_vendor_conflict; then return 0; fi
    lsmod 2>/dev/null | grep -Eq '^(led_ugreen|leds_mcu[^[:space:]]*|ugreen_leds|leds_ugreen)[[:space:]]'
}

led_list_network_slots() {
    if declare -F hardware_network_leds >/dev/null; then hardware_network_leds; else echo netdev; fi
}

led_list_disk_slots() {
    local count i line raw
    if declare -F hardware_disk_count >/dev/null; then
        count=$(hardware_disk_count)
        if [[ "$count" =~ ^[0-9]+$ ]]; then
            for ((i = 1; i <= count; i++)); do echo "disk${i}"; done
            return 0
        fi
    fi
    raw=$(led_cli_run all -status 2>/dev/null) || return 1
    while IFS= read -r line; do
        [[ "$line" =~ ^(disk[0-9]+): ]] && echo "${BASH_REMATCH[1]}"
    done <<< "$raw" | sort -V -u
}

led_all_status() {
    local led raw state line name i
    local -a logical_leds=() raw_leds=()
    led_backend_select >/dev/null || return 1
    echo "backend: $(led_backend_name)"
    for led in power $(led_list_network_slots 2>/dev/null) $(led_list_disk_slots 2>/dev/null); do
        logical_leds+=("$led")
        raw_leds+=("$(led_cli_name "$led")")
    done
    # One discovery and lock acquisition for the complete snapshot. Explicit
    # names avoid probing nonexistent LEDs and preserve iDX6011 Pro mapping.
    raw=$(_led_cli_run_raw snapshot "${raw_leds[@]}" -status) || return $?
    while IFS= read -r line; do
        name="${line%%:*}"
        if [[ "$line" =~ ^([[:alnum:]_-]+)[[:space:]]+status[[:space:]]+\-[[:space:]]\>[[:space:]](.*)$ ]]; then
            # The upstream direct status command keeps its target label in a
            # whitespace-delimited human-readable line. Normalize it to the
            # legacy `name: value` form used by the application status API.
            name="${BASH_REMATCH[1]}"
            line="$name: ${BASH_REMATCH[2]}"
        fi
        for i in "${!raw_leds[@]}"; do
            if [[ "$name" == "${raw_leds[$i]}" && "$line" == *': '* ]]; then
                printf '%s: %s\n' "${logical_leds[$i]}" "${line#*: }"
                break
            fi
        done
    done <<< "$raw"
}
