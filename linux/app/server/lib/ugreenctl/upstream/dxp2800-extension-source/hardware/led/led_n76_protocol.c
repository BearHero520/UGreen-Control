/* Independently written transaction planner based on the documented UGREEN
 * N76 wire protocol. No upstream kernel implementation is incorporated.
 * Provenance and unresolved readback: docs/N76_DIRECT_CONTROL.zh-CN.md. */
#include "led/led_n76_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int reject(char *error, size_t size, const char *message)
{
    if (error && size) (void)snprintf(error, size, "%s", message);
    return -ENOTSUP;
}

static void append(struct ugreenctl_n76_plan *plan, unsigned int reg, unsigned int value)
{
    struct ugreenctl_n76_command *command = &plan->commands[plan->count++];
    command->reg = (uint8_t)reg;
    command->value = (uint8_t)value;
    command->settle_ms = reg == 0xa0 || reg == 0xb1 ? 2 : 10;
}

int ugreenctl_n76_plan(const struct ugreenctl_led_profile *profile, uint8_t id,
                      const struct ugreenctl_led_patch *patch,
                      struct ugreenctl_n76_plan *out, char *error, size_t size)
{
    struct ugreenctl_n76_plan plan = {0};
    const struct ugreenctl_led_descriptor *target = NULL;
    unsigned int bit, color, channel;
    bool dh2600;
    size_t i;
    int result;
    if (!out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!profile || !profile->product_name || !profile->id ||
        !profile->protocol || strcmp(profile->protocol, "sysfs-n76"))
        return reject(error, size, "no exact N76 model profile");
    dh2600 = !strcmp(profile->product_name, "DH2600") && !strcmp(profile->id, "dh2600-n76");
    if (!dh2600 && (strcmp(profile->product_name, "DXP480T Plus") || strcmp(profile->id, "dxp480t-n76")))
        return reject(error, size, "N76 model is not independently mapped");
    if ((dh2600 && id > 2) || (!dh2600 && id != 0))
        return reject(error, size, "N76 target has no physical LED");
    for (i = 0; i < profile->led_count; ++i)
        if (profile->leds[i].id == id) target = &profile->leds[i];
    result = ugreenctl_led_validate_target_patch(target, patch, error, size);
    if (result < 0) return result;
    if (!(patch->fields & UGREENCTL_LED_PATCH_MODE))
        return reject(error, size, "N76 direct plan requires an explicit mode; current output is unknown");
    if (patch->value.mode == UGREENCTL_LED_MODE_BREATH)
        return reject(error, size, "N76 arbitrary breath timing has no verified protocol");
    if (patch->value.mode == UGREENCTL_LED_MODE_OFF && patch->fields != UGREENCTL_LED_PATCH_MODE)
        return reject(error, size, "N76 off plan accepts only mode off");
    if (patch->value.mode != UGREENCTL_LED_MODE_OFF && !(patch->fields & UGREENCTL_LED_PATCH_COLOR))
        return reject(error, size, "N76 direct on/blink requires an explicit palette color");
    if (dh2600 && (patch->fields & UGREENCTL_LED_PATCH_BRIGHTNESS) && patch->value.brightness != 1)
        return reject(error, size, "DH2600 has binary brightness; use mode off to extinguish it");
    color = (unsigned int)patch->value.color;
    if (patch->value.mode == UGREENCTL_LED_MODE_BLINK && color != 1 && color != 2)
        return reject(error, size, "N76 combined-color blink has no verified protocol");
    /* Official DH2600 array order: power bit 0, disk1 bit 4, disk2 bit 2.
     * DXP480T only exposes power bit 0; never infer four NVMe LEDs. */
    bit = id == 1 ? 4 : id == 2 ? 2 : 0;
    plan.mode_registers[0] = (uint8_t)(0x50 + bit);
    plan.mode_registers[1] = (uint8_t)(0x51 + bit);
    /* Compose a complete request without trusting any previous output cache.
     * Explicitly stop both channel effects before changing the selected lamp. */
    append(&plan, 0x50 + bit, 0);
    append(&plan, 0x51 + bit, 0);
    append(&plan, 0xb1, 3U << bit);
    if (patch->value.mode != UGREENCTL_LED_MODE_OFF) {
        if (patch->fields & UGREENCTL_LED_PATCH_TIMING) {
            channel = color == 2 ? 2 : 0;
            append(&plan, 0xc0 + bit * 2 + channel, patch->value.on_ms & 255);
            append(&plan, 0xc1 + bit * 2 + channel, patch->value.on_ms >> 8);
            append(&plan, 0xd0 + bit * 2 + channel, patch->value.off_ms & 255);
            append(&plan, 0xd1 + bit * 2 + channel, patch->value.off_ms >> 8);
        }
        if (!dh2600 && (patch->fields & UGREENCTL_LED_PATCH_BRIGHTNESS)) {
            append(&plan, 0xe0, patch->value.brightness);
            append(&plan, 0xe1, patch->value.brightness);
        }
        append(&plan, 0xa0, color << bit);
        if (patch->value.mode == UGREENCTL_LED_MODE_BLINK) {
            channel = color == 2 ? 1 : 0;
            append(&plan, 0x50 + bit + channel, 1);
            plan.expected_modes[channel] = 1;
        }
    }
    /* Mode registers indicate effects, not actual output enable or color.
     * Even an off request cannot claim verified output from mode==0. */
    plan.unverified_fields = UGREENCTL_LED_PATCH_MODE | UGREENCTL_LED_PATCH_COLOR |
        UGREENCTL_LED_PATCH_BRIGHTNESS | UGREENCTL_LED_PATCH_TIMING;
    *out = plan;
    return 0;
}
