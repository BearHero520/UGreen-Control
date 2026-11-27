#ifndef UGREENCTL_WOL_DXP480TPLUS_H
#define UGREENCTL_WOL_DXP480TPLUS_H

#include <stddef.h>

#include "ugreenctl.h"

const char * const *dxp480tplus_wol_interfaces(size_t *interface_count);
int dxp480tplus_read_wol_policy(enum ugreenctl_wol_policy *policy,
                                char *error, size_t error_size);
int dxp480tplus_set_wol_policy(enum ugreenctl_wol_policy policy,
                               char *error, size_t error_size);

#endif
