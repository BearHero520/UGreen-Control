#ifndef UGREENCTL_DISK_H
#define UGREENCTL_DISK_H

#include <stdbool.h>
#include <stddef.h>

#define UGREENCTL_DISK_MAX 128U

struct ugreenctl_disk {
    char name[64];
    char device[80];
    char serial[256];
    char wwn[256];
    /* PCI function and controller-local ATA port, not the global ataX ID. */
    char topology[128];
    char pci[16];
    char led_id[16];
    char mapping_source[32];
    char reason[160];
    unsigned int ata;
    unsigned int ata_port;
    unsigned int slot;
    bool mapped;
    char interface_type[16];
    char slot_name[32];
    bool led_supported;
};

struct ugreenctl_disk_scan {
    struct ugreenctl_disk disks[UGREENCTL_DISK_MAX];
    size_t count;
    bool profile_known;
    char profile[64];
    /* Namespace aliases belong to the one physical controller represented by
     * disk_name. They are not additional physical disks or slot claims. */
    struct {
        char name[64];
        char wwn[256];
        char disk_name[64];
    } namespace_aliases[UGREENCTL_DISK_MAX];
    size_t namespace_count;
};

/* Every call rereads sysfs. No SMART/drive I/O, subprocesses or cached sdX map.
 * Empty/missing DMI allows enumeration only; explicit calibration is refused.
 */
int ugreenctl_disk_scan(const char *dmi_product_name,
                       const char *calibration_path,
                       struct ugreenctl_disk_scan *out,
                       char *error, size_t error_size);

/* Explicit sysfs injection for fixture tests; not an environment override. */
int ugreenctl_disk_scan_at(const char *sysfs_root,
                          const char *dmi_product_name,
                          const char *calibration_path,
                          struct ugreenctl_disk_scan *out,
                          char *error, size_t error_size);

/* Selectors: physical device/namespace name with optional /dev/, serial:SN,
 * wwn:ID, slotN, diskN, nvmeN slot name, or exact serial. Namespace aliases
 * resolve to one physical controller; distinct controllers remain ambiguous.
 * Resolves only a unique mapped physical device. The pointer is owned by scan.
 * A new scan must be used immediately before locating a disk's LED.
 */
int ugreenctl_disk_resolve(const struct ugreenctl_disk_scan *scan,
                          const char *selector,
                          const struct ugreenctl_disk **out,
                          char *error, size_t error_size);

#endif
