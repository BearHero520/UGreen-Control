#ifndef UGREENCTL_IDENTITY_H
#define UGREENCTL_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

struct ugreenctl_machine_identity {
    char source[16];
    char product_name[128]; /* Raw DMI name or device-tree model. */
    char product_version[128];
    char manufacturer[128];
    char board_version[128];
    char compatible[512]; /* Exact DT tokens separated by '|'. */
    char bios_vendor[128];
    char bios_version[128];
    char bios_date[128];
};

int ugreenctl_identity_read(struct ugreenctl_machine_identity *identity,
                           char *error, size_t error_size);
/* Explicit fixture injection only; production always reads /sys. */
int ugreenctl_identity_read_at(const char *sysfs_root,
                              struct ugreenctl_machine_identity *identity,
                              char *error, size_t error_size);
bool ugreenctl_identity_has_compatible(const struct ugreenctl_machine_identity *identity,
                                      const char *token);
/* Canonical aliases require an exact firmware-proven DT model and compatible. */
const char *ugreenctl_identity_model(const struct ugreenctl_machine_identity *identity);

#endif
