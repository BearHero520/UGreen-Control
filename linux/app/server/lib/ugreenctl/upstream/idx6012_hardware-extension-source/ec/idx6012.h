#ifndef UGREENCTL_IDX6012_H
#define UGREENCTL_IDX6012_H
#include "ugreenctl.h"
#include "core/identity.h"
int idx6012_read_fans(struct ugreenctl_fan_status *, size_t *, char *, size_t);
/* Diagnostic G3 gate, deliberately NOT the startup policy API. */
int idx6012_read_g3(unsigned int *, char *, size_t);
int idx6012_set_fan(bool, const char *, uint8_t, char *, size_t);
int idx6012_set_mode(bool, const char *, bool, char *, size_t);
#ifdef UGREENCTL_IDX6012_TESTING
struct idx6012_io {
    int (*identity)(struct ugreenctl_machine_identity *);
    int (*owners)(void);
    int (*acquire)(void);
    void (*release)(void);
    int (*read_port)(unsigned short, uint8_t *);
    int (*send_port)(unsigned short, uint8_t);
    void (*pause)(void);
};
void idx6012_test_io(const struct idx6012_io *);
#endif
#endif
