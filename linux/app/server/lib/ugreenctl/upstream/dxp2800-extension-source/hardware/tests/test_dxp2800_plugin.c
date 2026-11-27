#include "ugreenctl.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
const struct ugreenctl_plugin *ugreenctl_plugin_v5(void);
static int calls;
static int identity_result;
int it8613_dxp2800_require_identity(char *e, size_t s)
{ (void)e; (void)s; return identity_result; }
int it8613_read_startup_policy(bool force, enum ugreenctl_startup_policy *p, char *e, size_t s)
{ (void)force; (void)e; (void)s; *p = UGREENCTL_STARTUP_RESTORE; ++calls; return 0; }
int it8613_set_startup_policy(bool force, enum ugreenctl_startup_policy p, char *e, size_t s)
{ (void)e; (void)s; CHECK(force && p == UGREENCTL_STARTUP_ON); ++calls; return 0; }
int it8613_dxp2800_read_fan(struct ugreenctl_fan_status *f, size_t *n, char *e, size_t s)
{ (void)f; (void)n; (void)e; (void)s; ++calls; return -EIO; }
int it8613_dxp2800_set_fan_pwm(const char *id, uint8_t pwm, char *e, size_t s)
{ (void)e; (void)s; CHECK(strcmp(id, "sys") == 0 && pwm == 128); ++calls; return -EIO; }
int main(void)
{
    const struct ugreenctl_plugin *p = ugreenctl_plugin_v5();
    struct ugreenctl_request r = {0}; char error[256];
    CHECK(p->abi_version == UGREENCTL_PLUGIN_ABI_V5 && strcmp(p->id, "dxp2800") == 0);
    CHECK(strcmp(p->dmi_product_names[0], "DXP2800") == 0 && p->dmi_product_names[1] == NULL);
    CHECK(p->capabilities == (UGREENCTL_CAP_FAN | UGREENCTL_CAP_POWER) && p->read_fans != NULL);
    CHECK(p->set_led == NULL && p->set_startup_policy != NULL && p->set_fan_mode == NULL);
    CHECK(p->set_fan_pwm(&r, "sys", 128, error, sizeof(error)) == -EPERM);
    CHECK(calls == 0);
    CHECK(p->set_startup_policy(&r, UGREENCTL_STARTUP_ON, error, sizeof(error)) == -EPERM && calls == 0);
    r.force = true;
    CHECK(p->set_fan_pwm(&r, "sys", 128, error, sizeof(error)) == -EIO && calls == 1);
    identity_result = -ENODEV;
    CHECK(p->set_startup_policy(&r, UGREENCTL_STARTUP_ON, error, sizeof(error)) == -ENODEV && calls == 1);
    enum ugreenctl_startup_policy policy;
    CHECK(p->read_startup_policy(&r, &policy, error, sizeof(error)) == -ENODEV && calls == 1);
    identity_result = 0;
    CHECK(p->set_startup_policy(&r, UGREENCTL_STARTUP_ON, error, sizeof(error)) == 0 && calls == 2);
    CHECK(p->read_startup_policy(&r, &policy, error, sizeof(error)) == 0 && policy == UGREENCTL_STARTUP_RESTORE && calls == 3);
    puts("DXP2800 plugin capability and write guards passed");
    return 0;
}
