#include "ugreenctl.h"
#include "ec/idx6012.h"
#include "power/idx6012_power.h"
#include <stdio.h>
#include <string.h>
/* The synthetic OEM identity requires exact manufacturer and board revision.
 * Never allow every machine named Default string. */
static const char *const names[]={"iDX6012","iDX6011/iDX6012 OEM V3.0",NULL};
static int fans(const struct ugreenctl_request *r,struct ugreenctl_fan_status *f,size_t *n,char *e,size_t z)
{ (void)r; return idx6012_read_fans(f,n,e,z); }
static int set_fan(const struct ugreenctl_request *r,const char *f,uint8_t p,char *e,size_t z)
{ return idx6012_set_fan(r->force,f,p,e,z); }
static int set_mode(const struct ugreenctl_request *r,const char *f,bool a,char *e,size_t z)
{ return idx6012_set_mode(r->force,f,a,e,z); }
static int startup(const struct ugreenctl_request *r,enum ugreenctl_startup_policy *p,char *e,size_t z)
{ (void)r; return idx6012_read_startup(p,e,z); }
static int set_startup(const struct ugreenctl_request *r,enum ugreenctl_startup_policy p,char *e,size_t z)
{ return idx6012_set_startup(r->force,p,e,z); }
static int status(const struct ugreenctl_request *r,struct ugreenctl_status *s,char *e,size_t z)
{
    memset(s,0,sizeof(*s));
    snprintf(s->controller,sizeof(s->controller),"ITE5571 (board-qualified)");
    return fans(r,s->fans,&s->fan_count,e,z);
}
const struct ugreenctl_plugin *ugreenctl_plugin_v5(void)
{
    static const struct ugreenctl_plugin p={
        .abi_version=UGREENCTL_PLUGIN_ABI_V5,.id="idx6012",
        .display_name="UGREEN iDX6012 (experimental)",.dmi_product_names=names,
        .capabilities=UGREENCTL_CAP_FAN|UGREENCTL_CAP_POWER,.read_status=status,.read_fans=fans,
        .read_startup_policy=startup,.set_startup_policy=set_startup,
        .set_fan_pwm=set_fan,.set_fan_mode=set_mode,
        .controller_name="ITE5571 (board-qualified)"
    };
    return &p;
}
