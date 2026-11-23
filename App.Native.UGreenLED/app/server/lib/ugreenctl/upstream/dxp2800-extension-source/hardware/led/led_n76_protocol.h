#ifndef UGREENCTL_LED_N76_PROTOCOL_H
#define UGREENCTL_LED_N76_PROTOCOL_H

#include "led/led.h"

/* Pure protocol planning only. No I/O, module build, scanning or authorization.
 * A successful plan is NOT permission to send it or proof of hardware state. */
struct ugreenctl_n76_command {
    uint8_t reg, value;
    unsigned int settle_ms;
};
struct ugreenctl_n76_plan {
    struct ugreenctl_n76_command commands[12];
    size_t count;
    uint8_t mode_registers[2], expected_modes[2];
    unsigned int unverified_fields;
};
int ugreenctl_n76_plan(const struct ugreenctl_led_profile *profile, uint8_t id,
                      const struct ugreenctl_led_patch *patch,
                      struct ugreenctl_n76_plan *out, char *error, size_t size);

#endif
