#ifndef UGREENCTL_LED_H
#define UGREENCTL_LED_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>

#include "ugreenctl.h"

int ugreenctl_led_not_implemented(char *error, size_t error_size);

/* LED MCU modes are the wire values, independent of the older plugin ABI. */
enum ugreenctl_led_mode {
    UGREENCTL_LED_MODE_OFF = 0,
    UGREENCTL_LED_MODE_ON = 1,
    UGREENCTL_LED_MODE_BLINK = 2,
    UGREENCTL_LED_MODE_BREATH = 3
};

#define UGREENCTL_LED_MODE_BIT(mode) (1U << (unsigned int)(mode))
#define UGREENCTL_LED_MODES_ALL 0x0fU

enum ugreenctl_led_color {
    UGREENCTL_LED_COLOR_NONE = 0,
    UGREENCTL_LED_COLOR_WHITE = 1,
    UGREENCTL_LED_COLOR_ORANGE = 2,
    UGREENCTL_LED_COLOR_WHITE_ORANGE = 3,
    UGREENCTL_LED_COLOR_RED = 4,
    UGREENCTL_LED_COLOR_GREEN = 5,
    UGREENCTL_LED_COLOR_BLUE = 6,
    UGREENCTL_LED_COLOR_CHANNEL1 = 7,
    UGREENCTL_LED_COLOR_CHANNEL2 = 8,
    UGREENCTL_LED_COLOR_BOTH = 9,
    UGREENCTL_LED_COLOR_MAX = UGREENCTL_LED_COLOR_BOTH
};
#define UGREENCTL_LED_COLOR_BIT(color) (1U << (unsigned int)(color))

enum ugreenctl_led_chip_read {
    UGREENCTL_LED_CHIP_WORD_LE = 0,
    UGREENCTL_LED_CHIP_BYTES_BE = 1,
    UGREENCTL_LED_CHIP_DRIVER_PROBE = 2
};

struct ugreenctl_led_descriptor;

struct ugreenctl_led_profile {
    const char *id;
    const char *product_name;
    const char *product_version; /* NULL: no revision restriction. */
    const char *protocol;
    unsigned int disk_count;
    bool experimental;
    const char *evidence;
    const struct ugreenctl_led_descriptor *leds;
    size_t led_count;
    const char *bus_prefix;
    uint16_t i2c_address;
    uint16_t expected_chip_id;
    uint8_t chip_register;
    enum ugreenctl_led_chip_read chip_read;
    const char *sysfs_driver; /* Bound driver's exact kernel name, if used. */
    const char *dt_compatible; /* Exact token in a real device-tree compatible list. */
};

struct ugreenctl_led_identity {
    char manufacturer[128];
    char product_name[128];
    char product_version[128];
    char board_version[128];
    const struct ugreenctl_led_profile *profile;
    char source[16];
    char compatible[512];
    char bios_vendor[128];
    char bios_version[128];
    char bios_date[128];
};

/* Session stability only: equal BIOS text is not proof of compatibility with
 * an untested firmware revision. Never use this as a version allowlist. */
static inline bool ugreenctl_led_same_identity(const struct ugreenctl_led_identity *a,
                                              const struct ugreenctl_led_identity *b)
{
    return a && b && a->profile && b->profile && a->profile->id && b->profile->id &&
        !strcmp(a->profile->id,b->profile->id) &&
        !strcmp(a->source,b->source) && !strcmp(a->manufacturer,b->manufacturer) &&
        !strcmp(a->product_name,b->product_name) && !strcmp(a->product_version,b->product_version) &&
        !strcmp(a->board_version,b->board_version) && !strcmp(a->compatible,b->compatible) &&
        !strcmp(a->bios_vendor,b->bios_vendor) && !strcmp(a->bios_version,b->bios_version) &&
        !strcmp(a->bios_date,b->bios_date);
}

struct ugreenctl_led_descriptor {
    const char *name; /* power, netdev, disk1 ... disk8 */
    uint8_t id;
    unsigned int bay; /* zero for indicators other than a disk bay */
    bool rgb;
    unsigned int mode_mask;
    unsigned int max_brightness;
    unsigned int color_mask;
    const char *sysfs_name;
    unsigned int min_on_ms, min_off_ms;
    unsigned int min_brightness;
    unsigned int min_breath_on_ms, min_breath_off_ms;
    unsigned int max_cycle_ms, timing_quantum_ms;
};

struct ugreenctl_led_value {
    enum ugreenctl_led_mode mode;
    unsigned int brightness;
    unsigned int red, green, blue;
    unsigned int on_ms, off_ms;
    enum ugreenctl_led_color color;
    unsigned int unknown_fields;
    unsigned int cached_fields;
    /* Direct N76: mode registers are readable, actual outputs are not.
     * Never replace unknown_fields with the user's requested values. */
    bool n76_mode_valid, unverified_write;
    uint8_t n76_modes[2], n76_version;
};

enum ugreenctl_led_patch_field {
    UGREENCTL_LED_PATCH_MODE = 1U << 0,
    UGREENCTL_LED_PATCH_RGB = 1U << 1,
    UGREENCTL_LED_PATCH_BRIGHTNESS = 1U << 2,
    UGREENCTL_LED_PATCH_TIMING = 1U << 3,
    UGREENCTL_LED_PATCH_COLOR = 1U << 4
};

struct ugreenctl_led_patch {
    unsigned int fields;
    struct ugreenctl_led_value value;
};

struct ugreenctl_led_access {
    bool force;
    bool apply;
    bool n76_direct;
    unsigned int n76_bus;
    uint16_t n76_address;
};

struct ugreenctl_led_session;

const struct ugreenctl_led_profile *ugreenctl_led_profiles(size_t *count);
const struct ugreenctl_led_profile *ugreenctl_led_profile_find(
    const char *product_name, const char *product_version);
int ugreenctl_led_detect(struct ugreenctl_led_identity *identity,
                        char *error, size_t error_size);
size_t ugreenctl_led_descriptors(const struct ugreenctl_led_profile *profile,
                               struct ugreenctl_led_descriptor *out,
                               size_t capacity);
const char *ugreenctl_led_mode_name(enum ugreenctl_led_mode mode);
const char *ugreenctl_led_color_name(enum ugreenctl_led_color color);
int ugreenctl_led_validate_target_patch(const struct ugreenctl_led_descriptor *target,
                                       const struct ugreenctl_led_patch *patch,
                                       char *error, size_t error_size);
/* Pure preflight against a previously read state. Backends must repeat this
 * check against current state before writing; it is not an authorization. */
int ugreenctl_led_validate_state_patch(const struct ugreenctl_led_profile *profile,
                                      const struct ugreenctl_led_descriptor *target,
                                      const struct ugreenctl_led_value *before,
                                      const struct ugreenctl_led_patch *patch,
                                      char *error, size_t error_size);
int ugreenctl_led_validate_patch(const struct ugreenctl_led_patch *patch,
                                char *error, size_t error_size);
/* Production open always detects real DMI/DT; no environment/path overrides. */
int ugreenctl_led_open(const struct ugreenctl_led_access *access,
                      struct ugreenctl_led_session **session,
                      char *error, size_t error_size);
const struct ugreenctl_led_identity *ugreenctl_led_identity(
    const struct ugreenctl_led_session *session);
uint16_t ugreenctl_led_chip_id(const struct ugreenctl_led_session *session);
bool ugreenctl_led_identity_confirmed(const struct ugreenctl_led_session *session);
const char *ugreenctl_led_bus(const struct ugreenctl_led_session *session);
int ugreenctl_led_read(struct ugreenctl_led_session *session, uint8_t id,
                      struct ugreenctl_led_value *value,
                      char *error, size_t error_size);
/* Read-only plan check, including registered backend capabilities. `before`
 * may be a planned legacy state. Actual writes still authorize and recheck
 * live state; this does not reserve triggers or promise atomic execution. */
int ugreenctl_led_preflight(struct ugreenctl_led_session *session, uint8_t id,
                           const struct ugreenctl_led_value *before,
                           const struct ugreenctl_led_patch *patch,
                           char *error, size_t error_size);
int ugreenctl_led_apply(struct ugreenctl_led_session *session, uint8_t id,
                       const struct ugreenctl_led_patch *patch,
                       struct ugreenctl_led_value *readback,
                       char *error, size_t error_size);
/* Keeps the controller lock throughout identify and best-effort restoration. */
int ugreenctl_led_identify(struct ugreenctl_led_session *session, uint8_t id,
                          unsigned int seconds,
                          const volatile sig_atomic_t *cancelled,
                          char *error, size_t error_size);
void ugreenctl_led_close(struct ugreenctl_led_session *session);

#endif
