#ifndef UGREENCTL_WOL_H
#define UGREENCTL_WOL_H

#include <stddef.h>

#include "ugreenctl.h"

int ugreenctl_wol_read_policy(const char * const *interfaces, size_t interface_count,
                              enum ugreenctl_wol_policy *policy,
                              char *error, size_t error_size);
/* Use a model-specific physical-port count only after the firmware names do
 * not resolve. This keeps the stock name map preferred while allowing a
 * documented alternative-OS topology without guessing interface names. */
int ugreenctl_wol_read_policy_with_renamed_count(const char * const *interfaces,
                                                 size_t interface_count,
                                                 size_t renamed_interface_count,
                                                 enum ugreenctl_wol_policy *policy,
                                                 char *error, size_t error_size);
int ugreenctl_wol_set_policy(const char * const *interfaces, size_t interface_count,
                             enum ugreenctl_wol_policy policy,
                             char *error, size_t error_size);
int ugreenctl_wol_set_policy_with_renamed_count(const char * const *interfaces,
                                                size_t interface_count,
                                                size_t renamed_interface_count,
                                                enum ugreenctl_wol_policy policy,
                                                char *error, size_t error_size);

#endif
