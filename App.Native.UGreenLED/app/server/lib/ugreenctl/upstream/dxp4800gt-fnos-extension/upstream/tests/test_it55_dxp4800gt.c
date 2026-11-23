#define _GNU_SOURCE
#include "ec/it55_dxp4800gt.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint8_t mem[256],command,reg,status_byte;
static unsigned int phase,writes,commands,locked,released,identity_calls;
static int busy,lock_error,identity_error,stall,ignore,change;
static int id(struct ugreenctl_machine_identity *v) {
    memset(v,0,sizeof(*v)); strcpy(v->manufacturer,"UGREEN"); strcpy(v->product_name,identity_error?"DXP4800":"DXP4800 GT");
    strcpy(v->bios_version,change && identity_calls++?"changed":"DXP4800 GT V1.0 20260429"); return 0;
}
static int owners(char *detail,size_t size) {
    if (!busy) return 0;
    snprintf(detail,size,"fixture EC owner");
    return -EBUSY;
}
static int acquire(void) { ++locked; return lock_error; }
static void release(void) { ++released; }
static void pause_io(void) { }
static uint8_t input(unsigned short p) {
    if (p==0x66) return stall?2:status_byte;
    CHECK(p==0x62 && (status_byte&1)); status_byte&=~1; return mem[reg];
}
static void output(uint8_t v,unsigned short p) {
    CHECK(locked && !released);
    if (p==0x66) {
        command=v; phase=0; status_byte=8;
        CHECK(v==0x80||v==0x81||v==0x88||v==0x89);
        if (v==0x88||v==0x89) ++commands;
    } else {
        CHECK(p==0x62);
        if (!phase++) { reg=v; if (command==0x80) status_byte=1; }
        else { CHECK(command==0x81); ++writes; if (!ignore) mem[reg]=v; status_byte=0; }
    }
}
static void reset(void) {
    memset(mem,0,sizeof(mem)); mem[0]=1; mem[1]=2; mem[3]=3;
    mem[0x34]=0x04; mem[0x35]=0xd2; mem[0x38]=0x09; mem[0x39]=0x29;
    mem[0xb0]=mem[0xb4]=1; mem[0xb1]=100; mem[0xb5]=120;
    phase=writes=commands=locked=released=identity_calls=0;
    status_byte=0; busy=lock_error=identity_error=stall=ignore=change=0;
}
static void test_ioports(void) {
    char detail[160]={0};
    FILE *fixture=tmpfile(); CHECK(fixture);
    fputs("0000-00ff : PCI Bus 0000:00\n  0060-006f : pnp 00:02\n",fixture);
    rewind(fixture);
    CHECK(it55_gt_test_ioports(fixture,detail,sizeof(detail))==-EBUSY);
    CHECK(strstr(detail,"0060-006f : pnp 00:02"));
    fclose(fixture);
    fixture=tmpfile(); CHECK(fixture);
    /* fnOS report: all three resources are adjacent to, but do not claim, the EC ports. */
    fputs("0000-00ff : PCI Bus 0000:00\n"
          "  0060-0060 : keyboard\n"
          "  0061-0061 : PNP0800:00\n"
          "  0064-0064 : keyboard\n",fixture);
    rewind(fixture);
    CHECK(!it55_gt_test_ioports(fixture,detail,sizeof(detail)));
    fclose(fixture);
}
static void test_acpi_devices(void) {
    char root[]="/tmp/gt-acpi-XXXXXX", node[128], detail[160]={0};
    CHECK(mkdtemp(root));
    snprintf(node,sizeof(node),"%s/AMDI0010:00",root);
    CHECK(!mkdir(node,0700));
    CHECK(!it55_gt_test_acpi_devices(root,detail,sizeof(detail)));
    CHECK(!rmdir(node));
    snprintf(node,sizeof(node),"%s/PNP0C09:00",root);
    CHECK(!mkdir(node,0700));
    CHECK(it55_gt_test_acpi_devices(root,detail,sizeof(detail))==-EBUSY);
    CHECK(strstr(detail,"PNP0C09:00"));
    CHECK(!rmdir(node) && !rmdir(root));
    CHECK(it55_gt_test_acpi_devices(root,detail,sizeof(detail))==-ENOENT);
}
int main(void) {
    struct it55_gt_io ops={id,owners,acquire,release,input,output,pause_io};
    struct ugreenctl_fan_status f[2]; size_t n; char e[256]; enum ugreenctl_startup_policy p;
    it55_gt_test_io(&ops);
    test_ioports();
    test_acpi_devices();
    reset(); CHECK(!it55_gt_read_fans(f,&n,e,sizeof(e))); CHECK(n==2 && f[0].rpm==1234 && f[1].rpm==2345); CHECK(f[0].pwm==100 && f[1].pwm==120); CHECK(!writes && released==1);
    reset(); CHECK(!it55_gt_set_fan(true,"cpu",150,e,sizeof(e))); CHECK(mem[0xb0]==1 && mem[0xb1]==150 && mem[0xb5]==120 && writes==2 && released==1);
    reset(); CHECK(!it55_gt_set_fan(true,"sys",198,e,sizeof(e))); CHECK(mem[0xb5]==198 && mem[0xb1]==100);
    reset(); CHECK(it55_gt_set_fan(false,"cpu",100,e,sizeof(e))==-EPERM && !locked && !writes);
    reset(); CHECK(it55_gt_set_fan(true,"cpu",199,e,sizeof(e))==-ERANGE && !locked);
    CHECK(it55_gt_set_fan(true,"cpu",39,e,sizeof(e))==-ERANGE);
    CHECK(it55_gt_set_fan(true,"all",100,e,sizeof(e))==-EINVAL);
    reset(); busy=1; CHECK(it55_gt_set_fan(true,"cpu",100,e,sizeof(e))==-EBUSY && !locked);
    CHECK(strstr(e,"controller owner check") && strstr(e,"fixture EC owner"));
    reset(); status_byte=0xff; CHECK(it55_gt_read_fans(f,&n,e,sizeof(e))==-ENODEV && !writes && released==1);
    reset(); status_byte=1; CHECK(it55_gt_read_fans(f,&n,e,sizeof(e))==-EBUSY && !writes); CHECK(strstr(e,"EC version read"));
    reset(); identity_error=1; CHECK(it55_gt_set_fan(true,"cpu",100,e,sizeof(e))==-ENODEV && !locked);
    reset(); lock_error=-EACCES; CHECK(it55_gt_set_fan(true,"cpu",100,e,sizeof(e))==-EACCES && released==1 && !writes);
    reset(); stall=1; CHECK(it55_gt_set_fan(true,"cpu",100,e,sizeof(e))==-ETIMEDOUT && released==1 && !writes);
    reset(); ignore=1; CHECK(it55_gt_set_fan(true,"cpu",150,e,sizeof(e))==-EIO && released==1);
    reset(); change=1; CHECK(it55_gt_set_fan(true,"cpu",100,e,sizeof(e))==-ESTALE && !writes);
    reset(); mem[0]=mem[1]=mem[3]=0; CHECK(it55_gt_set_fan(true,"cpu",100,e,sizeof(e))==-ENODEV && !writes);
    reset(); CHECK(!it55_gt_read_startup(&p,e,sizeof(e)) && p==UGREENCTL_STARTUP_UNKNOWN && !writes && !commands);
    reset(); CHECK(!it55_gt_set_startup(true,UGREENCTL_STARTUP_ON,e,sizeof(e)) && command==0x88 && commands==1 && !writes);
    reset(); CHECK(!it55_gt_set_startup(true,UGREENCTL_STARTUP_OFF,e,sizeof(e)) && command==0x89 && commands==1);
    reset(); CHECK(it55_gt_set_startup(true,UGREENCTL_STARTUP_RESTORE,e,sizeof(e))==-ENOTSUP && !locked);
    reset(); CHECK(it55_gt_set_startup(false,UGREENCTL_STARTUP_ON,e,sizeof(e))==-EPERM && !locked);
    puts("DXP4800 GT EC transactions, exact routing, guards, PWM readback and truthful startup status passed");
    return 0;
}
