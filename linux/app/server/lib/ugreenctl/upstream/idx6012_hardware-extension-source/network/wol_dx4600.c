#include "network/wol_dx4600.h"

#include "network/wol.h"

#define DX4600_RENAMED_INTERFACE_COUNT 2U

/* Official DX4600 hwmonitor applies the power.conf WOL policy to these exact
 * stock interface names.  Official hardware specifications independently
 * confirm two wired Ethernet ports for this family.  On an alternative OS,
 * require exactly two physical PCI Ethernet adapters when both stock names
 * are absent; never guess an arbitrary renamed interface. */
static const char * const interfaces[] = {
    "eth0",
    "eth1"
};

const char * const *dx4600_wol_interfaces(size_t *interface_count)
{
    if (interface_count != NULL) {
        *interface_count = sizeof(interfaces) / sizeof(interfaces[0]);
    }
    return interfaces;
}

int dx4600_read_wol_policy(enum ugreenctl_wol_policy *policy,
                           char *error, size_t error_size)
{
    size_t interface_count;
    const char * const *mapped_interfaces = dx4600_wol_interfaces(&interface_count);

    return ugreenctl_wol_read_policy_with_renamed_count(
        mapped_interfaces, interface_count, DX4600_RENAMED_INTERFACE_COUNT,
        policy, error, error_size);
}

int dx4600_set_wol_policy(enum ugreenctl_wol_policy policy,
                          char *error, size_t error_size)
{
    size_t interface_count;
    const char * const *mapped_interfaces = dx4600_wol_interfaces(&interface_count);

    return ugreenctl_wol_set_policy_with_renamed_count(
        mapped_interfaces, interface_count, DX4600_RENAMED_INTERFACE_COUNT,
        policy, error, error_size);
}
