#ifndef UGREENCTL_WOL_DXP4800S_H
#define UGREENCTL_WOL_DXP4800S_H

#include <stddef.h>

#include "ugreenctl.h"

const char * const *dxp4800s_wol_interfaces(size_t *interface_count);
int dxp4800s_read_wol_policy(enum ugreenctl_wol_policy *policy,
                             char *error, size_t error_size);
int dxp4800s_set_wol_policy(enum ugreenctl_wol_policy policy,
                            char *error, size_t error_size);

#endif
