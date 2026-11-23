#ifndef UGREENCTL_WOL_INTERFACE_H
#define UGREENCTL_WOL_INTERFACE_H
#include "models/wake_profiles.h"
#include "ugreenctl.h"
#define UGREENCTL_WOL_NAME_SIZE 16
int ugreenctl_wol_interfaces_at(const char *root, const struct ugreenctl_wol_profile *profile,
                               char (*names)[UGREENCTL_WOL_NAME_SIZE], size_t capacity,
                               size_t *count, char *error, size_t error_size);
int ugreenctl_wol_interface_identity_at(const char *root, const struct ugreenctl_wol_profile *profile,
                                       const char *name, char *identity, size_t size,
                                       char *error, size_t error_size);
int ugreenctl_wol_interface(const struct ugreenctl_wol_profile *profile, const char *name,
                            bool write, enum ugreenctl_wol_policy *policy,
                            char *error, size_t error_size);
#endif
