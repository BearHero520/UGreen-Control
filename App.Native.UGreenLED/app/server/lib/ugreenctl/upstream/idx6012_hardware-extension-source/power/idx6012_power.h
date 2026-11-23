#ifndef IDX6012_POWER_H
#define IDX6012_POWER_H
#include "ugreenctl.h"
#include "core/identity.h"
int idx6012_power_identity(const struct ugreenctl_machine_identity *);
int idx6012_read_startup(enum ugreenctl_startup_policy *, char *, size_t);
int idx6012_set_startup(bool, enum ugreenctl_startup_policy, char *, size_t);
struct idx6012_power_io {
 int (*open)(void); void (*close)(void);
 int (*read)(uint32_t *); int (*write)(uint32_t);
};
#ifdef IDX6012_POWER_TESTING
void idx6012_power_test_io(const struct idx6012_power_io *);
#endif
#endif
