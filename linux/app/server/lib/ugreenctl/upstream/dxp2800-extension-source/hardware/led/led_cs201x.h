#ifndef UGREENCTL_LED_CS201X_H
#define UGREENCTL_LED_CS201X_H

#include "led/led.h"
#include <sys/types.h>

/* Independent userspace implementation of the published CS2011 interface.
 * Production uses real DMI and fixed locks/ports; no path/env override exists. */
struct ugreenctl_led_cs201x;
int ugreenctl_led_cs201x_open(const struct ugreenctl_led_access *access,
    struct ugreenctl_led_cs201x **context,char *error,size_t size);
/* Explicit foreground-holder lifecycle. Saves all four raw LED states;
 * takeover_flow permits only a verified FLOW 0 -> 1 transition, never LDN
 * activation. restore_close must be used to report restoration failures. */
int ugreenctl_led_cs201x_open_holder(const struct ugreenctl_led_access *access,
    bool takeover_flow,struct ugreenctl_led_cs201x **context,char *error,size_t size);
int ugreenctl_led_cs201x_restore_close(struct ugreenctl_led_cs201x *context,
    char *error,size_t size);
/* Only pass a PID already authenticated by the local socket's SO_PEERCRED. */
void ugreenctl_led_cs201x_set_client(struct ugreenctl_led_cs201x *context,pid_t authenticated_pid);
const struct ugreenctl_led_identity *ugreenctl_led_cs201x_identity(
    const struct ugreenctl_led_cs201x *context);
bool ugreenctl_led_cs201x_identity_confirmed(const struct ugreenctl_led_cs201x *context);
uint16_t ugreenctl_led_cs201x_chip_id(const struct ugreenctl_led_cs201x *context);
const char *ugreenctl_led_cs201x_bus(const struct ugreenctl_led_cs201x *context);
int ugreenctl_led_cs201x_read(struct ugreenctl_led_cs201x *context,uint8_t id,
    struct ugreenctl_led_value *value,char *error,size_t size);
int ugreenctl_led_cs201x_apply(struct ugreenctl_led_cs201x *context,uint8_t id,
    const struct ugreenctl_led_patch *patch,struct ugreenctl_led_value *readback,
    char *error,size_t size);
int ugreenctl_led_cs201x_identify(struct ugreenctl_led_cs201x *context,uint8_t id,
    unsigned int seconds,const volatile sig_atomic_t *cancelled,char *error,size_t size);
/* BEGIN snapshots seven raw bytes and starts a 200/200 ms blue blink.
 * END restores exactly those bytes. One saved identify per context; APPLY
 * is refused until END succeeds. END without a saved identify is ENOENT. */
int ugreenctl_led_cs201x_begin_identify(struct ugreenctl_led_cs201x *context,uint8_t id,
    char *error,size_t size);
int ugreenctl_led_cs201x_end_identify(struct ugreenctl_led_cs201x *context,uint8_t id,
    char *error,size_t size);
void ugreenctl_led_cs201x_close(struct ugreenctl_led_cs201x *context);

#ifdef UGREENCTL_LED_TESTING
struct ugreenctl_led_cs201x_test_io {
    int (*detect)(void *,struct ugreenctl_led_identity *,char *,size_t);
    int (*owners)(void *,uint16_t runtime_base);
    int (*lock)(void *);
    void (*unlock)(void *);
    int (*permission)(void *,uint16_t port,unsigned int count,bool enable);
    int (*read_port)(void *,uint16_t port,uint8_t *value);
    int (*write_port)(void *,uint16_t port,uint8_t value);
    void (*pause_ms)(void *,unsigned int milliseconds);
};
int ugreenctl_led_cs201x_test_open(const struct ugreenctl_led_access *access,
    const struct ugreenctl_led_cs201x_test_io *io,void *opaque,
    struct ugreenctl_led_cs201x **context,char *error,size_t size);
int ugreenctl_led_cs201x_test_open_holder(const struct ugreenctl_led_access *access,
    bool takeover_flow,const struct ugreenctl_led_cs201x_test_io *io,void *opaque,
    struct ugreenctl_led_cs201x **context,char *error,size_t size);
#endif
#endif
