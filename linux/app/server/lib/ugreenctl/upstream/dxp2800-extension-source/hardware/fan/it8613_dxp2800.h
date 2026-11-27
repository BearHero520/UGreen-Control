#ifndef UGREENCTL_IT8613_DXP2800_H
#define UGREENCTL_IT8613_DXP2800_H

#include "ugreenctl.h"

/* Shared exact board/revision guard for this model's verified capabilities. */
int it8613_dxp2800_require_identity(char *error, size_t error_size);
int it8613_dxp2800_read_fan(struct ugreenctl_fan_status *fans, size_t *fan_count,
                           char *error, size_t error_size);
int it8613_dxp2800_set_fan_pwm(const char *fan_id, uint8_t pwm,
                              char *error, size_t error_size);
#endif
