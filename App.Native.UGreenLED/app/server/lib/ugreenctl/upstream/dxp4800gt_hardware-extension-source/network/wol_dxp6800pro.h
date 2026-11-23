#ifndef UGREENCTL_WOL_DXP6800PRO_H
#define UGREENCTL_WOL_DXP6800PRO_H

#include <stddef.h>

#include "ugreenctl.h"

const char * const *dxp6800pro_wol_interfaces(size_t *interface_count);
int dxp6800pro_read_wol_policy(enum ugreenctl_wol_policy *policy,
                               char *error, size_t error_size);
int dxp6800pro_set_wol_policy(enum ugreenctl_wol_policy policy,
                              char *error, size_t error_size);

#endif
