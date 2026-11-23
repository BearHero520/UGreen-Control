#define _POSIX_C_SOURCE 200809L
/* Disposable QEMU only: all fan writes target fixture files in /tmp. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void put(const char *p, const char *s) {
    FILE *f = fopen(p, "w"); CHECK(f); CHECK(fputs(s, f) >= 0); CHECK(!fclose(f));
}
static int number(const char *p) {
    int n; FILE *f = fopen(p, "r"); CHECK(f && fscanf(f, "%d", &n) == 1); CHECK(!fclose(f)); return n;
}
static int run(const char *s) {
    int r = system(s); CHECK(r >= 0 && WIFEXITED(r)); return WEXITSTATUS(r);
}
#define CLI "/work/models/cli.so --plugin-dir /tmp/plugins "
int main(int argc, char **argv) {
    char path[128];
    CHECK(!mkdir("/tmp/plugins", 0700)); CHECK(!mkdir("/tmp/hw", 0700));
    CHECK(!mkdir("/tmp/hw/hwmon0", 0700)); CHECK(!mkdir("/tmp/thermal", 0700));
    CHECK(!mkdir("/tmp/thermal/hwmon0", 0700)); CHECK(!mkdir("/tmp/empty", 0700));
    CHECK(!symlink("/work/models/dxp8800pro.so", "/tmp/plugins/dxp8800pro.so"));
    put("/tmp/hw/hwmon0/name", "it8613\n");
    for (int n = 2; n <= 4; ++n) {
        snprintf(path, sizeof(path), "/tmp/hw/hwmon0/pwm%d", n); put(path, "100\n");
        snprintf(path, sizeof(path), "/tmp/hw/hwmon0/pwm%d_enable", n); put(path, "2\n");
        snprintf(path, sizeof(path), "/tmp/hw/hwmon0/fan%d_input", n); put(path, "900\n");
    }
    CHECK(!setenv("UGREENCTL_HWMON_ROOT", "/tmp/hw", 1));
    CHECK(!setenv("UGREENCTL_IT8613_LOCK", "/tmp/fan.lock", 1));
    CHECK(!setenv("UGREENCTL_THERMAL_HWMON_ROOT", "/tmp/thermal", 1));
    CHECK(!setenv("UGREENCTL_THERMAL_BLOCK_ROOT", "/tmp/empty", 1));
    if (argc > 1 && !strcmp(argv[1], "reject")) {
        CHECK(run(CLI "fan status") != 0);
        CHECK(run(CLI "--model dxp8800pro --force --apply fan set sys 128") != 0);
        CHECK(run(CLI "--model dxp8800pro --force --apply power startup set on") != 0);
        CHECK(number("/tmp/hw/hwmon0/pwm3") == 100);
        puts("Different DMI rejected, including explicit --model override"); return 0;
    }
    CHECK(!run(CLI "fan status"));
    CHECK(!run(CLI "--force fan set sys 128"));
    CHECK(number("/tmp/hw/hwmon0/pwm3") == 100);
    CHECK(run(CLI "--apply fan set sys 128") != 0);
    CHECK(!run(CLI "--force --apply fan set cpu 70"));
    CHECK(number("/tmp/hw/hwmon0/pwm2") == 70);
    CHECK(number("/tmp/hw/hwmon0/pwm3") == 100);
    CHECK(!run(CLI "--force --apply fan set sys 128"));
    CHECK(number("/tmp/hw/hwmon0/pwm3") == 128 && number("/tmp/hw/hwmon0/pwm4") == 128);
    CHECK(number("/tmp/hw/hwmon0/pwm3_enable") == 1 && number("/tmp/hw/hwmon0/pwm4_enable") == 1);
    CHECK(run(CLI "--force --apply fan set sys1 120") != 0);
    CHECK(run(CLI "--force --apply fan set cpu 39") != 0);
    CHECK(!run(CLI "power startup set restore"));
    CHECK(run(CLI "--apply power startup set on") != 0);
    put("/tmp/thermal/hwmon0/name", "coretemp\n");
    put("/tmp/thermal/hwmon0/temp1_input", "38000\n");
    put("/tmp/curve.conf", "profile=stock-8800pro\nminimum_pwm=40\nallow_unvalidated_writes=true\nrequire_storage_sensor=false\n");
    CHECK(!run("/work/models/fand.so --config /tmp/curve.conf --state /tmp/state --ugreenctl /work/models/cli.so --plugin-dir /tmp/plugins --once"));
    CHECK(number("/tmp/hw/hwmon0/pwm2") == 70);
    CHECK(number("/tmp/hw/hwmon0/pwm3") == 64 && number("/tmp/hw/hwmon0/pwm4") == 64);
    put("/tmp/curve.conf", "profile=stock-6800pro\nallow_unvalidated_writes=true\nrequire_storage_sensor=false\n");
    CHECK(run("/work/models/fand.so --config /tmp/curve.conf --state /tmp/state --ugreenctl /work/models/cli.so --plugin-dir /tmp/plugins --once") != 0);
    put("/tmp/curve.conf", "profile=stock-8800pro\nallow_unvalidated_writes=true\nrequire_storage_sensor=false\n");
    CHECK(!unlink("/tmp/thermal/hwmon0/temp1_input"));
    CHECK(!run("/work/models/fand.so --config /tmp/curve.conf --state /tmp/state --ugreenctl /work/models/cli.so --plugin-dir /tmp/plugins --once"));
    CHECK(number("/tmp/hw/hwmon0/pwm2") == 255 && number("/tmp/hw/hwmon0/pwm3") == 255 && number("/tmp/hw/hwmon0/pwm4") == 255);
    puts("DXP8800 Pro installed CLI/plugin/daemon runtime guards and independent curve passed"); return 0;
}
