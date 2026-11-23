#ifndef UGREENCTL_HARDWARE_CLI_H
#define UGREENCTL_HARDWARE_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "led/led.h"

#define UGREENCTL_CLI_MAX_TARGETS 32U
#define UGREENCTL_CLI_MAX_ACTIONS 64U

enum ugreenctl_hardware_command {
    UGREENCTL_HW_HELP,
    UGREENCTL_HW_LED_LIST,
    UGREENCTL_HW_LED_STATUS,
    UGREENCTL_HW_LED_SET,
    UGREENCTL_HW_LED_IDENTIFY,
    UGREENCTL_HW_DISK_LIST,
    UGREENCTL_HW_DISK_LOCATE,
    UGREENCTL_HW_LED_COMPAT
};

enum ugreenctl_led_cli_action_kind {
    UGREENCTL_CLI_ON,
    UGREENCTL_CLI_OFF,
    UGREENCTL_CLI_COLOR,
    UGREENCTL_CLI_BRIGHTNESS,
    UGREENCTL_CLI_BLINK,
    UGREENCTL_CLI_BREATH,
    UGREENCTL_CLI_STATUS
};

struct ugreenctl_led_cli_action {
    enum ugreenctl_led_cli_action_kind kind;
    uint8_t red, green, blue;
    uint8_t brightness; /* Raw units, or original 0..100 when brightness_percent. */
    uint16_t on_ms, off_ms;
    bool named_color, brightness_percent;
    enum ugreenctl_led_color color;
};

struct ugreenctl_hardware_options {
    enum ugreenctl_hardware_command command;
    bool json, apply, force, compatibility;
    const char *targets[UGREENCTL_CLI_MAX_TARGETS];
    size_t target_count;
    struct ugreenctl_led_cli_action actions[UGREENCTL_CLI_MAX_ACTIONS];
    size_t action_count;
    unsigned int seconds;
    const char *map_file;
    bool n76_direct;
    unsigned int n76_bus;
    uint16_t n76_address;
};

/* Parsing performs no hardware access. -1 means a command handled by main.c;
 * -2 means invalid syntax; 0 means the complete command has been validated. */
int ugreenctl_hardware_parse(int argc, char **argv, bool compatibility,
                            struct ugreenctl_hardware_options *options,
                            char *error, size_t error_size);

/* -1 means this is not a new LED/disk command. Other values are exit codes. */
int ugreenctl_hardware_cli(int argc, char **argv);
int ugreenctl_led_compat_cli(int argc, char **argv);

#endif
