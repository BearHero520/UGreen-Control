#define _POSIX_C_SOURCE 200809L
/* QEMU only, with no hardware passthrough. Successful EC transactions are
 * covered by upstream test_it55_dxp4800gt; this tests CLI/plugin routing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
#define CLI "/work/models/cli.so --plugin-dir /tmp/plugins "
static int run(const char *s) { int r=system(s); CHECK(r>=0 && WIFEXITED(r)); return WEXITSTATUS(r); }
static void put(const char *p,const char *s) { FILE *f=fopen(p,"w"); CHECK(f); CHECK(fputs(s,f)>=0); CHECK(!fclose(f)); }
static void contains(const char *p,const char *s) { char b[4096]; FILE *f=fopen(p,"r"); CHECK(f); size_t n=fread(b,1,sizeof(b)-1,f); b[n]=0; fclose(f); CHECK(strstr(b,s)); }
#define FAND "/work/models/fand.so --config /tmp/curve --state /tmp/state --ugreenctl /tmp/mockcli --plugin-dir /tmp/plugins --once"
static void daemon_tests(void) {
    CHECK(!mkdir("/tmp/thermal",0700)); CHECK(!mkdir("/tmp/thermal/hwmon0",0700));
    CHECK(!mkdir("/tmp/thermal/hwmon1",0700)); CHECK(!mkdir("/tmp/thermal/hwmon2",0700));
    CHECK(!mkdir("/tmp/empty",0700));
    put("/tmp/thermal/hwmon0/name","k10temp\n"); put("/tmp/thermal/hwmon0/temp1_input","72000\n");
    put("/tmp/thermal/hwmon1/name","drivetemp\n"); put("/tmp/thermal/hwmon1/temp1_input","42000\n");
    put("/tmp/thermal/hwmon2/name","nvme\n"); put("/tmp/thermal/hwmon2/temp1_input","52000\n");
    CHECK(!setenv("UGREENCTL_THERMAL_HWMON_ROOT","/tmp/thermal",1));
    CHECK(!setenv("UGREENCTL_THERMAL_BLOCK_ROOT","/tmp/empty",1));
    put("/tmp/mockcli","#!/bin/sh\necho \"$*\" >> /tmp/calls\n"); CHECK(!chmod("/tmp/mockcli",0700));
    put("/tmp/curve","profile=stock-4800gt\nallow_unvalidated_writes=true\nrequire_storage_sensor=true\n");
    put("/tmp/calls",""); CHECK(!run(FAND));
    contains("/tmp/calls","--force --apply fan set cpu 43"); contains("/tmp/calls","--force --apply fan set sys 100");
    contains("/tmp/state","profile=stock-4800gt"); contains("/tmp/state","status=running");
    put("/tmp/thermal/hwmon0/temp1_input","100000\n");
    put("/tmp/thermal/hwmon1/temp1_input","59000\n");
    put("/tmp/calls",""); CHECK(!run(FAND));
    contains("/tmp/calls","fan set cpu 187"); contains("/tmp/calls","fan set sys 198");
    CHECK(!unlink("/tmp/thermal/hwmon0/temp1_input"));
    put("/tmp/calls",""); CHECK(!run(FAND));
    contains("/tmp/calls","fan set cpu 198"); contains("/tmp/calls","fan set sys 198"); contains("/tmp/state","status=failsafe");
    put("/tmp/curve","profile=custom\nallow_unvalidated_writes=true\n"); CHECK(run(FAND)!=0);
    put("/tmp/curve","profile=stock-4800s\nallow_unvalidated_writes=true\n"); CHECK(run(FAND)!=0);
    put("/tmp/curve","profile=stock-4800gt\nallow_unvalidated_writes=false\n"); CHECK(run(FAND)!=0);
    put("/tmp/curve","profile=stock-4800gt\nallow_unvalidated_writes=true\n");
    put("/tmp/mockcli","#!/bin/sh\nexit 1\n"); CHECK(run(FAND)!=0); contains("/tmp/state","status=error");
    puts("GT daemon: official plan, guarded channel writes, 198 failsafe, wrong profiles and write failures passed");
}
int main(int argc,char **argv) {
    CHECK(!mkdir("/tmp/plugins",0700));
    CHECK(!symlink("/work/models/dxp4800gt.so","/tmp/plugins/dxp4800gt.so"));
    if (argc>1 && !strcmp(argv[1],"reject")) {
        CHECK(run(CLI "--model dxp4800gt --force --apply fan set cpu 100")!=0);
        CHECK(run(CLI "--model dxp4800gt --force --apply power startup set on")!=0);
        puts("GT-like DMI rejected despite explicit model override"); return 0;
    }
    CHECK(!run(CLI "models"));
    CHECK(run(CLI "info")!=0);
    CHECK(!run(CLI "--force fan set cpu 100"));
    CHECK(!run(CLI "--force power startup set on"));
    CHECK(run(CLI "--apply fan set cpu 100")!=0);
    CHECK(run(CLI "--apply power startup set on")!=0);
    CHECK(run(CLI "--force --apply fan set cpu 199")!=0);
    CHECK(run(CLI "--force --apply fan set sys 39")!=0);
    CHECK(run(CLI "--force --apply fan set all 100")!=0);
    CHECK(run(CLI "--force --apply power startup set restore")!=0);
    CHECK(run(CLI "--force --apply fan set cpu 100")!=0);
    CHECK(run(CLI "--force --apply power startup set on")!=0);
    daemon_tests();
    puts("GT CLI/plugin routing, dry run, ranges and absent-EC failures passed");
    return 0;
}
