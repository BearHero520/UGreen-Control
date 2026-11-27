#include "ec/idx6012.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x); exit(1); } } while (0)
static uint8_t mem[256],status,reg,command;
static int phase,writes,fail_write;
static int busy,wrong,lock_error,io_error,stall,changed,reads,acquired,released,sends,rollover;
static int id(struct ugreenctl_machine_identity *v)
{
    memset(v,0,sizeof(*v)); strcpy(v->source,"dmi");
    strcpy(v->manufacturer,wrong==1?"Other":"UGREEN");
    strcpy(v->product_name,wrong==2?"iDX6011 Pro":"Default string");
    strcpy(v->product_version,wrong==3?"unknown":"iDX6011_iDX6012 V3.0 241112");
    strcpy(v->bios_date,"11/12/2024");
    strcpy(v->bios_version,wrong==4?"other":changed && reads++?"changed":"5.32"); return 0;
}
static int owners(void) { return busy?-EBUSY:0; }
static int acquire(void) { ++acquired; return lock_error; }
static void release(void) { ++released; }
static void pause_io(void) { }
static int input(unsigned short p,uint8_t *v)
{
    if (io_error) return -EIO;
    if (p==0x66) { *v=stall?2:status; return 0; }
    CHECK(p==0x62 && status==1); *v=mem[reg]; status=0;
    if (rollover && reg==0x96) ++mem[reg];
    return 0;
}
static int output(unsigned short p,uint8_t v)
{
    CHECK(acquired==1 && released==0); ++sends;
    if (p==0x66) { CHECK(v==0x80 || v==0x81);command=v;phase=0; }
    else {
        CHECK(p==0x62);
        if (!phase++) { reg=v; if(command==0x80) status=1; }
        else { CHECK(command==0x81 && reg>=0x9c && reg<=0x9f); ++writes;
            if (writes!=fail_write) mem[reg]=v;
            if (reg==0x9c || reg==0x9e) CHECK(v==0 || mem[reg+1]>=40);
        }
    }
    return 0;
}
static void reset(void)
{
    memset(mem,0,sizeof(mem)); mem[0x96]=5;mem[0x97]=100;mem[0x98]=5;mem[0x99]=90;mem[0xa6]=1;
    command=phase=writes=fail_write=0;status=reg=0;busy=wrong=lock_error=io_error=stall=changed=reads=acquired=released=sends=rollover=0;
}
int main(void)
{
    const struct idx6012_io ops={id,owners,acquire,release,input,output,pause_io};
    struct ugreenctl_fan_status f[2];size_t n;char e[256];unsigned gate=99;
    idx6012_test_io(&ops);
    struct ugreenctl_machine_identity ident;
    reset();id(&ident);CHECK(!strcmp(ugreenctl_identity_model(&ident),"iDX6011/iDX6012 OEM V3.0"));
    for (int i=1;i<=3;++i) { wrong=i;id(&ident);CHECK(!strcmp(ugreenctl_identity_model(&ident),ident.product_name)); }
    reset();CHECK(!idx6012_read_fans(f,&n,e,sizeof(e)));CHECK(n==2 && f[0].rpm==1380 && f[1].rpm==1370);
    CHECK(f[0].mode_known && !f[0].manual && !f[0].pwm_known && released==1);
    reset();mem[0x9c]=1;mem[0x9d]=150;CHECK(!idx6012_read_fans(f,&n,e,sizeof(e)));CHECK(f[0].manual && f[0].pwm_known && f[0].pwm==150);
    reset();mem[0x9c]=2;CHECK(!idx6012_read_fans(f,&n,e,sizeof(e)));CHECK(!f[0].mode_known && !f[0].pwm_known);
    reset();mem[0x9c]=1;mem[0x9d]=255;CHECK(!idx6012_read_fans(f,&n,e,sizeof(e)));CHECK(!f[0].pwm_known);
    for (int i=1;i<=3;++i) { reset();wrong=i;CHECK(idx6012_read_fans(f,&n,e,sizeof(e))==-ENODEV && !n && !acquired && !sends); }
    reset();busy=1;CHECK(idx6012_read_fans(f,&n,e,sizeof(e))==-EBUSY && !acquired && !sends);
    reset();lock_error=-EWOULDBLOCK;CHECK(idx6012_read_fans(f,&n,e,sizeof(e))<0 && released==1 && !sends);
    reset();status=1;CHECK(idx6012_read_fans(f,&n,e,sizeof(e))==-EBUSY && !sends && released==1);
    reset();stall=1;CHECK(idx6012_read_fans(f,&n,e,sizeof(e))==-ETIMEDOUT && !sends && released==1);
    reset();io_error=1;CHECK(idx6012_read_fans(f,&n,e,sizeof(e))==-EIO && released==1);
    reset();changed=1;CHECK(idx6012_read_fans(f,&n,e,sizeof(e))==-ESTALE && !n && released==1);
    reset();rollover=1;CHECK(idx6012_read_fans(f,&n,e,sizeof(e))==-EAGAIN && !n && released==1);
    reset();CHECK(!idx6012_read_g3(&gate,e,sizeof(e)) && gate==1);
    reset();mem[0xa6]=2;gate=99;CHECK(idx6012_read_g3(&gate,e,sizeof(e))==-ERANGE && gate==99);
    reset();CHECK(!idx6012_set_fan(true,"sys",150,e,sizeof(e)));CHECK(writes==4 && mem[0x9c]==1 && mem[0x9d]==150 && mem[0x9e]==1 && mem[0x9f]==150);
    acquired=released=0;CHECK(!idx6012_set_mode(true,"sys",true,e,sizeof(e)));CHECK(mem[0x9c]==0 && mem[0x9e]==0);
    reset();CHECK(idx6012_set_fan(false,"sys",100,e,sizeof(e))==-EPERM && !sends);
    reset();CHECK(idx6012_set_fan(true,"cpu",100,e,sizeof(e))==-EINVAL && !sends);
    reset();CHECK(idx6012_set_fan(true,"sys",39,e,sizeof(e))==-ERANGE && !sends);
    reset();CHECK(idx6012_set_fan(true,"sys",199,e,sizeof(e))==-ERANGE && !sends);
    reset();wrong=4;CHECK(idx6012_set_fan(true,"sys",100,e,sizeof(e))==-ENODEV && !sends);
    reset();busy=1;CHECK(idx6012_set_fan(true,"sys",100,e,sizeof(e))==-EBUSY && !writes);
    for(int i=1;i<=4;++i) { reset();fail_write=i;CHECK(idx6012_set_fan(true,"sys",150,e,sizeof(e))==-EIO);CHECK(mem[0x9c]==0 && mem[0x9d]==0 && mem[0x9e]==0 && mem[0x9f]==0 && released==1); }
    reset();mem[0x9c]=mem[0x9e]=1;mem[0x9d]=mem[0x9f]=100;fail_write=3;CHECK(idx6012_set_fan(true,"sys",150,e,sizeof(e))==-EIO);CHECK(mem[0x9d]==100 && mem[0x9f]==100 && mem[0x9c]==1 && mem[0x9e]==1);
    puts("iDX6012 read/write EC safety tests passed");return 0;
}
