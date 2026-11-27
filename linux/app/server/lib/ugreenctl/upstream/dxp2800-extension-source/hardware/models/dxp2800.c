#include "ugreenctl.h"
#include "fan/it8613_dxp2800.h"
#include "power/it8613_power.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char * const products[] = {"DXP2800", NULL};

static int read_startup(const struct ugreenctl_request *request,
                        enum ugreenctl_startup_policy *policy, char *error, size_t size)
{
    int result = it8613_dxp2800_require_identity(error, size);
    if (result != 0) return result;
    return it8613_read_startup_policy(request->force, policy, error, size);
}

static int set_startup(const struct ugreenctl_request *request,
                       enum ugreenctl_startup_policy policy, char *error, size_t size)
{
    int result;
    if (!request->force) {
        (void)snprintf(error, size, "DXP2800 EM26 AC recovery writes require --force --apply");
        return -EPERM;
    }
    result = it8613_dxp2800_require_identity(error, size);
    if (result != 0) return result;
    return it8613_set_startup_policy(request->force, policy, error, size);
}

static int read_fans(const struct ugreenctl_request *request,
                     struct ugreenctl_fan_status *fans, size_t *count,
                     char *error, size_t size)
{
    (void)request;
    return it8613_dxp2800_read_fan(fans, count, error, size);
}

static int read_status(const struct ugreenctl_request *request,
                       struct ugreenctl_status *status, char *error, size_t size)
{
    memset(status, 0, sizeof(*status));
    status->startup_policy = UGREENCTL_STARTUP_UNKNOWN;
    status->wol_policy = UGREENCTL_WOL_UNKNOWN;
    (void)snprintf(status->controller, sizeof(status->controller), "ITE IT8613 hwmon/direct (EM26)");
    return read_fans(request, status->fans, &status->fan_count, error, size);
}

static int set_pwm(const struct ugreenctl_request *request, const char *id,
                   uint8_t pwm, char *error, size_t size)
{
    /* The CLI enforces --apply before invoking a plugin; ABI v5 carries force. */
    if (!request->force) {
        (void)snprintf(error, size, "DXP2800 EM26 writes require --force --apply; physical validation pending");
        return -EPERM;
    }
    return it8613_dxp2800_set_fan_pwm(id, pwm, error, size);
}

const struct ugreenctl_plugin *ugreenctl_plugin_v5(void)
{
    static const struct ugreenctl_plugin plugin = {
        .abi_version = UGREENCTL_PLUGIN_ABI_V5,
        .id = "dxp2800",
        .display_name = "UGREEN DXP2800 EM26 (firmware-reversed)",
        .dmi_product_names = products,
        .capabilities = UGREENCTL_CAP_FAN | UGREENCTL_CAP_POWER,
        .read_status = read_status,
        .set_fan_pwm = set_pwm,
        .read_fans = read_fans,
        .read_startup_policy = read_startup,
        .set_startup_policy = set_startup,
        .controller_name = "ITE IT8613 hwmon/direct (EM26)"
    };
    return &plugin;
}
