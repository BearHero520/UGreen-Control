#!/bin/bash
# 绿联机型档案：将 DMI 型号转换为控制协议、LED 布局与 HCTL 规则。

UGREEN_CLI_RELEASE="local-dh2600-n76-direct-20261122-swap"

hardware_detected_product_name() {
    local product="${UGREEN_PRODUCT_NAME:-}" file
    if [[ -z "$product" ]]; then
        for file in /sys/class/dmi/id/product_name /sys/devices/virtual/dmi/id/product_name; do
            if [[ -r "$file" ]]; then
                product=$(<"$file")
                [[ -n "$product" ]] && break
            fi
        done
    fi
    if [[ -z "$product" ]] && command -v dmidecode >/dev/null 2>&1; then
        product=$(dmidecode --string system-product-name 2>/dev/null || true)
    fi
    product="${product//$'\r'/}"
    product="${product//$'\n'/}"
    printf '%s\n' "$product"
}

hardware_detected_product_version() {
    local version="${UGREEN_PRODUCT_VERSION:-}" file
    if [[ -z "$version" ]]; then
        for file in /sys/class/dmi/id/product_version /sys/devices/virtual/dmi/id/product_version; do
            if [[ -r "$file" ]]; then version=$(<"$file"); [[ -n "$version" ]] && break; fi
        done
    fi
    version="${version//$'\r'/}"
    version="${version//$'\n'/}"
    printf '%s\n' "$version"
}

hardware_profile_from_name() {
    local normalized="${1^^}"
    case "$normalized" in
        DH2600) echo "dh2600" ;;
        *IDX6011*PRO*) echo "idx6011_pro" ;;
        *IDX6011*) echo "idx6011" ;;
        *IDX6012*) echo "idx6012" ;;
        *DXP480T*) echo "dxp480t_plus" ;;
        *DXP2800*GT*) echo "dxp2800_gt" ;;
        *DXP2800*) echo "dxp2800" ;;
        *DXP4800*GT*) echo "dxp4800_gt" ;;
        *DXP4800S*) echo "dxp4800s" ;;
        *DXP4800*PLUS*) echo "dxp4800_plus" ;;
        *DXP4800*PRO*) echo "dxp4800_pro" ;;
        *DXP4800*) echo "dxp4800" ;;
        *DXP6800*) echo "dxp6800" ;;
        *DXP8800*) echo "dxp8800" ;;
        *DX4700*) echo "dx4700" ;;
        *DX4600*) echo "dx4600" ;;
        *) echo "unknown" ;;
    esac
}

hardware_profile_key() {
    local override="auto"
    if declare -F settings_get >/dev/null && [[ -n "${SETTINGS_FILE:-}" ]]; then
        override=$(settings_get "$SETTINGS_FILE" hardware profile "auto")
    fi
    case "$override" in
        dh2600|dx4600|dx4700|dxp2800|dxp2800_gt|dxp4800|dxp4800_plus|dxp4800_pro|dxp4800s|dxp4800_gt|dxp6800|dxp8800|dxp480t_plus|idx6011|idx6011_pro|idx6012)
            echo "$override"
            ;;
        *)
            local product vendor
            product=$(hardware_detected_product_name)
            vendor="${UGREEN_SYS_VENDOR:-}"
            if [[ -z "$vendor" && -r /sys/class/dmi/id/sys_vendor ]]; then
                vendor=$(</sys/class/dmi/id/sys_vendor)
            fi
            # The user confirmed this exact shared-board signature is iDX6012.
            # This selects application layout only; the CLI independently checks DMI.
            if [[ "$product" == 'Default string' && "$vendor" == UGREEN &&
                  "$(hardware_detected_product_version)" == 'iDX6011_iDX6012 V3.0 241112' ]]; then
                echo idx6012
            else
                hardware_profile_from_name "$product"
            fi
            ;;
    esac
}

hardware_profile_display_name() {
    case "${1:-$(hardware_profile_key)}" in
        dh2600) echo "UGREEN DH2600" ;;
        dx4600) echo "UGREEN DX4600 / DX4600+ / DX4600 Pro" ;;
        dx4700) echo "UGREEN DX4700+" ;;
        dxp2800) echo "UGREEN DXP2800" ;;
        dxp2800_gt) echo "UGREEN DXP2800 GT" ;;
        dxp4800) echo "UGREEN DXP4800" ;;
        dxp4800_plus) echo "UGREEN DXP4800 Plus" ;;
        dxp4800_pro) echo "UGREEN DXP4800 Pro" ;;
        dxp4800s) echo "UGREEN DXP4800S" ;;
        dxp4800_gt) echo "UGREEN DXP4800 GT" ;;
        dxp6800)
            case "$(hardware_detected_product_name)" in
                "DXP6800 Plus") echo "UGREEN DXP6800 Plus" ;;
                *) echo "UGREEN DXP6800 Pro" ;;
            esac
            ;;
        dxp8800)
            case "$(hardware_detected_product_name)" in
                "DXP8800"|"UGREEN DXP8800") echo "UGREEN DXP8800" ;;
                "DXP8800 Plus"|"UGREEN DXP8800 Plus") echo "UGREEN DXP8800 Plus" ;;
                "DXP8800 Pro"|"UGREEN DXP8800 Pro") echo "UGREEN DXP8800 Pro" ;;
                "DXP8800 Ultra"|"UGREEN DXP8800 Ultra") echo "UGREEN DXP8800 Ultra" ;;
                *) echo "UGREEN DXP8800 系列" ;;
            esac
            ;;
        dxp480t_plus) echo "UGREEN DXP480T / DXP480T Plus" ;;
        idx6011) echo "UGREEN iDX6011" ;;
        idx6011_pro) echo "UGREEN iDX6011 Pro" ;;
        idx6012) echo "UGREEN iDX6012" ;;
        *) echo "未知机型" ;;
    esac
}

hardware_support_level() {
    case "${1:-$(hardware_profile_key)}" in
        dh2600|dx4600|dx4700|dxp2800|dxp4800|dxp4800_plus|dxp6800|dxp8800) echo "experimental" ;;
        dxp4800s|dxp4800_gt|idx6011|idx6011_pro|idx6012) echo "experimental" ;;
        dxp2800_gt|dxp4800_pro) echo "unverified" ;;
        dxp480t_plus) echo "limited" ;;
        *) echo "unknown" ;;
    esac
}

hardware_write_protocol() {
    echo "upstream-managed"
}

hardware_disk_count() {
    case "${1:-$(hardware_profile_key)}" in
        dh2600|dxp2800|dxp2800_gt) echo 2 ;;
        dx4600|dx4700|dxp4800|dxp4800_plus|dxp4800_pro|dxp4800s|dxp4800_gt) echo 4 ;;
        dxp6800|idx6011|idx6011_pro|idx6012) echo 6 ;;
        dxp8800) echo 8 ;;
        dxp480t_plus) echo 0 ;;
        *) echo "" ;;
    esac
}

hardware_netdev_count() {
    case "${1:-$(hardware_profile_key)}" in
        dh2600|dxp480t_plus) echo 0 ;;
        idx6011_pro) echo 2 ;;
        *) echo 1 ;;
    esac
}

hardware_driver_supported() {
    [[ "${1:-$(hardware_profile_key)}" != "dxp480t_plus" ]]
}

hardware_power26_controller() {
    [[ "${1:-$(hardware_profile_key)}" == "dxp480t_plus" ]]
}

# N76 profiles need one read-only CLI preflight before the daemon starts its
# normal state loop. The CLI remains the only hardware access path.
hardware_led_probe_target() {
    case "${1:-$(hardware_profile_key)}" in
        dh2600|dxp480t_plus) echo "power" ;;
        *) echo "" ;;
    esac
}

# The official DH2600 N76 driver is attached to the Intel I801 SMBus and
# accepts only 0x31 or 0x26. These helpers expose read-only adapter metadata to
# the application adapter. All identity checks and I2C transactions remain in
# the bundled ugreen_leds_cli/ugreenctl implementation.
hardware_n76_direct_supported() {
    [[ "${1:-$(hardware_profile_key)}" == "dh2600" ]]
}

hardware_n76_direct_enabled() {
    hardware_n76_direct_supported "${1:-$(hardware_profile_key)}" || return 1
    if declare -F settings_get >/dev/null && [[ -n "${SETTINGS_FILE:-}" ]]; then
        [[ "$(settings_get "$SETTINGS_FILE" hardware n76_direct auto)" != "off" ]]
    fi
}

hardware_n76_direct_bus_candidates() {
    local override path name bus
    override="${UGREEN_N76_BUS_CANDIDATES:-}"
    if [[ -n "$override" ]]; then
        tr ', ' '\n' <<< "$override"
        return 0
    fi
    for path in /sys/class/i2c-adapter/i2c-* /sys/class/i2c-dev/i2c-*; do
        [[ -r "$path/name" ]] || continue
        name=$(<"$path/name")
        [[ "$name" == SMBus\ I801\ adapter* ]] || continue
        bus=${path##*/i2c-}
        [[ "$bus" =~ ^[0-9]+$ ]] && printf '%s\n' "$bus"
    done | sort -n -u
}

hardware_n76_direct_address_candidates() {
    printf '%s\n' 0x31 0x26
}

hardware_hctl_profile() {
    case "${1:-$(hardware_profile_key)}" in
        # DH2600 firmware reverses the two SATA LED positions.
        dh2600) echo "dh2600" ;;
        # Intel hosts 0/1 -> bays 5/6; ASMedia hosts 2..5 -> bays 1..4.
        # iDX6012 shares the existing six-bay routing, not sequential HCTL+1.
        dxp6800|idx6012) echo "dxp6800" ;;
        *) echo "sequential" ;;
    esac
}

# Almighty owns exact-model LED channel mappings, including netdev2.
hardware_cli_led_name() {
    local led="${1:-}"
    # The fnOS N76 path exposes the two DH2600 outputs in the opposite order
    # from the physical bay labels. Keep the application mapping in physical
    # bay order and translate only direct userspace commands. Bound-driver
    # sysfs names already follow the kernel driver's logical names.
    if [[ -n "${LED_N76_DIRECT_SPEC:-}" && "$(hardware_profile_key)" == dh2600 ]]; then
        case "$led" in
            disk1) echo disk2; return ;;
            disk2) echo disk1; return ;;
        esac
    fi
    printf '%s\n' "$led"
}

hardware_network_leds() {
    local count i
    count=$(hardware_netdev_count)
    for ((i = 1; i <= count; i++)); do
        if ((i == 1)); then echo "netdev"; else echo "netdev${i}"; fi
    done
}

hardware_profile_options() {
    cat <<'EOF'
auto|自动识别
dh2600|UGREEN DH2600（实验性）
dx4600|UGREEN DX4600 Pro
dx4700|UGREEN DX4700+
dxp2800|UGREEN DXP2800
dxp2800_gt|UGREEN DXP2800 GT（待验证）
dxp4800|UGREEN DXP4800
dxp4800_plus|UGREEN DXP4800 Plus
dxp4800_pro|UGREEN DXP4800 Pro（待验证）
dxp4800s|UGREEN DXP4800S（BIOS 控制固件逆向）
dxp4800_gt|UGREEN DXP4800 GT（实验性）
dxp6800|UGREEN DXP6800 Pro
dxp8800|UGREEN DXP8800 Plus
dxp480t_plus|UGREEN DXP480T / DXP480T Plus（仅电源灯）
idx6011|UGREEN iDX6011（实验性）
idx6011_pro|UGREEN iDX6011 Pro（实验性）
idx6012|UGREEN iDX6012（实验性）
EOF
}
