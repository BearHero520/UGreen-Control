#define _POSIX_C_SOURCE 200809L
/* Runs only inside the disposable QEMU guest. All PWM paths are /tmp files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void put(const char *path, const char *text)
{ FILE *f = fopen(path, "w"); CHECK(f); CHECK(fputs(text, f) >= 0); CHECK(!fclose(f)); }
static int number(const char *path)
{ int n; FILE *f = fopen(path, "r"); CHECK(f && fscanf(f, "%d", &n) == 1); CHECK(!fclose(f)); return n; }
static int run(const char *command)
{ int status = system(command); CHECK(status >= 0 && WIFEXITED(status)); return WEXITSTATUS(status); }

int main(int argc, char **argv)
{
    const int reject = argc > 1 && !strcmp(argv[1], "reject");
    CHECK(!mkdir("/tmp/hw", 0700)); CHECK(!mkdir("/tmp/hw/hwmon0", 0700));
    CHECK(!mkdir("/tmp/plugins", 0700)); CHECK(!mkdir("/tmp/no-thermal", 0700));
    CHECK(!symlink("/work/models/dxp2800.so", "/tmp/plugins/dxp2800.so"));
    put("/tmp/hw/hwmon0/name", "it8613\n");
    put("/tmp/hw/hwmon0/pwm3", "64\n");
    put("/tmp/hw/hwmon0/pwm3_enable", "2\n");
    put("/tmp/hw/hwmon0/fan3_input", "900\n");
    CHECK(!setenv("UGREENCTL_HWMON_ROOT", "/tmp/hw", 1));
    CHECK(!setenv("UGREENCTL_IT8613_LOCK", "/tmp/fan.lock", 1));
    CHECK(!setenv("UGREENCTL_THERMAL_HWMON_ROOT", "/tmp/no-thermal", 1));
    CHECK(!setenv("UGREENCTL_THERMAL_BLOCK_ROOT", "/tmp/no-thermal", 1));
    if (reject) {
        CHECK(run("/work/models/cli.so --plugin-dir /tmp/plugins fan status") != 0);
        CHECK(run("/work/models/cli.so --plugin-dir /tmp/plugins --force --apply fan set sys 128") != 0);
        CHECK(run("/work/models/cli.so --plugin-dir /tmp/plugins power startup get") != 0);
        CHECK(run("/work/models/cli.so --plugin-dir /tmp/plugins --force --apply power startup set on") != 0);
        CHECK(number("/tmp/hw/hwmon0/pwm3") == 64 && number("/tmp/hw/hwmon0/pwm3_enable") == 2);
        puts("Real CLI/plugin rejects a different DMI revision before any fixture write");
        return 0;
    }
    CHECK(!run("/work/models/cli.so --plugin-dir /tmp/plugins fan status"));
    CHECK(!run("/work/models/cli.so --plugin-dir /tmp/plugins --force fan set sys 128"));
    CHECK(number("/tmp/hw/hwmon0/pwm3") == 64 && number("/tmp/hw/hwmon0/pwm3_enable") == 2);
    CHECK(run("/work/models/cli.so --plugin-dir /tmp/plugins --apply fan set sys 128") != 0);
    CHECK(number("/tmp/hw/hwmon0/pwm3") == 64);
    CHECK(!run("/work/models/cli.so --plugin-dir /tmp/plugins --force --apply fan set sys 128"));
    CHECK(number("/tmp/hw/hwmon0/pwm3") == 128 && number("/tmp/hw/hwmon0/pwm3_enable") == 1);
    CHECK(run("/work/models/cli.so --plugin-dir /tmp/plugins --force --apply fan set cpu 128") != 0);
    CHECK(run("/work/models/cli.so --plugin-dir /tmp/plugins --force --apply fan set sys 39") != 0);
    CHECK(number("/tmp/hw/hwmon0/pwm3") == 128);
    CHECK(!run("/work/models/cli.so --plugin-dir /tmp/plugins power startup set restore"));
    CHECK(run("/work/models/cli.so --plugin-dir /tmp/plugins --apply power startup set on") != 0);
    CHECK(!run("/work/models/cli.so network wol interfaces"));
    CHECK(run("/work/models/cli.so network wol set on") != 0);
    CHECK(!run("/work/models/cli.so --interface enp2s0 network wol set on"));
    CHECK(run("/work/models/cli.so --force --apply --interface lo network wol set on") != 0);
    char rtc_command[256];
    snprintf(rtc_command, sizeof(rtc_command), "/work/models/cli.so power rtc-wake set %lld", (long long)time(NULL) + 600);
    CHECK(!run(rtc_command));
    CHECK(run("/work/models/cli.so --apply power rtc-wake clear") != 0);
    put("/tmp/curve.conf", "profile=custom\nminimum_pwm=40\nallow_unvalidated_writes=true\nrequire_storage_sensor=false\n");
    CHECK(!run("/work/models/fand.so --config /tmp/curve.conf --state /tmp/state --ugreenctl /work/models/cli.so --plugin-dir /tmp/plugins --once"));
    CHECK(number("/tmp/hw/hwmon0/pwm3") == 255);
    puts("Real CLI/plugin/watchdog: dry run, confirmation, PWM, readback, revision and sensor failsafe passed");
    return 0;
}
