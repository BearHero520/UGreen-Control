#ifndef UGREENCTL_LED_SYSFS_H
#define UGREENCTL_LED_SYSFS_H

#include "led/led.h"

struct ugreenctl_led_sysfs;
/* Internal transport. Production callers always pass /sys after real machine
 * identity checks; no CLI/environment path override exists. */
int ugreenctl_led_sysfs_open(const struct ugreenctl_led_profile *profile,
                            const char *root,
                            int (*authorize)(void *, char *, size_t), void *opaque,
                            struct ugreenctl_led_sysfs **out,
                            char *error, size_t size);
int ugreenctl_led_sysfs_read(struct ugreenctl_led_sysfs *context, uint8_t id,
                            struct ugreenctl_led_value *value,
                            char *error, size_t size);
int ugreenctl_led_sysfs_preflight(struct ugreenctl_led_sysfs *context, uint8_t id,
                                 const struct ugreenctl_led_value *before,
                                 const struct ugreenctl_led_patch *patch,
                                 char *error, size_t size);
int ugreenctl_led_sysfs_apply(struct ugreenctl_led_sysfs *context, uint8_t id,
                             const struct ugreenctl_led_patch *patch,
                             struct ugreenctl_led_value *value,
                             char *error, size_t size);
bool ugreenctl_led_sysfs_identity_confirmed(struct ugreenctl_led_sysfs *context);
void ugreenctl_led_sysfs_close(struct ugreenctl_led_sysfs *context);
int ugreenctl_led_sysfs_identify(struct ugreenctl_led_sysfs *context,uint8_t id,unsigned int seconds,
    const volatile sig_atomic_t *cancelled,int (*authorize)(void *,char *,size_t),
    void (*pause_ms)(void *,unsigned int),void *opaque,char *error,size_t size);
#ifdef UGREENCTL_LED_TESTING
void ugreenctl_led_sysfs_test_observer(struct ugreenctl_led_sysfs *context,
    void (*observer)(void *,const char *,const char *),void *opaque);
#endif

#endif
