/* Legacy userspace protocol adapted from miskcoo/ugreen_leds_controller.
 * Copyright (c) 2024 miskcoo@gmail.com; MIT, see LICENSE.upstream-led.
 * No GPL kernel driver implementation is incorporated here. */
#include "led/led_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned int checksum(const uint8_t *data, size_t count)
{
    unsigned int sum = 0;
    size_t i;
    for (i = 0; i < count; ++i) sum += data[i];
    return sum;
}

int ugreenctl_led_decode(const uint8_t *frame, size_t length,
                        struct ugreenctl_led_value *value)
{
    unsigned int sum, cycle, on;
    if (frame == NULL || value == NULL || length != UGREENCTL_LED_FRAME_SIZE)
        return -EINVAL;
    sum = checksum(frame, 9);
    if (sum != ((unsigned int)frame[9] << 8 | frame[10]) ||
        frame[0] > UGREENCTL_LED_MODE_BREATH) return -EBADMSG;
    cycle = ((unsigned int)frame[5] << 8) | frame[6];
    on = ((unsigned int)frame[7] << 8) | frame[8];
    if (on > cycle) return -EBADMSG;
    memset(value, 0, sizeof(*value));
    value->mode = (enum ugreenctl_led_mode)frame[0];
    value->brightness = frame[1];
    value->red = frame[2]; value->green = frame[3]; value->blue = frame[4];
    value->on_ms = on; value->off_ms = cycle - on;
    return 0;
}

int ugreenctl_led_encode(uint8_t id, uint8_t command,
                        const uint8_t parameters[4],
                        uint8_t frame[UGREENCTL_LED_COMMAND_SIZE])
{
    unsigned int sum;
    if (id > 9 || command < 1 || command > 5 || parameters == NULL || frame == NULL)
        return -EINVAL;
    memset(frame, 0, UGREENCTL_LED_COMMAND_SIZE);
    frame[0] = id; frame[1] = 0xa0; frame[2] = 1; frame[5] = command;
    memcpy(frame + 6, parameters, 4);
    /* The LED ID is excluded from this checksum. */
    sum = checksum(frame + 1, 9);
    frame[10] = (uint8_t)(sum >> 8); frame[11] = (uint8_t)sum;
    return 0;
}

const char *ugreenctl_led_mode_name(enum ugreenctl_led_mode mode)
{
    switch (mode) {
    case UGREENCTL_LED_MODE_OFF: return "off";
    case UGREENCTL_LED_MODE_ON: return "on";
    case UGREENCTL_LED_MODE_BLINK: return "blink";
    case UGREENCTL_LED_MODE_BREATH: return "breath";
    default: return "unknown";
    }
}

const char *ugreenctl_led_color_name(enum ugreenctl_led_color color)
{
    static const char *const names[] = {"none","white","orange","white-orange","red","green","blue","channel1","channel2","both"};
    return (unsigned int)color < sizeof(names)/sizeof(names[0]) ? names[color] : "unknown";
}

int ugreenctl_led_encode_smbus(uint8_t command, const uint8_t parameters[4],
                              bool little_endian_checksum, uint8_t frame[12])
{
    uint8_t legacy[12];
    int result = ugreenctl_led_encode(0, command, parameters, legacy);
    if (result < 0) return result;
    memcpy(frame, legacy + 1, 11);
    if (little_endian_checksum) { frame[9] = legacy[11]; frame[10] = legacy[10]; }
    frame[11] = 0;
    return 0;
}

int ugreenctl_led_validate_patch(const struct ugreenctl_led_patch *patch,
                                char *error, size_t error_size)
{
    const unsigned int known = UGREENCTL_LED_PATCH_MODE | UGREENCTL_LED_PATCH_RGB |
        UGREENCTL_LED_PATCH_BRIGHTNESS | UGREENCTL_LED_PATCH_TIMING | UGREENCTL_LED_PATCH_COLOR;
    const char *message = NULL;
    if (patch == NULL || patch->fields == 0 || (patch->fields & ~known) != 0)
        message = "LED patch must contain known fields";
    else if ((patch->fields & UGREENCTL_LED_PATCH_RGB) &&
             (patch->value.red > 255 || patch->value.green > 255 || patch->value.blue > 255))
        message = "RGB components must be in 0..255";
    else if ((patch->fields & UGREENCTL_LED_PATCH_BRIGHTNESS) && patch->value.brightness > 255)
        message = "brightness must be in 0..255";
    else if ((patch->fields & UGREENCTL_LED_PATCH_COLOR) &&
             ((unsigned int)patch->value.color > UGREENCTL_LED_COLOR_MAX ||
              (patch->fields & UGREENCTL_LED_PATCH_RGB)))
        message = "palette color must be known and cannot be combined with RGB";
    else if ((patch->fields & UGREENCTL_LED_PATCH_MODE) &&
             (patch->value.mode < UGREENCTL_LED_MODE_OFF ||
              patch->value.mode > UGREENCTL_LED_MODE_BREATH))
        message = "invalid LED mode";
    else if ((patch->fields & UGREENCTL_LED_PATCH_TIMING) &&
             (!(patch->fields & UGREENCTL_LED_PATCH_MODE) ||
              (patch->value.mode != UGREENCTL_LED_MODE_BLINK &&
               patch->value.mode != UGREENCTL_LED_MODE_BREATH)))
        message = "timing requires blink or breath mode";
    else if ((patch->fields & UGREENCTL_LED_PATCH_MODE) &&
             (patch->value.mode == UGREENCTL_LED_MODE_BLINK ||
              patch->value.mode == UGREENCTL_LED_MODE_BREATH) &&
             !(patch->fields & UGREENCTL_LED_PATCH_TIMING))
        message = "blink and breath require on/off durations";
    else if ((patch->fields & UGREENCTL_LED_PATCH_TIMING) &&
             (patch->value.on_ms > 65535U || patch->value.off_ms > 65535U ||
              patch->value.on_ms > 65535U - patch->value.off_ms))
        message = "on_ms + off_ms must be at most 65535";
    if (message == NULL) return 0;
    if (error != NULL && error_size != 0) (void)snprintf(error, error_size, "%s", message);
    return -EINVAL;
}

int ugreenctl_led_validate_target_patch(const struct ugreenctl_led_descriptor *target,
                                       const struct ugreenctl_led_patch *patch,
                                       char *error, size_t error_size)
{
    const char *message = NULL;
    int result = ugreenctl_led_validate_patch(patch, error, error_size);
    if (result < 0) return result;
    if (target == NULL) message = "LED target is unavailable";
    else if ((patch->fields & UGREENCTL_LED_PATCH_RGB) && !target->rgb)
        message = "this LED has no RGB hardware; use its supported palette colors";
    else if ((patch->fields & UGREENCTL_LED_PATCH_COLOR) &&
             !(target->color_mask & UGREENCTL_LED_COLOR_BIT(patch->value.color)))
        message = "palette color is not supported by this LED";
    else if ((patch->fields & UGREENCTL_LED_PATCH_MODE) &&
             !(target->mode_mask & UGREENCTL_LED_MODE_BIT(patch->value.mode)))
        message = "mode is not supported by this LED";
    else if ((patch->fields & UGREENCTL_LED_PATCH_BRIGHTNESS) && target->max_brightness == 0)
        message = "brightness control is not supported by this LED";
    else if ((patch->fields & UGREENCTL_LED_PATCH_BRIGHTNESS) &&
             patch->value.brightness > target->max_brightness)
        message = "brightness exceeds this LED's supported range";
    else if ((patch->fields & UGREENCTL_LED_PATCH_BRIGHTNESS) &&
             patch->value.brightness < target->min_brightness)
        message = "brightness is below this LED's supported range; use mode off to turn it off";
    else if (!target->rgb && target->max_brightness==1 &&
             (patch->fields&UGREENCTL_LED_PATCH_MODE) && (patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS) &&
             patch->value.mode<=UGREENCTL_LED_MODE_ON &&
             (patch->value.mode==UGREENCTL_LED_MODE_ON)!=(patch->value.brightness!=0))
        message = "binary LED cannot represent contradictory mode and brightness";
    else if ((patch->fields & UGREENCTL_LED_PATCH_TIMING) &&
             patch->value.mode==UGREENCTL_LED_MODE_BLINK &&
             (patch->value.on_ms < target->min_on_ms || patch->value.off_ms < target->min_off_ms))
        message = "on/off durations are below this LED driver's supported minimum";
    else if ((patch->fields & UGREENCTL_LED_PATCH_TIMING) &&
             patch->value.mode==UGREENCTL_LED_MODE_BREATH &&
             (patch->value.on_ms < target->min_breath_on_ms || patch->value.off_ms < target->min_breath_off_ms))
        message = "breath durations are below this LED driver's supported minimum";
    else if ((patch->fields & UGREENCTL_LED_PATCH_TIMING) && target->max_cycle_ms &&
             patch->value.on_ms+patch->value.off_ms > target->max_cycle_ms)
        message = "on/off cycle exceeds this LED controller's supported range";
    else if ((patch->fields & UGREENCTL_LED_PATCH_TIMING) && target->timing_quantum_ms &&
             (patch->value.on_ms%target->timing_quantum_ms || patch->value.off_ms%target->timing_quantum_ms))
        message = "on/off durations must be exact multiples of this LED controller's timing quantum";
    if (message == NULL) return 0;
    if (error != NULL && error_size != 0) (void)snprintf(error, error_size, "%s", message);
    return -ENOTSUP;
}

int ugreenctl_led_validate_state_patch(const struct ugreenctl_led_profile *profile,
                                      const struct ugreenctl_led_descriptor *target,
                                      const struct ugreenctl_led_value *before,
                                      const struct ugreenctl_led_patch *patch,
                                      char *error, size_t error_size)
{
    const char *message = NULL;
    enum ugreenctl_led_mode mode;
    enum ugreenctl_led_color color;
    int result = ugreenctl_led_validate_target_patch(target,patch,error,error_size);
    if (result < 0) return result;
    if (!profile || !profile->protocol || !before) return -EINVAL;
    if (strcmp(profile->protocol,"sysfs-n76")) return 0;
    mode=(patch->fields&UGREENCTL_LED_PATCH_MODE) ? patch->value.mode : before->mode;
    color=(patch->fields&UGREENCTL_LED_PATCH_COLOR) ? patch->value.color : before->color;
    if (!(patch->fields&UGREENCTL_LED_PATCH_MODE) &&
        ((before->unknown_fields&UGREENCTL_LED_PATCH_MODE) ||
         (mode!=UGREENCTL_LED_MODE_ON && mode!=UGREENCTL_LED_MODE_OFF)))
        message="N76 changes with an unknown mode or during an effect require the complete requested mode and timing";
    else if (target->max_brightness>1 &&
             (patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS) && patch->value.brightness==1)
        message="N76 brightness 1 is an ON command rather than intensity 1; use mode on or the supported intensity range";
    else if ((mode==UGREENCTL_LED_MODE_ON || mode==UGREENCTL_LED_MODE_BLINK) &&
             !(patch->fields&UGREENCTL_LED_PATCH_COLOR) &&
             (before->unknown_fields&UGREENCTL_LED_PATCH_COLOR))
        message="N76 mode on or blink requires an explicit palette color when the current color is unknown";
    else if (mode==UGREENCTL_LED_MODE_ON && color==UGREENCTL_LED_COLOR_NONE)
        message="N76 mode on requires an explicit nonempty palette color";
    else if (mode==UGREENCTL_LED_MODE_BLINK &&
             color!=UGREENCTL_LED_COLOR_WHITE && color!=UGREENCTL_LED_COLOR_ORANGE)
        message="N76 blink requires one white or orange channel; the stock driver does not blink combined colors";
    if (!message) return 0;
    if (error && error_size) (void)snprintf(error,error_size,"%s",message);
    return -ENOTSUP;
}
