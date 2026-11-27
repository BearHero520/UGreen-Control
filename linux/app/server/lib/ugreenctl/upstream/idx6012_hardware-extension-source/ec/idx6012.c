#define _GNU_SOURCE
#include "ec/idx6012.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* Firmware ug_it55_functions.c: ITE5571 non-Pro protocol.
 * User-authorized 2026-11-15 exception ONLY for the captured OEM board/BIOS:
 * exact platform identity substitutes for unavailable numeric silicon ID.
 * This exception does not enable power controls or other board revisions. */
#ifndef UGREENCTL_IDX6012_TESTING
struct idx6012_io {
    int (*identity)(struct ugreenctl_machine_identity *);
    int (*owners)(void); int (*acquire)(void); void (*release)(void);
    int (*read_port)(unsigned short, uint8_t *);
    int (*send_port)(unsigned short, uint8_t); void (*pause)(void);
};
#endif
static int port_fd=-1, lock_fd=-1;
static int identity(struct ugreenctl_machine_identity *v)
{ return ugreenctl_identity_read(v,NULL,0); }
static int absent(const char *p)
{
    struct stat st;
    if (!lstat(p,&st)) return -EBUSY;
    return errno==ENOENT ? 0 : -errno;
}
static int contains(const char *p,const char *needle)
{
    FILE *f=fopen(p,"r"); char line[1024]; int found=0;
    if (!f) return -errno;
    while (fgets(line,sizeof(line),f)) if (strstr(line,needle)) found=1;
    if (ferror(f)) found=-EIO;
    fclose(f); return found;
}
static int owners(void)
{
    const char *paths[]={"/proc/it86","/sys/module/ug_idx6011_sio",
        "/sys/module/ug_idx6011pro_sio","/sys/module/ug_4800gt_sio"};
    int r;
    for (size_t n=0;n<sizeof(paths)/sizeof(paths[0]);++n)
        if ((r=absent(paths[n]))) return r;
    DIR *d=opendir("/sys/bus/acpi/devices"); struct dirent *e;
    if (!d) return -errno;
    r=0;
    while ((e=readdir(d))) {
        if (strncmp(e->d_name,"PNP0C09:",8)) continue;
        char p[512]; unsigned status;
        snprintf(p,sizeof(p),"/sys/bus/acpi/devices/%s/status",e->d_name);
        FILE *f=fopen(p,"r");
        if (!f) { r=-errno; break; }
        int ok=fscanf(f,"%u",&status); fclose(f);
        if (ok!=1 || status) { r=-EBUSY; break; }
        snprintf(p,sizeof(p),"/sys/bus/acpi/devices/%s/driver",e->d_name);
        if ((r=absent(p))) break;
    }
    closedir(d); if (r) return r;
    r=absent("/sys/module/ec_sys");
    if (r && r!=-EBUSY) return r;
    if (r==-EBUSY) {
        struct utsname u; char p[512];
        if (uname(&u)) return -errno;
        snprintf(p,sizeof(p),"/boot/config-%s",u.release);
        if (contains(p,"CONFIG_ACPI_EC_DEBUGFS=y\n")!=1 ||
            contains("/proc/modules","ec_sys ")!=0 ||
            contains("/proc/mounts"," /sys/kernel/debug debugfs ")!=1 ||
            absent("/sys/kernel/debug/ec")) return -EBUSY;
        d=opendir("/sys/bus/acpi/drivers/ec");
        if (!d) return -errno;
        r=0;
        while ((e=readdir(d))) {
            if (e->d_name[0]=='.') continue;
            struct stat st;
            snprintf(p,sizeof(p),"/sys/bus/acpi/drivers/ec/%s",e->d_name);
            if (lstat(p,&st)) { r=-errno; break; }
            if (S_ISLNK(st.st_mode)) { r=-EBUSY; break; }
        }
        closedir(d); if (r) return r;
    }
    FILE *f=fopen("/proc/ioports","r"); char line[512]; bool visible=false;
    if (!f) return -errno;
    r=0;
    while (fgets(line,sizeof(line),f)) {
        unsigned long lo,hi; char name[256];
        if (sscanf(line," %lx-%lx : %255[^\n]",&lo,&hi,name)!=3) continue;
        visible |= (lo || hi);
        if (((lo<=0x62 && hi>=0x62)||(lo<=0x66 && hi>=0x66)) &&
            strncmp(name,"PCI Bus ",8)) { r=-EBUSY; break; }
    }
    if (ferror(f)) r=-EIO;
    fclose(f); return r ? r : visible ? 0 : -EACCES;
}
static void release(void)
{
    if (port_fd>=0) close(port_fd);
    if (lock_fd>=0) close(lock_fd);
    port_fd=lock_fd=-1;
}
static int acquire(void)
{
#if !defined(__i386__) && !defined(__x86_64__)
    return -ENOTSUP;
#else
    struct stat st;
    lock_fd=open("/run/ugreenctl-it55-ec.lock",O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);
    if (lock_fd<0) return -errno;
    if (fstat(lock_fd,&st)) return -errno;
    if (!S_ISREG(st.st_mode)||st.st_uid||st.st_nlink!=1) return -EPERM;
    if (flock(lock_fd,LOCK_EX|LOCK_NB)) return -errno;
    port_fd=open("/dev/port",O_RDWR|O_CLOEXEC);
    return port_fd<0 ? -errno : 0;
#endif
}
static int input(unsigned short p,uint8_t *v)
{ ssize_t n=pread(port_fd,v,1,p); return n==1 ? 0 : n<0 ? -errno : -EIO; }
static int output(unsigned short p,uint8_t v)
{ ssize_t n=pwrite(port_fd,&v,1,p); return n==1 ? 0 : n<0 ? -errno : -EIO; }
static void pause_io(void) { struct timespec t={0,1000000}; nanosleep(&t,NULL); }
static struct idx6012_io io={identity,owners,acquire,release,input,output,pause_io};
#ifdef UGREENCTL_IDX6012_TESTING
void idx6012_test_io(const struct idx6012_io *v) { io=*v; }
#endif
static int wait_for(uint8_t mask,bool set)
{
    for (unsigned n=0;n<100;++n) {
        uint8_t v; int r=io.read_port(0x66,&v);
        if (r) return r;
        if (v==255) return -ENODEV;
        if (!!(v&mask)==set) return 0;
        io.pause();
    }
    return -ETIMEDOUT;
}
static int read_byte(uint8_t reg,uint8_t *v)
{
    uint8_t s; int r=io.read_port(0x66,&s);
    if (r) return r;
    if (s==255) return -ENODEV;
    if (s&1) return -EBUSY;
    r=wait_for(2,false);
    if (!r) r=io.send_port(0x66,0x80);
    if (!r) r=wait_for(2,false);
    if (!r) r=io.send_port(0x62,reg);
    if (!r) r=wait_for(1,true);
    if (!r) r=io.read_port(0x62,v);
    return r;
}
static int check_identity(struct ugreenctl_machine_identity *v)
{
    int r=io.identity(v);
    if (r) return r;
    if (strcmp(v->source,"dmi")||strcmp(v->manufacturer,"UGREEN")) return -ENODEV;
    if (!strcmp(v->product_name,"iDX6012")) return 0;
    return !strcmp(v->product_name,"Default string") &&
        !strcmp(v->product_version,"iDX6011_iDX6012 V3.0 241112") ? 0 : -ENODEV;
}
static int read_values(uint8_t v[9],char *error,size_t size)
{
    struct ugreenctl_machine_identity before={0},after={0};
    const uint8_t regs[]={0x96,0x97,0x98,0x99,0x9c,0x9d,0x9e,0x9f,0xa6};
    bool acquired=false; int r=check_identity(&before);
    if (!r) r=io.owners();
    if (!r) { acquired=true; r=io.acquire(); }
    if (!r) r=io.owners();
    for (size_t n=0;!r && n<4;n+=2) {
        bool stable=false;
        for (unsigned retry=0;!r && retry<3;++retry) {
            uint8_t hi;
            r=read_byte(regs[n],&v[n]);
            if (!r) r=read_byte(regs[n+1],&v[n+1]);
            if (!r) r=read_byte(regs[n],&hi);
            if (!r && hi==v[n]) { stable=true; break; }
        }
        if (!r && !stable) r=-EAGAIN;
    }
    for (size_t n=4;!r && n<sizeof(regs);++n) r=read_byte(regs[n],&v[n]);
    if (!r) r=check_identity(&after);
    if (!r && memcmp(&before,&after,sizeof(before))) r=-ESTALE;
    if (!r) r=io.owners();
    if (acquired) io.release();
    if (r && error && size) snprintf(error,size,"iDX6012 read-only EC: %s",strerror(-r));
    return r;
}
int idx6012_read_fans(struct ugreenctl_fan_status *fans,size_t *count,char *e,size_t z)
{
    uint8_t v[9]; *count=0; int r=read_values(v,e,z);
    if (r) return r;
    for (unsigned n=0;n<2;++n) {
        struct ugreenctl_fan_status *f=&fans[n]; memset(f,0,sizeof(*f));
        snprintf(f->id,sizeof(f->id),"sys%u",n+1);
        f->rpm=((unsigned)v[n*2]<<8)|v[n*2+1]; f->tachometer=(uint16_t)f->rpm;
        f->mode_known=v[4+n*2]<=1; f->manual=v[4+n*2]==1;
        f->pwm_known=f->manual && v[5+n*2]<=198;
        if (f->pwm_known) f->pwm=v[5+n*2];
    }
    *count=2; return 0;
}
int idx6012_read_g3(unsigned int *gate,char *e,size_t z)
{
    uint8_t v[9]; int r=read_values(v,e,z);
    if (!r && v[8]>1) r=-ERANGE;
    if (!r) *gate=v[8];
    return r;
}

static int write_checked(uint8_t reg,uint8_t value)
{
    uint8_t s,actual;
    int r=io.read_port(0x66,&s);
    if (!r && (s==255 || (s&1))) r=-EBUSY;
    if (!r) r=wait_for(2,false);
    if (!r) r=io.send_port(0x66,0x81);
    if (!r) r=wait_for(2,false);
    if (!r) r=io.send_port(0x62,reg);
    if (!r) r=wait_for(2,false);
    if (!r) r=io.send_port(0x62,value);
    if (!r) r=wait_for(2,false);
    if (!r) r=read_byte(reg,&actual);
    if (!r && actual!=value) r=-EIO;
    return r;
}
static int write_identity(struct ugreenctl_machine_identity *v)
{
    int r=check_identity(v);
    if (!r && (strcmp(v->product_version,"iDX6011_iDX6012 V3.0 241112") ||
               strcmp(v->bios_version,"5.32") || strcmp(v->bios_date,"11/12/2024"))) r=-ENODEV;
    return r;
}
static int recheck(const struct ugreenctl_machine_identity *before)
{
    struct ugreenctl_machine_identity after={0}; int r=write_identity(&after);
    if (!r && memcmp(before,&after,sizeof(after))) r=-ESTALE;
    if (!r) r=io.owners();
    return r;
}
static int change_fans(bool force,const char *channel,bool automatic,uint8_t pwm,char *e,size_t z)
{
    struct ugreenctl_machine_identity before={0};
    uint8_t saved[4]={0}; bool acquired=false,touched=false;
    int r=0;
    if (!force) r=-EPERM;
    else if (strcmp(channel,"sys")) r=-EINVAL;
    else if (!automatic && (pwm<40 || pwm>198)) r=-ERANGE;
    if (!r) r=write_identity(&before);
    if (!r) r=io.owners();
    if (!r) { acquired=true; r=io.acquire(); }
    if (!r) r=recheck(&before);
    for (unsigned n=0;!r && n<4;++n) r=read_byte(0x9c+n,&saved[n]);
    if (!r && (saved[0]>1 || saved[2]>1 ||
        (saved[0] && (saved[1]<40 || saved[1]>198)) ||
        (saved[2] && (saved[3]<40 || saved[3]>198)))) r=-ERANGE;
    for (unsigned n=0;!r && n<2;++n) {
        r=recheck(&before);
        if (r) break;
        touched=true;
        if (automatic) r=write_checked(0x9c+n*2,0);
        else {
            /* Set duty first: enabling with stale zero would stop a fan. */
            r=write_checked(0x9d+n*2,pwm);
            if (!r) r=write_checked(0x9c+n*2,1);
        }
    }
    if (!r) r=recheck(&before);
    if (r && touched) {
        int rollback=recheck(&before);
        for (unsigned n=0;!rollback && n<2;++n) {
            if (!saved[n*2]) {
                rollback=write_checked(0x9c+n*2,0);
                if (!rollback) rollback=write_checked(0x9d+n*2,saved[n*2+1]);
            } else {
                rollback=write_checked(0x9d+n*2,saved[n*2+1]);
                if (!rollback) rollback=write_checked(0x9c+n*2,1);
            }
        }
        if (rollback) r=-EUCLEAN;
    }
    if (acquired) io.release();
    if (r && e && z) snprintf(e,z,"iDX6012 fan transaction: %s%s",strerror(-r),
        r==-EUCLEAN?"; rollback not confirmed, inspect both fans immediately":"");
    return r;
}
int idx6012_set_fan(bool force,const char *channel,uint8_t pwm,char *e,size_t z)
{ return change_fans(force,channel,false,pwm,e,z); }
int idx6012_set_mode(bool force,const char *channel,bool automatic,char *e,size_t z)
{
    if (!automatic) { if(e&&z) snprintf(e,z,"Use fan set sys PWM to enter manual mode safely"); return -ENOTSUP; }
    return change_fans(force,channel,true,0,e,z);
}
