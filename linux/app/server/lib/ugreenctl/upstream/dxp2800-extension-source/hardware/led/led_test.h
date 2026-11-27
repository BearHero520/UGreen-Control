#ifndef UGREENCTL_LED_TEST_H
#define UGREENCTL_LED_TEST_H

/* Only built into the dedicated fixture test binary. The production binary
 * has no path or environment override for hardware identity or transports. */
#ifndef UGREENCTL_LED_TESTING
#error "LED test transport must not be included in production builds"
#endif

#include "led/led.h"

struct ugreenctl_led_test_io {
    int (*owners)(void *opaque);
    int (*lock)(void *opaque);
    void (*unlock)(void *opaque);
    int (*read_chip_id)(void *opaque, uint16_t *chip_id);
    int (*read_status)(void *opaque, uint8_t id, uint8_t frame[11]);
    int (*write_command)(void *opaque, uint8_t id, const uint8_t frame[12]);
    int (*read_ack)(void *opaque);
    void (*pause_ms)(void *opaque, unsigned int milliseconds);
    int (*read_identity)(void *opaque, struct ugreenctl_led_identity *identity,
                         char *error, size_t size);
    int (*read_byte)(void *opaque, uint8_t reg, uint8_t *value);
    int (*write_byte)(void *opaque, uint8_t reg, uint8_t value);
};

bool ugreenctl_led_test_process_is_owner(const char *comm, const char *executable,
                                         const char *const *arguments, size_t count);

int ugreenctl_led_test_open(const struct ugreenctl_led_identity *identity,
                           const struct ugreenctl_led_access *access,
                           const struct ugreenctl_led_test_io *io, void *opaque,
                           struct ugreenctl_led_session **session,
                           char *error, size_t error_size);
int ugreenctl_led_test_open_sysfs(const struct ugreenctl_led_identity *identity,
                                 const struct ugreenctl_led_access *access,
                                 const struct ugreenctl_led_test_io *io,void *opaque,
                                 const char *sysfs_root,struct ugreenctl_led_session **session,
                                 char *error,size_t size);
void ugreenctl_led_test_sysfs_observer(struct ugreenctl_led_session *session,
    void (*observer)(void *,const char *,const char *),void *opaque);

#endif
