#define _DEFAULT_SOURCE
#include "core/wake_cli.h"
#include "core/identity.h"
#include "models/wake_profiles.h"
#include "network/wol.h"
#include "network/wol_interface.h"
#include "power/rtc_wake.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int wake_error(const char *message)
{
    (void)fprintf(stderr,"error: %s\n",message); return EXIT_FAILURE;
}
static int wake_lock(bool rtc, char *error, size_t size)
{
    struct stat st;
    const char *path=rtc ? "/run/ugreenctl-rtc.lock" : "/run/ugreenctl-wol.lock";
    int fd=open(path,O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,0600);
    if (fd<0) goto failed;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=0 || st.st_nlink!=1 ||
        (st.st_mode & 0022) || flock(fd,LOCK_EX | LOCK_NB)) {
        (void)close(fd); goto failed;
    }
    return fd;
failed:
    (void)snprintf(error,size,"cannot obtain trusted exclusive wake lock %s",path); return -1;
}
static bool model_option_matches(const char *requested, const char *id, const char *product)
{
    if (!requested || !strcmp(requested,id) || !strcmp(requested,product)) return true;
    /* Retain the existing exact plugin-family selectors, never bypass identity. */
    if (!strcmp(requested,"dx4600") && (!strcmp(product,"DX4600+") || !strcmp(product,"DX4600 Pro"))) return true;
    return !strcmp(requested,"dxp4800plus") && !strcmp(product,"DXP4800 Pro");
}
int ugreenctl_wake_cli(int argc, char **argv)
{
    const char *positionals[4]={0}, *model=NULL, *interface=NULL;
    bool apply=false,force=false,rtc=false,write=false,list_interfaces=false;
    size_t count=0,n;
    int i,command=-1,result=0,lock=-1;
    char error[256]={0};
    struct ugreenctl_machine_identity identity,again;
    const struct ugreenctl_rtc_profile *rp=NULL;
    const struct ugreenctl_wol_profile *wp=NULL;
    time_t epoch=0;
    enum ugreenctl_wol_policy policy=UGREENCTL_WOL_UNKNOWN;
    /* Locate the actual command, skipping option arguments. */
    for (i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--model") || !strcmp(argv[i],"--plugin-dir") || !strcmp(argv[i],"--interface")) { ++i; continue; }
        if (!strncmp(argv[i],"--",2)) continue;
        command=i; break;
    }
    if (command<0 || command+1>=argc) return -1;
    rtc=!strcmp(argv[command],"power") && !strcmp(argv[command+1],"rtc-wake");
    if (!rtc && (strcmp(argv[command],"network") || strcmp(argv[command+1],"wol"))) return -1;
    for (i=1;i<argc;++i) {
        const char *a=argv[i];
        if (!strcmp(a,"--force")) { force=true; continue; }
        if (!strcmp(a,"--apply")) { apply=true; continue; }
        if (!strcmp(a,"--model") || !strcmp(a,"--interface") || !strcmp(a,"--plugin-dir")) {
            if (++i>=argc) return wake_error("missing wake option argument");
            if (!strcmp(a,"--model")) { if (model) return wake_error("duplicate --model"); model=argv[i]; }
            if (!strcmp(a,"--interface")) { if (interface) return wake_error("duplicate --interface"); interface=argv[i]; }
            continue; /* Built-in wake tables do not load plugins. */
        }
        if (!strncmp(a,"--",2) || count==4) return wake_error("invalid wake arguments");
        positionals[count++]=a;
    }
    if (rtc && interface) return wake_error("--interface only applies to WOL");
    if (count==3 && !strcmp(positionals[2],"models")) {
        if (model || interface || apply || force) return wake_error("models listing takes no hardware options");
        if (rtc) {
            rp=ugreenctl_rtc_profiles(&n);
            for (count=0;count<n;++count)
                (void)printf("%s\t%s\trtc=%s\tfirmware-evidence; actual wake unverified\n",rp[count].id,rp[count].product,rp[count].rtc_name);
        } else {
            wp=ugreenctl_wol_profiles(&n);
            for (count=0;count<n;++count)
                (void)printf("%s\t%s\twol=%s\tactual wake unverified\n",wp[count].id,wp[count].product,
                             wp[count].automatic_pci_count ? "automatic-or-explicit" : "explicit-interface-required");
        }
        return EXIT_SUCCESS;
    }
    if (count<3) return wake_error("use get, set, clear (RTC), models, or interfaces (WOL)");
    if (!rtc && count==3 && !strcmp(positionals[2],"interfaces")) {
        if (interface || apply || force) return wake_error("interface listing takes no write/interface options");
        list_interfaces=true;
    } else if (!strcmp(positionals[2],"set") && count==4) {
        write=true;
        if (rtc) {
            const char *s=positionals[3];
            for (;*s;++s) if (*s<'0' || *s>'9') return wake_error("RTC requires a positive Unix epoch");
            if (ugreenctl_rtc_wake_parse(positionals[3],&epoch,error,sizeof(error)) || epoch<=0) return wake_error("invalid RTC epoch");
        } else {
            if (!strcmp(positionals[3],"on")) policy=UGREENCTL_WOL_ON;
            else if (!strcmp(positionals[3],"off")) policy=UGREENCTL_WOL_OFF;
            else return wake_error("WOL policy must be on or off");
        }
    } else if (rtc && count==3 && !strcmp(positionals[2],"clear")) write=true;
    else if (count!=3 || strcmp(positionals[2],"get")) return wake_error("invalid wake command");

    if (ugreenctl_identity_read(&identity,error,sizeof(error))) return wake_error(error);
    if (rtc) {
        rp=ugreenctl_rtc_match(&identity);
        if (!rp) return wake_error("RTC is not independently firmware-mapped for this exact machine");
        if (!model_option_matches(model,rp->id,rp->product)) return wake_error("selected RTC model does not match machine identity");
        if (epoch && ugreenctl_rtc_validate_epoch(rp,epoch,time(NULL),error,sizeof(error))) return wake_error(error);
    } else {
        wp=ugreenctl_wol_match(&identity);
        if (!wp) return wake_error("WOL is not firmware-mapped for this exact machine");
        if (!model_option_matches(model,wp->id,wp->product)) return wake_error("selected WOL model does not match machine identity");
        if (!list_interfaces && !interface && !wp->automatic_pci_count)
            return wake_error("automatic NIC mapping is unconfirmed; specify --interface <physical-ethernet-name>");
    }
    if (list_interfaces) {
        char names[32][UGREENCTL_WOL_NAME_SIZE];
        size_t total=0,index;
        result=ugreenctl_wol_interfaces_at("/sys",wp,names,32,&total,error,sizeof(error));
        if (result) return wake_error(error[0] ? error : "cannot enumerate physical Ethernet interfaces");
        for (index=0;index<total;++index) (void)puts(names[index]);
        return EXIT_SUCCESS;
    }
    if (write && !apply) {
        (void)puts("dry-run: wake request validated against exact model; no hardware access; use --force --apply");
        return EXIT_SUCCESS;
    }
    if (write) {
        if (!force) return wake_error("wake control is experimental; requires --force --apply");
        lock=wake_lock(rtc,error,sizeof(error));
        if (lock<0) return wake_error(error);
        result=ugreenctl_identity_read(&again,error,sizeof(error));
        if (result || memcmp(&identity,&again,sizeof(identity))) {
            (void)close(lock); return wake_error("machine/board/BIOS identity changed before wake write");
        }
    }
    if (rtc) {
        if (!write) result=ugreenctl_rtc_wake_read(rp,&epoch,error,sizeof(error));
        else if (epoch) result=ugreenctl_rtc_wake_set(rp,epoch,error,sizeof(error));
        else result=ugreenctl_rtc_wake_clear(rp,error,sizeof(error));
    } else if (interface) {
        result=ugreenctl_wol_interface(wp,interface,write,&policy,error,sizeof(error));
    } else {
        const char *stock[]={"eth0","eth1"};
        if (write) result=ugreenctl_wol_set_policy_with_renamed_count(stock,2,wp->automatic_pci_count,policy,error,sizeof(error));
        else result=ugreenctl_wol_read_policy_with_renamed_count(stock,2,wp->automatic_pci_count,&policy,error,sizeof(error));
    }
    if (lock>=0) (void)close(lock);
    if (result) return wake_error(error[0] ? error : "wake operation failed; state not confirmed");
    if (write) {
        if (rtc) (void)printf("RTC alarm %s: %lld; interface readback verified; actual power-on unverified\n",epoch ? "set" : "cleared",(long long)epoch);
        else (void)printf("WOL %s; driver readback verified; shutdown wake unverified\n",policy==UGREENCTL_WOL_ON ? "on" : "off");
    } else if (rtc) (void)printf("%lld\n",(long long)epoch);
    else (void)puts(policy==UGREENCTL_WOL_ON ? "on" : "off");
    return EXIT_SUCCESS;
}
