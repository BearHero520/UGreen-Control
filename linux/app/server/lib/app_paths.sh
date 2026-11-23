#!/bin/bash
# Platform-independent paths. Executables always come from this application's bundle.
ugreen_resolve_runtime() {
    RUNTIME_DIR="${UGREEN_RUNTIME_DIR:-${RUNTIME_DIR:-/run/ugreen-nas}}"
    mkdir -p "$RUNTIME_DIR" || return 1
    chmod 0700 "$RUNTIME_DIR" || return 1
    export RUNTIME_DIR
}

ugreen_resolve_paths() {
    APP_NAME="${APP_NAME:-ugreen-nas-tool}"
    APP_ROOT="${APP_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
    VAR_DIR="${VAR_DIR:-/var/lib/ugreen-nas}"
    SERVER_DIR="${APP_ROOT}/server"
    WWW_DIR="${APP_ROOT}/www"
    LIB_DIR="${SERVER_DIR}/lib"
    UGREEN_CLI="${SERVER_DIR}/bin/ugreen_leds_cli"
    ugreen_resolve_runtime || return 1
    export APP_NAME APP_ROOT VAR_DIR RUNTIME_DIR SERVER_DIR WWW_DIR LIB_DIR UGREEN_CLI
    export TARGET="$APP_ROOT"
}
