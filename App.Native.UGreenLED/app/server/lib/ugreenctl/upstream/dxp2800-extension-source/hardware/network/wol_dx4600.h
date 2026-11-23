#ifndef UGREENCTL_WOL_DX4600_H
#define UGREENCTL_WOL_DX4600_H

#include <stddef.h>

#include "ugreenctl.h"

const char * const *dx4600_wol_interfaces(size_t *interface_count);
int dx4600_read_wol_policy(enum ugreenctl_wol_policy *policy,
                           char *error, size_t error_size);
int dx4600_set_wol_policy(enum ugreenctl_wol_policy policy,
                          char *error, size_t error_size);

#endif
