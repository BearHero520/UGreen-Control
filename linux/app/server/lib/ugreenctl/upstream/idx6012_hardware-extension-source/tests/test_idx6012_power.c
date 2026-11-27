#include "power/idx6012_power.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
static uint32_t value;static int opens,writes,denied,failwrite;
static int begin(void){opens++;return denied;}
static void end(void){}
static int readv(uint32_t *v){*v=value;return 0;}
static int writev(uint32_t v){writes++;value=v;return writes==failwrite?-EIO:0;}
int main(void){
 struct idx6012_power_io io={begin,end,readv,writev};idx6012_power_test_io(&io);
 struct ugreenctl_machine_identity id={0};
 strcpy(id.source,"dmi");strcpy(id.manufacturer,"UGREEN");strcpy(id.product_name,"Default string");
 strcpy(id.product_version,"iDX6011_iDX6012 V3.0 241112");strcpy(id.bios_version,"5.32");strcpy(id.bios_date,"11/12/2024");
 assert(!idx6012_power_identity(&id));strcpy(id.bios_version,"unknown");assert(idx6012_power_identity(&id));
 enum ugreenctl_startup_policy p;char error[256];value=0xd8fa3878;
 assert(!idx6012_read_startup(&p,error,sizeof(error)) && p==UGREENCTL_STARTUP_ON);
 int before=opens;
 assert(idx6012_set_startup(false,UGREENCTL_STARTUP_OFF,error,sizeof(error)) && opens==before);
 assert(idx6012_set_startup(true,UGREENCTL_STARTUP_RESTORE,error,sizeof(error)) && opens==before);
 denied=-EBUSY;assert(idx6012_set_startup(true,UGREENCTL_STARTUP_OFF,error,sizeof(error)) && writes==0);denied=0;
 assert(!idx6012_set_startup(true,UGREENCTL_STARTUP_OFF,error,sizeof(error)) && value==0xd8fa3879);
 assert(!idx6012_set_startup(true,UGREENCTL_STARTUP_ON,error,sizeof(error)) && value==0xd8fa3878);
 writes=0;failwrite=1;assert(idx6012_set_startup(true,UGREENCTL_STARTUP_OFF,error,sizeof(error)) && value==0xd8fa3878 && writes==2);
 return 0;
}
