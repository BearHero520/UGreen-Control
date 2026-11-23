#ifndef UGREENCTL_LED_SERVICE_H
#define UGREENCTL_LED_SERVICE_H

#include "led/led.h"

#define UGREENCTL_LED_SERVICE_SOCKET "/run/ugreenctl-ledd.sock"
struct ugreenctl_led_service_client;

/* The foreground holder owns controller resources; clients keep one exclusive
 * connection for the entire LED session, including identify and restoration.
 * GT global flow takeover is explicit and never enables an inactive LDN. */
int ugreenctl_led_service_run(const struct ugreenctl_led_access *access,
                              bool takeover_flow,
                              const volatile sig_atomic_t *stop,
                              char *error, size_t size);
int ugreenctl_led_service_connect(const struct ugreenctl_led_identity *identity,
                                  const struct ugreenctl_led_access *access,
                                  struct ugreenctl_led_service_client **client,
                                  char *error, size_t size);
bool ugreenctl_led_service_identity_confirmed(const struct ugreenctl_led_service_client *client);
int ugreenctl_led_service_read(struct ugreenctl_led_service_client *client,
                               uint8_t id, struct ugreenctl_led_value *value,
                               char *error, size_t size);
int ugreenctl_led_service_apply(struct ugreenctl_led_service_client *client,
                                uint8_t id, const struct ugreenctl_led_patch *patch,
                                struct ugreenctl_led_value *value,
                                char *error, size_t size);
int ugreenctl_led_service_identify(struct ugreenctl_led_service_client *client,
                                   uint8_t id, unsigned int seconds,
                                   const volatile sig_atomic_t *cancelled,
                                   char *error, size_t size);
void ugreenctl_led_service_close(struct ugreenctl_led_service_client *client);

#endif
