#include "core/identity.h"
#include "fan/it8613_dxp2800.h"
#include "fan/it8613_hwmon.h"
#include "fan/it8613_direct.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static const char *product = "DXP2800", *version = "EM_DXP2800_V1.0.26";
static int identity_result, hwmon_result = -ENODEV, hwmon_calls, direct_calls;
static uint8_t duty;
int ugreenctl_identity_read(struct ugreenctl_machine_identity *id, char *e, size_t n)
{
    (void)e; (void)n;
    memset(id, 0, sizeof(*id));
    snprintf(id->product_name, sizeof(id->product_name), "%s", product);
    snprintf(id->product_version, sizeof(id->product_version), "%s", version);
    return identity_result;
}
static void hwmap(const struct it8613_hwmon_channel *c, size_t count)
{
    CHECK(count == 1 && strcmp(c->id, "sys") == 0);
    CHECK(c->pwm_index == 3 && c->fan_index == 3); ++hwmon_calls;
}
static void directmap(const struct it8613_direct_channel *c, size_t count)
{
    CHECK(count == 1 && strcmp(c->id, "sys") == 0 && c->pwm_supported);
    CHECK(c->control_register == 0x17 && c->duty_register == 0x73);
    CHECK(c->tachometer_low_register == 0x0f && c->tachometer_high_register == 0x1a);
    ++direct_calls;
}
int it8613_hwmon_read_fans(const struct it8613_hwmon_channel *c, size_t count,
                           struct ugreenctl_fan_status *fans, size_t *n, char *e, size_t s)
{ (void)fans; (void)n; (void)e; (void)s; hwmap(c, count); return hwmon_result; }
int it8613_hwmon_set_manual_pwm(const struct it8613_hwmon_channel *c, size_t n,
                                uint8_t pwm, char *e, size_t s)
{ (void)e; (void)s; hwmap(c, n); duty = pwm; return hwmon_result; }
int it8613_direct_read_fans(const struct it8613_direct_channel *c, size_t count,
                            struct ugreenctl_fan_status *fans, size_t *n, char *e, size_t s)
{ (void)fans; (void)e; (void)s; directmap(c, count); *n = count; return 0; }
int it8613_direct_set_manual_pwm(const struct it8613_direct_channel *c, size_t n,
                                 uint8_t pwm, char *e, size_t s)
{ (void)e; (void)s; directmap(c, n); duty = pwm; return 0; }

int main(void)
{
    struct ugreenctl_fan_status fans[UGREENCTL_MAX_FANS];
    size_t n = 0; char error[256]; int before;
    CHECK(it8613_dxp2800_read_fan(fans, &n, error, sizeof(error)) == 0 && n == 1);
    CHECK(direct_calls == 1 && hwmon_calls == 1);
    CHECK(it8613_dxp2800_set_fan_pwm("sys", 40, error, sizeof(error)) == 0 && duty == 40);
    CHECK(it8613_dxp2800_set_fan_pwm("sys", 255, error, sizeof(error)) == 0 && duty == 255);
    before = direct_calls;
    for (int i = 0; i < 4; ++i) {
        const int results[] = {0, -EIO, -EACCES, -EBUSY};
        hwmon_result = results[i];
        CHECK(it8613_dxp2800_read_fan(fans, &n, error, sizeof(error)) == results[i]);
        CHECK(it8613_dxp2800_set_fan_pwm("sys", 128, error, sizeof(error)) == results[i]);
        CHECK(direct_calls == before);
    }
    before = hwmon_calls;
    CHECK(it8613_dxp2800_set_fan_pwm("cpu", 128, error, sizeof(error)) == -EINVAL);
    CHECK(it8613_dxp2800_set_fan_pwm("all", 128, error, sizeof(error)) == -EINVAL);
    CHECK(it8613_dxp2800_set_fan_pwm(NULL, 128, error, sizeof(error)) == -EINVAL);
    CHECK(it8613_dxp2800_set_fan_pwm("sys", 39, error, sizeof(error)) == -EINVAL);
    CHECK(hwmon_calls == before);
    for (int i = 0; i < 4; ++i) {
        const char *versions[] = {"", "EM_DXP2800_V1.0.24", "DXP2800 V1.2 251215", "EM_DXP2800_V1.0.26-extra"};
        version = versions[i];
        CHECK(it8613_dxp2800_read_fan(fans, &n, error, sizeof(error)) == -ENODEV);
        CHECK(it8613_dxp2800_set_fan_pwm("sys", 128, error, sizeof(error)) == -ENODEV);
    }
    version = "EM_DXP2800_V1.0.26"; product = "DXP2800 GT";
    CHECK(it8613_dxp2800_read_fan(fans, &n, error, sizeof(error)) == -ENODEV);
    product = "DXP2800"; identity_result = -EACCES;
    CHECK(it8613_dxp2800_read_fan(fans, &n, error, sizeof(error)) == -EACCES);
    CHECK(hwmon_calls == before);
    puts("DXP2800 EM26 identity, map and hwmon/direct isolation passed");
    return 0;
}
