#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "models/wake_profiles.h"
#include "network/wol_interface.h"
#include "power/rtc_wake.h"

static char root[256];
static void put(const char *suffix, const char *content)
{
    char path[1024]; char *p; FILE *f;
    snprintf(path,sizeof(path),"%s/%s",root,suffix);
    for (p=path+1;*p;++p) if (*p=='/') { *p=0; (void)mkdir(path,0700); *p='/'; }
    f=fopen(path,"w"); assert(f); assert(fputs(content,f)>=0); assert(!fclose(f));
}
int main(void)
{
    size_t nr,nw,i;
    const struct ugreenctl_rtc_profile *r=ugreenctl_rtc_profiles(&nr),*arm=NULL;
    const struct ugreenctl_wol_profile *w=ugreenctl_wol_profiles(&nw),*mac=NULL;
    struct ugreenctl_machine_identity id={0};
    char error[256],path[1024],identity[256],tmp[]="/tmp/wake-profiles-XXXXXX";
    assert(nr==25 && nw==27);
    for (i=0;i<nr;++i) {
        memset(&id,0,sizeof(id));
        strcpy(id.source,r[i].dt_model ? "devicetree" : "dmi");
        strcpy(id.product_name,r[i].dt_model ? r[i].dt_model : r[i].product);
        if (r[i].dt_compatible) strcpy(id.compatible,r[i].dt_compatible);
        assert(ugreenctl_rtc_match(&id)==&r[i]);
        assert(ugreenctl_wol_match(&id));
        strcat(id.product_name," unknown revision");
        assert(!ugreenctl_rtc_match(&id) && !ugreenctl_wol_match(&id));
        if (!strcmp(r[i].id,"dh2300")) arm=&r[i];
    }
    for (i=0;i<nw;++i) if (!strcmp(w[i].id,"dh2300")) mac=&w[i];
    assert(arm && mac);
    memset(&id,0,sizeof(id));strcpy(id.source,"dmi");strcpy(id.product_name,"Default string");
    assert(!ugreenctl_wol_match(&id));
    strcpy(id.manufacturer,"UGREEN");strcpy(id.product_version,"iDX6011_iDX6012 V3.0 241112");
    assert(ugreenctl_wol_match(&id) && ugreenctl_wol_match(&id)->automatic_pci_count==2);
    assert(!ugreenctl_rtc_match(&id));
    strcpy(id.manufacturer,"Other");assert(!ugreenctl_wol_match(&id));

    strcpy(id.source,"dmi"); strcpy(id.product_name,"DH2300");
    assert(!ugreenctl_rtc_match(&id)); /* DT-only hardware cannot be asserted via DMI. */
    strcpy(id.product_name,"DH2100"); assert(!ugreenctl_rtc_match(&id));
    strcpy(id.source,"devicetree"); strcpy(id.product_name,arm->dt_model);
    strcpy(id.compatible,"rockchip,rk3588"); assert(!ugreenctl_rtc_match(&id));
    assert(!ugreenctl_rtc_validate_epoch(arm,1800000120,1800000000,error,sizeof(error)));
    assert(ugreenctl_rtc_validate_epoch(arm,1800000121,1800000000,error,sizeof(error)));
    assert(ugreenctl_rtc_validate_epoch(&r[0],1800000010,1800000000,error,sizeof(error)));

    assert(mkdtemp(tmp)); strcpy(root,tmp);
    put("class/rtc/rtc0/name","unrelated\n"); put("class/rtc/rtc0/wakealarm","");
    put("class/rtc/rtc7/name","rtc_cmos 00:03\n"); put("class/rtc/rtc7/wakealarm","");
    assert(!ugreenctl_rtc_select_at(root,&r[0],path,sizeof(path),error,sizeof(error)));
    assert(strstr(path,"rtc7/wakealarm")); /* not first device */
    put("class/rtc/rtc8/name","rtc_cmos 00:04\n"); put("class/rtc/rtc8/wakealarm","");
    assert(ugreenctl_rtc_select_at(root,&r[0],path,sizeof(path),error,sizeof(error)));
    put("class/rtc/rtc9/name","hym8563 2-0051\n"); put("class/rtc/rtc9/wakealarm","");
    put("class/rtc/rtc9/device/uevent","OF_FULLNAME=/wrong/hym8563@51\n");
    assert(ugreenctl_rtc_select_at(root,arm,path,sizeof(path),error,sizeof(error)));
    put("class/rtc/rtc9/device/uevent","OF_FULLNAME=/i2c@2ac50000/hym8563@51\n");
    assert(!ugreenctl_rtc_select_at(root,arm,path,sizeof(path),error,sizeof(error)));
    assert(strstr(path,"rtc9/wakealarm"));

    put("class/net/enp8s0/type","1\n");
    put("class/net/enp8s0/device/uevent","PCI_CLASS=020000\nPCI_SLOT_NAME=0000:08:00.0\n");
    assert(!ugreenctl_wol_interface_identity_at(root,&w[0],"enp8s0",identity,sizeof(identity),error,sizeof(error)));
    assert(!strcmp(identity,"pci:0000:08:00.0"));
    put("class/net/lo/type","772\n"); put("class/net/br0/type","1\n");
    assert(ugreenctl_wol_interface_identity_at(root,&w[0],"lo",identity,sizeof(identity),error,sizeof(error)));
    assert(ugreenctl_wol_interface_identity_at(root,&w[0],"br0",identity,sizeof(identity),error,sizeof(error)));
    assert(ugreenctl_wol_interface_identity_at(root,&w[0],"../enp8s0",identity,sizeof(identity),error,sizeof(error)));
    put("class/net/end0/type","1\n");
    put("class/net/end0/device/uevent","OF_FULLNAME=/ethernet@2a220000\nOF_COMPATIBLE_0=rockchip,rk3576-gmac\n");
    assert(!ugreenctl_wol_interface_identity_at(root,mac,"end0",identity,sizeof(identity),error,sizeof(error)));
    assert(ugreenctl_wol_interface_identity_at(root,&w[0],"end0",identity,sizeof(identity),error,sizeof(error)));
    put("class/net/end0/device/uevent","OF_FULLNAME=/ethernet@2a230000\nOF_COMPATIBLE_0=rockchip,rk3576-gmac\n");
    assert(ugreenctl_wol_interface_identity_at(root,mac,"end0",identity,sizeof(identity),error,sizeof(error)));
    /* Replay the reference fnOS interface shape for every independent model.
     * The reference host identity is never used to authorise a NAS profile. */
    for (i=0;i<nr;++i) {
        char event[512];
        snprintf(root,sizeof(root),"%s/model-%zu",tmp,i);
        put("class/rtc/rtc4/name",r[i].rtc_of_path ? "hym8563 2-0051\n" : "rtc_cmos 00:01\n");
        put("class/rtc/rtc4/wakealarm","");
        if (r[i].rtc_of_path) {
            snprintf(event,sizeof(event),"OF_FULLNAME=%s\n",r[i].rtc_of_path);
            put("class/rtc/rtc4/device/uevent",event);
        }
        assert(!ugreenctl_rtc_select_at(root,&r[i],path,sizeof(path),error,sizeof(error)));
        assert(strstr(path,"rtc4/wakealarm"));
        assert(!ugreenctl_rtc_validate_epoch(&r[i],1800000120,1800000000,error,sizeof(error)));
        assert(ugreenctl_rtc_validate_epoch(&r[i],1800000001,1800000000,error,sizeof(error)));
        put("class/rtc/rtc4/name","unrecognised-controller\n");
        assert(ugreenctl_rtc_select_at(root,&r[i],path,sizeof(path),error,sizeof(error)));
    }
    for (i=0;i<nw;++i) {
        snprintf(root,sizeof(root),"%s/net-%zu",tmp,i);
        put("class/net/enp1s0/type","1\n");
        /* Linux uevent omits the leading zero on this reference system. */
        put("class/net/enp1s0/device/uevent","DRIVER=r8169\nPCI_CLASS=20000\nPCI_SLOT_NAME=0000:02:00.0\n");
        assert(!ugreenctl_wol_interface_identity_at(root,&w[i],"enp1s0",identity,sizeof(identity),error,sizeof(error)));
        assert(!strcmp(identity,"pci:0000:02:00.0"));
        put("class/net/wlp6s0/type","1\n");
        put("class/net/wlp6s0/device/uevent","PCI_CLASS=28000\nPCI_SLOT_NAME=0000:08:00.0\n");
        assert(ugreenctl_wol_interface_identity_at(root,&w[i],"wlp6s0",identity,sizeof(identity),error,sizeof(error)));
        put("class/net/br0/type","1\n");
        assert(ugreenctl_wol_interface_identity_at(root,&w[i],"br0",identity,sizeof(identity),error,sizeof(error)));
    }
    puts("25 independent RTC and WOL profiles; identity, RTC ambiguity/resolution, PCI/DT NIC guards passed");
    return 0;
}
