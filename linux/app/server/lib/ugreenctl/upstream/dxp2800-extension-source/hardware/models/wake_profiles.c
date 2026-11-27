#include "models/wake_profiles.h"
#include <string.h>

/* Firmware routing and input hashes: docs/ALL_MODELS_WAKE_EVIDENCE.zh-CN.md.
 * Public model names are exact allowlists, never firmware prefix matches.
 * A generic OnSched script establishes the Linux alarm operation, not S5 wake.
 */
#define RTC(id, product) {id,product,NULL,NULL,"rtc_cmos",NULL,1}
static const struct ugreenctl_rtc_profile rtc_profiles[] = {
    RTC("dh2600","DH2600"),
    RTC("dx4600","DX4600"), RTC("dx4600plus","DX4600+"), RTC("dx4600pro","DX4600 Pro"),
    RTC("dx4700","DX4700"), RTC("dx4700plus","DX4700+"),
    RTC("dxp2800","DXP2800"), RTC("dxp2800gt","DXP2800 GT"),
    RTC("dxp4800","DXP4800"), RTC("dxp4800s","DXP4800S"),
    RTC("dxp4800plus","DXP4800 Plus"), RTC("dxp4800pro","DXP4800 Pro"),
    RTC("dxp4800gt","DXP4800 GT"), RTC("dxp480tplus","DXP480T Plus"),
    RTC("dxp6800","DXP6800"), RTC("dxp6800plus","DXP6800 Plus"),
    RTC("dxp6800pro","DXP6800 Pro"), RTC("dxp6800ultra","DXP6800 Ultra"),
    RTC("dxp8800","DXP8800"), RTC("dxp8800plus","DXP8800 Plus"),
    RTC("dxp8800pro","DXP8800 Pro"), RTC("dxp8800ultra","DXP8800 Ultra"),
    RTC("idx6011pro","iDX6011 Pro"),
    {"dh2300","DH2300","UGREEN RK3576 DH2300 Board","rockchip,rk3576",
     "hym8563","/i2c@2ac50000/hym8563@51",60},
    {"dh4300plus","DH4300 Plus","UGREEN RK3588C DH4300Plus Board","rockchip,rk3588",
     "hym8563","/i2c@fec80000/hym8563@51",60}
};

/* Zero means renamed-interface count has NOT been established: explicit
 * --interface is required. It does not mean the machine lacks WOL. */
#define WOL(id, product, n) {id,product,NULL,NULL,NULL,NULL,n}
static const struct ugreenctl_wol_profile wol_profiles[] = {
    WOL("dh2600","DH2600",0),
    WOL("dx4600","DX4600",2), WOL("dx4600plus","DX4600+",2), WOL("dx4600pro","DX4600 Pro",2),
    WOL("dx4700","DX4700",0), WOL("dx4700plus","DX4700+",0),
    WOL("dxp2800","DXP2800",0), WOL("dxp2800gt","DXP2800 GT",0),
    WOL("dxp4800","DXP4800",2), WOL("dxp4800s","DXP4800S",2),
    WOL("dxp4800plus","DXP4800 Plus",2), WOL("dxp4800pro","DXP4800 Pro",2),
    WOL("dxp4800gt","DXP4800 GT",0), WOL("dxp480tplus","DXP480T Plus",1),
    WOL("dxp6800","DXP6800",0), WOL("dxp6800plus","DXP6800 Plus",2),
    WOL("dxp6800pro","DXP6800 Pro",2), WOL("dxp6800ultra","DXP6800 Ultra",0),
    WOL("dxp8800","DXP8800",0), WOL("dxp8800plus","DXP8800 Plus",0),
    WOL("dxp8800pro","DXP8800 Pro",0), WOL("dxp8800ultra","DXP8800 Ultra",0),
    WOL("idx6011pro","iDX6011 Pro",0),
    {"dh2300","DH2300","UGREEN RK3576 DH2300 Board","rockchip,rk3576",
     "/ethernet@2a220000","rockchip,rk3576-gmac",0},
    {"dh4300plus","DH4300 Plus","UGREEN RK3588C DH4300Plus Board","rockchip,rk3588",
     NULL,NULL,0}
};

static int matches(const struct ugreenctl_machine_identity *i, const char *product,
                   const char *dt_model, const char *dt_compatible)
{
    if (!i) return 0;
    if (dt_model)
        return !strcmp(i->source,"devicetree") && !strcmp(i->product_name,dt_model) &&
            ugreenctl_identity_has_compatible(i,dt_compatible);
    return !strcmp(i->source,"dmi") && !strcmp(i->product_name,product);
}
const struct ugreenctl_rtc_profile *ugreenctl_rtc_profiles(size_t *count)
{
    *count=sizeof(rtc_profiles)/sizeof(rtc_profiles[0]); return rtc_profiles;
}
const struct ugreenctl_wol_profile *ugreenctl_wol_profiles(size_t *count)
{
    *count=sizeof(wol_profiles)/sizeof(wol_profiles[0]); return wol_profiles;
}
const struct ugreenctl_rtc_profile *ugreenctl_rtc_match(const struct ugreenctl_machine_identity *i)
{
    size_t n;
    for (n=0;n<sizeof(rtc_profiles)/sizeof(rtc_profiles[0]);++n)
        if (matches(i,rtc_profiles[n].product,rtc_profiles[n].dt_model,rtc_profiles[n].dt_compatible)) return &rtc_profiles[n];
    return NULL;
}
const struct ugreenctl_wol_profile *ugreenctl_wol_match(const struct ugreenctl_machine_identity *i)
{
    size_t n;
    for (n=0;n<sizeof(wol_profiles)/sizeof(wol_profiles[0]);++n)
        if (matches(i,wol_profiles[n].product,wol_profiles[n].dt_model,wol_profiles[n].dt_compatible)) return &wol_profiles[n];
    return NULL;
}
