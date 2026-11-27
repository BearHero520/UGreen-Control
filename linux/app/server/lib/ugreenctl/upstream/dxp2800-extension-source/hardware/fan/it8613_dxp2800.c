#include "fan/it8613_dxp2800.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "core/identity.h"
#include "fan/it8613_direct.h"
#include "fan/it8613_hwmon.h"

/* UGOS 1.20 ug-load-drive.sh routes EM_DXP2800 to ug_it86x-sio.ko.
 * set_fan.isra.0@0x210: clear 0x17 bit7, duty 0x73.
 * fan_read@0x4c0: tachometer 0x0f/0x1a. See DXP2800_EM26_SUPPORT.md.
 * This independent mapping must not be inherited by the JW/V1.2/GT boards. */
static const struct it8613_hwmon_channel channel = {"sys", 3, 3};
static const struct it8613_direct_channel direct_channel = {
    "sys", 0x17, 0x73, 0x0f, 0x1a, true
};

int it8613_dxp2800_require_identity(char *error, size_t error_size)
{
    struct ugreenctl_machine_identity identity;
    int result = ugreenctl_identity_read(&identity, error, error_size);
    if (result != 0) return result;
    if (strcmp(identity.product_name, "DXP2800") != 0 ||
        strcmp(identity.product_version, "EM_DXP2800_V1.0.26") != 0) {
        (void)snprintf(error, error_size,
                       "DXP2800 hardware control requires exact DMI DXP2800 / "
                       "EM_DXP2800_V1.0.26 (got '%s' / '%s')",
                       identity.product_name, identity.product_version);
        return -ENODEV;
    }
    return 0;
}

int it8613_dxp2800_read_fan(struct ugreenctl_fan_status *fans, size_t *fan_count,
                           char *error, size_t error_size)
{
    int result = it8613_dxp2800_require_identity(error, error_size);
    if (result != 0) return result;
    result = it8613_hwmon_read_fans(&channel, 1, fans, fan_count, error, error_size);
    if (result != -ENODEV) return result;
    return it8613_direct_read_fans(&direct_channel, 1, fans, fan_count, error, error_size);
}

int it8613_dxp2800_set_fan_pwm(const char *fan_id, uint8_t pwm,
                              char *error, size_t error_size)
{
    int result;
    if (fan_id == NULL || strcmp(fan_id, "sys") != 0 || pwm < 40) {
        (void)snprintf(error, error_size, "DXP2800 requires target sys and PWM 40..255");
        return -EINVAL;
    }
    result = it8613_dxp2800_require_identity(error, error_size);
    if (result != 0) return result;
    result = it8613_hwmon_set_manual_pwm(&channel, 1, pwm, error, error_size);
    if (result != -ENODEV) return result;
    return it8613_direct_set_manual_pwm(&direct_channel, 1, pwm, error, error_size);
}
