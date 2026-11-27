#ifndef UGREENCTL_WOL_DXP4800_H
#define UGREENCTL_WOL_DXP4800_H

#include <stddef.h>

#include "ugreenctl.h"

const char * const *dxp4800_wol_interfaces(size_t *interface_count);
int dxp4800_read_wol_policy(enum ugreenctl_wol_policy *policy,
                            char *error, size_t error_size);
int dxp4800_set_wol_policy(enum ugreenctl_wol_policy policy,
                           char *error, size_t error_size);

#endif
