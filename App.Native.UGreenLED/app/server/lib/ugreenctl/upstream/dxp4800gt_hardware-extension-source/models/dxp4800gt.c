#include "ugreenctl.h"
#include "ec/it55_dxp4800gt.h"
#include <stdio.h>
#include <string.h>
static const char *const names[]={"DXP4800 GT",NULL};
static int fans(const struct ugreenctl_request *r,struct ugreenctl_fan_status *f,size_t *n,char *e,size_t z)
{ (void)r; return it55_gt_read_fans(f,n,e,z); }
static int startup(const struct ugreenctl_request *r,enum ugreenctl_startup_policy *p,char *e,size_t z)
{ (void)r; return it55_gt_read_startup(p,e,z); }
static int set_fan(const struct ugreenctl_request *r,const char *f,uint8_t p,char *e,size_t z)
{ return it55_gt_set_fan(r->force,f,p,e,z); }
static int set_startup(const struct ugreenctl_request *r,enum ugreenctl_startup_policy p,char *e,size_t z)
{ return it55_gt_set_startup(r->force,p,e,z); }
static int status(const struct ugreenctl_request *r,struct ugreenctl_status *s,char *e,size_t z)
{
    memset(s,0,sizeof(*s));
    snprintf(s->controller,sizeof(s->controller),"ITE IT55xx EC (stock protocol)");
    return fans(r,s->fans,&s->fan_count,e,z);
}
const struct ugreenctl_plugin *ugreenctl_plugin_v5(void)
{
    static const struct ugreenctl_plugin p={
        .abi_version=UGREENCTL_PLUGIN_ABI_V5,.id="dxp4800gt",
        .display_name="UGREEN DXP4800 GT (firmware-reversed)",.dmi_product_names=names,
        .capabilities=UGREENCTL_CAP_FAN|UGREENCTL_CAP_POWER,
        .read_status=status,.read_fans=fans,.set_fan_pwm=set_fan,
        .read_startup_policy=startup,.set_startup_policy=set_startup,
        .controller_name="ITE IT55xx EC (stock protocol)"
    };
    return &p;
}
