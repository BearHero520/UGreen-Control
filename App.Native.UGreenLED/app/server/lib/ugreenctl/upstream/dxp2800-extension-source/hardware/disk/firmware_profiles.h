#ifndef UGREENCTL_DISK_FIRMWARE_PROFILES_H
#define UGREENCTL_DISK_FIRMWARE_PROFILES_H

#include <stdbool.h>
#include <stddef.h>

/* Hardware facts recovered from official storage_serv hardware.map.init.0.
 * ATA_GLOBAL/BLOCK_NAME are evidence records, not permission to trust Linux
 * enumeration order; disk.c must convert them through a validated topology.
 */
struct ugreenctl_disk_firmware_rule {
    const char *slot;
    const char *kind;
    const char *identity;
    const char *interface_type;
    bool color_led;
};

struct ugreenctl_disk_firmware_layout {
    const char *model;
    const char *revision;
    const struct ugreenctl_disk_firmware_rule *rules;
    size_t count;
};

const struct ugreenctl_disk_firmware_layout *ugreenctl_disk_firmware_layout(
    const char *model, const char *revision);

#endif
