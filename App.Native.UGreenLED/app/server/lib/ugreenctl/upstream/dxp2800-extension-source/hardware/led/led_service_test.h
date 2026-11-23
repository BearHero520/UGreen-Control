#ifndef UGREENCTL_LED_SERVICE_TEST_H
#define UGREENCTL_LED_SERVICE_TEST_H
#include "led/led_service.h"
#include <sys/types.h>

struct ugreenctl_led_service_test_ops {
    void *context;
    const struct ugreenctl_led_identity *identity;
    bool (*confirmed)(void *);
    int (*read)(void *,uint8_t,struct ugreenctl_led_value *,char *,size_t);
    int (*apply)(void *,uint8_t,const struct ugreenctl_led_patch *,struct ugreenctl_led_value *,char *,size_t);
    void (*client)(void *,pid_t);
    int (*begin_identify)(void *,uint8_t,char *,size_t);
    int (*end_identify)(void *,uint8_t,char *,size_t);
};
int ugreenctl_led_service_test_serve(int fd,const struct ugreenctl_led_service_test_ops *ops);
int ugreenctl_led_service_test_connect(int fd,const struct ugreenctl_led_identity *identity,
                                      const struct ugreenctl_led_access *access,
                                      struct ugreenctl_led_service_client **client,char *error,size_t size);
#endif
