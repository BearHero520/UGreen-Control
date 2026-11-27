#include "ugreenctl.h"

#ifndef MODEL_ID
#error "MODEL_ID must be supplied by CMake"
#endif
#ifndef MODEL_NAME
#error "MODEL_NAME must be supplied by CMake"
#endif
#ifndef MODEL_DMI_NAME
#error "MODEL_DMI_NAME must be supplied by CMake"
#endif

static const char * const product_names[] = {
    MODEL_DMI_NAME,
    NULL
};

const struct ugreenctl_plugin *ugreenctl_plugin_v3(void)
{
    static const struct ugreenctl_plugin plugin = {
        .abi_version = UGREENCTL_PLUGIN_ABI_V3,
        .id = MODEL_ID,
        .display_name = MODEL_NAME,
        .dmi_product_names = product_names,
        .capabilities = 0,
        .read_status = NULL,
        .set_fan_pwm = NULL,
        .set_startup_policy = NULL,
        .set_led = NULL,
        .set_fan_mode = NULL
    };
    return &plugin;
}

const struct ugreenctl_plugin *ugreenctl_plugin_v2(void)
{
    static const struct ugreenctl_plugin plugin = {
        .abi_version = UGREENCTL_PLUGIN_ABI_V2,
        .id = MODEL_ID,
        .display_name = MODEL_NAME,
        .dmi_product_names = product_names,
        .capabilities = 0,
        .read_status = NULL,
        .set_fan_pwm = NULL,
        .set_startup_policy = NULL,
        .set_led = NULL,
        .set_fan_mode = NULL
    };
    return &plugin;
}
