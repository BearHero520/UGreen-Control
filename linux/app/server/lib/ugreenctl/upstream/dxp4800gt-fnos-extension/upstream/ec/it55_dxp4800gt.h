#ifndef IT55_DXP4800GT_H
#define IT55_DXP4800GT_H
#include "ugreenctl.h"
#include "core/identity.h"
int it55_gt_read_fans(struct ugreenctl_fan_status *, size_t *, char *, size_t);
int it55_gt_set_fan(bool, const char *, uint8_t, char *, size_t);
int it55_gt_read_startup(enum ugreenctl_startup_policy *, char *, size_t);
int it55_gt_set_startup(bool, enum ugreenctl_startup_policy, char *, size_t);
#ifdef UGREENCTL_GT_TESTING
#include <stdio.h>
struct it55_gt_io {
    int (*identity)(struct ugreenctl_machine_identity *);
    int (*owners)(char *, size_t);
    int (*acquire)(void);
    void (*release)(void);
    uint8_t (*input)(unsigned short);
    void (*output)(uint8_t, unsigned short);
    void (*pause)(void);
};
void it55_gt_test_io(const struct it55_gt_io *);
int it55_gt_test_ioports(FILE *, char *, size_t);
int it55_gt_test_acpi_devices(const char *, char *, size_t);
#endif
#endif
