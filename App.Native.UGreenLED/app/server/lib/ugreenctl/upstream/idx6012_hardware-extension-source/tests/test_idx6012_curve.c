#define main fan_daemon_main
#include "core/fan_daemon.c"
#undef main
#include <assert.h>
int main(void) {
    struct ugreenctl_fan_curve_config config;
    ugreenctl_fan_curve_config_defaults(&config);
    const struct model_route *route = find_model_route("iDX6011/iDX6012 OEM V3.0");
    assert(route && route->requires_force);
    assert(!strcmp(route->targets[0].name, "sys") && !route->targets[1].name);
    assert(!find_model_route("Default string"));
    strcpy(config.profile, "custom");
    config.minimum_pwm = 40;
    config.failsafe_pwm = 198;
    config.pwm = (struct ugreenctl_curve_pwm_points){64,128,170,198};
    assert(profile_matches_route(&config, route) == 0);
    config.pwm.maximum = 199;
    assert(profile_matches_route(&config, route) != 0);
    config.pwm.maximum = 198;
    config.failsafe_pwm = 255;
    assert(profile_matches_route(&config, route) != 0);
    config.failsafe_pwm = 198;
    strcpy(config.profile, "stock-6800pro");
    assert(profile_matches_route(&config, route) != 0);
    char path[] = "/tmp/idx6012-curve-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    const char *text = "profile=stock-idx6012-compat\n";
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
    char error[256];
    assert(ugreenctl_fan_curve_load_config(path, &config, error, sizeof(error)) == 0);
    unlink(path);
    assert(profile_matches_route(&config, route) == 0);
    assert(profile_matches_route(&config, find_model_route("DXP6800 Pro")) != 0);
    assert(config.minimum_pwm == 64 && config.failsafe_pwm == 198);
    assert(config.cpu.start == 38 && config.hdd.full == 48 && config.ssd.maximum == 70);
    struct ugreenctl_thermal_snapshot sample = {95,60,75,95};
    struct ugreenctl_fan_curve_plan plan;
    assert(ugreenctl_fan_curve_evaluate_plan(&config, &sample, &plan, error, sizeof(error)) == 0);
    assert(plan.system_pwm == 198);
    sample = (struct ugreenctl_thermal_snapshot){20,20,20,20};
    assert(ugreenctl_fan_curve_evaluate_plan(&config, &sample, &plan, error, sizeof(error)) == 0);
    assert(plan.system_pwm == 64);
    sample = (struct ugreenctl_thermal_snapshot){-1,-1,-1,-1};
    ugreenctl_fan_curve_evaluate_plan(&config, &sample, &plan, error, sizeof(error));
    assert(plan.system_pwm == 198);
    return 0;
}
