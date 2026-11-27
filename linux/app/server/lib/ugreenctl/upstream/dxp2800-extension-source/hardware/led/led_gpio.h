#ifndef UGREENCTL_LED_GPIO_H
#define UGREENCTL_LED_GPIO_H

#include "led/led.h"
#include <sys/types.h>

/* Persistent holder transport. The context owns the fixed controller flock
 * and GPIO v2 line FDs until close; CLI calls must use the holder service.
 * Open requests lines as-is and does not change electrical configuration. */
struct ugreenctl_led_gpio;
int ugreenctl_led_gpio_open(const struct ugreenctl_led_access *access,
                           struct ugreenctl_led_gpio **context,
                           char *error,size_t error_size);
const struct ugreenctl_led_identity *ugreenctl_led_gpio_identity(
    const struct ugreenctl_led_gpio *context);
bool ugreenctl_led_gpio_identity_confirmed(struct ugreenctl_led_gpio *context);
int ugreenctl_led_gpio_read(struct ugreenctl_led_gpio *context,uint8_t id,
                           struct ugreenctl_led_value *value,char *error,size_t error_size);
int ugreenctl_led_gpio_apply(struct ugreenctl_led_gpio *context,uint8_t id,
                            const struct ugreenctl_led_patch *patch,
                            struct ugreenctl_led_value *readback,char *error,size_t error_size);
void ugreenctl_led_gpio_close(struct ugreenctl_led_gpio *context);
/* PID must come from the holder's authenticated SO_PEERCRED connection. */
void ugreenctl_led_gpio_set_client(struct ugreenctl_led_gpio *context,pid_t authenticated_pid);

/* Shared production process-owner check; no caller-supplied path overrides. */
int ugreenctl_led_process_owners(void);
int ugreenctl_led_process_owners_except(pid_t authenticated_pid);

#ifdef UGREENCTL_LED_TESTING
struct ugreenctl_led_gpio_test_io {
    int (*open_chip)(void *opaque,const char *path);
    int (*control)(void *opaque,int fd,unsigned long request,void *argument);
    void (*close_fd)(void *opaque,int fd);
    int (*owners)(void *opaque);
    int (*lock)(void *opaque);
    void (*unlock)(void *opaque);
};
int ugreenctl_led_gpio_test_open(const char *sysfs_root,const char *device_root,
    const struct ugreenctl_led_access *access,const struct ugreenctl_led_gpio_test_io *io,
    void *opaque,struct ugreenctl_led_gpio **context,char *error,size_t error_size);
#endif

#endif
