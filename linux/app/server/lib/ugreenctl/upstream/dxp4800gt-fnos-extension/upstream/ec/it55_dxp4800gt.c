#define _GNU_SOURCE
#include "ec/it55_dxp4800gt.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__i386__) || defined(__x86_64__)
#include <sys/io.h>
#endif

/* Firmware 1.21.1.0016, ug_4800gt-sio. These are EC memory offsets,
 * NOT IT8613 registers. Only this exact model is routed here. */
#define CMD 0x66
#define DATA 0x62
#ifndef UGREENCTL_GT_TESTING
struct it55_gt_io {
    int (*identity)(struct ugreenctl_machine_identity *);
    int (*owners)(char *, size_t);
    int (*acquire)(void);
    void (*release)(void);
    uint8_t (*input)(unsigned short);
    void (*output)(uint8_t, unsigned short);
    void (*pause)(void);
};
#endif
static int lock_fd = -1;
static bool ports;
static int real_identity(struct ugreenctl_machine_identity *id)
{ return ugreenctl_identity_read(id, NULL, 0); }

static int check_ioports(FILE *f,char *detail,size_t size)
{
    char line[512];
    bool visible=false;
    while (fgets(line,sizeof(line),f)) {
        unsigned long start,end; char name[256];
        if (sscanf(line," %lx-%lx : %255[^\n]",&start,&end,name)!=3) continue;
        if (start || end) visible=true;
        if ((start<=DATA && end>=DATA) || (start<=CMD && end>=CMD)) {
            /* Broad PCI root windows are containers, not EC owners. */
            if (end-start<16 || strstr(name,"EC") || strstr(name,"ec")) {
                snprintf(detail,size,"/proc/ioports %04lx-%04lx : %s",start,end,name);
                return -EBUSY;
            }
        }
    }
    if (ferror(f)) return -EIO;
    return visible ? 0 : -EACCES;
}

static int check_acpi_devices(DIR *devices,char *detail,size_t size)
{
    for (;;) {
        errno=0;
        struct dirent *entry=readdir(devices);
        if (!entry) return -errno;
        if (!strncmp(entry->d_name,"PNP0C09:",8) || !strncmp(entry->d_name,"LNXEC:",6)) {
            snprintf(detail,size,"ACPI EC device: %s",entry->d_name);
            return -EBUSY;
        }
    }
}

static int acpi_owners(char *detail,size_t size)
{
    const char *tables="/sys/firmware/acpi/tables";
    const char *device_dir="/sys/bus/acpi/devices";
    struct stat st;
    if (access(tables,R_OK|X_OK)<0) {
        int r=-errno;
        snprintf(detail,size,"cannot inspect %s",tables);
        return r;
    }
    if (!lstat("/sys/firmware/acpi/tables/ECDT",&st)) {
        snprintf(detail,size,"ACPI boot EC table: ECDT");
        return -EBUSY;
    }
    if (errno!=ENOENT) return -errno;
    /* Linux may run an EC even if request_region failed. Refuse a declared
     * EC as well, and require the namespace to be visible before direct I/O. */
    DIR *devices=opendir(device_dir);
    if (!devices) {
        int r=-errno;
        snprintf(detail,size,"cannot inspect %s",device_dir);
        return r;
    }
    int r=check_acpi_devices(devices,detail,size);
    closedir(devices);
    return r;
}

static int real_owners(char *detail,size_t size)
{
    /* ec_sys can load with first_ec == NULL; only a real debugfs EC node
     * is ownership evidence. The ACPI namespace and port checks run even
     * when the debugfs driver is disabled or debugfs is not mounted. */
    const char *paths[] = {"/proc/it86", "/sys/module/ug_4800gt_sio", "/sys/kernel/debug/ec"};
    struct stat st;
    for (size_t i=0;i<sizeof(paths)/sizeof(paths[0]);++i) {
        if (!lstat(paths[i], &st)) {
            snprintf(detail,size,"present: %s",paths[i]);
            return -EBUSY;
        }
        if (errno != ENOENT) return -errno;
    }
    /* An active ACPI EC owns these ports even if ec_sys is not loaded.
     * Never race the kernel or unload a driver to gain access. */
    FILE *f=fopen("/proc/ioports","r");
    if (!f) return -errno;
    int result=check_ioports(f,detail,size);
    fclose(f);
    return result ? result : acpi_owners(detail,size);
}
static void real_release(void)
{
#if defined(__i386__) || defined(__x86_64__)
    if (ports) { (void)ioperm(DATA,1,0); (void)ioperm(CMD,1,0); ports=false; }
#endif
    if (lock_fd>=0) { close(lock_fd); lock_fd=-1; }
}
static int real_acquire(void)
{
#if defined(__i386__) || defined(__x86_64__)
    struct stat st;
    lock_fd=open("/run/ugreenctl-it55-ec.lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if (lock_fd<0) return -errno;
    if (fstat(lock_fd,&st)<0) return -errno;
    if (!S_ISREG(st.st_mode) || st.st_uid!=0 || st.st_nlink!=1) return -EPERM;
    if (flock(lock_fd,LOCK_EX|LOCK_NB)<0) return -errno;
    if (ioperm(DATA,1,1)<0) return -errno;
    if (ioperm(CMD,1,1)<0) { int e=errno; (void)ioperm(DATA,1,0); return -e; }
    ports=true;
    return 0;
#else
    return -ENOTSUP;
#endif
}
static uint8_t real_input(unsigned short port)
{
#if defined(__i386__) || defined(__x86_64__)
    return inb(port);
#else
    (void)port; return 0xff;
#endif
}
static void real_output(uint8_t value,unsigned short port)
{
#if defined(__i386__) || defined(__x86_64__)
    outb(value,port);
#else
    (void)value; (void)port;
#endif
}
static void real_pause(void) { struct timespec delay={0,1000000L}; nanosleep(&delay,NULL); }
static struct it55_gt_io io={real_identity,real_owners,real_acquire,real_release,real_input,real_output,real_pause};
#ifdef UGREENCTL_GT_TESTING
void it55_gt_test_io(const struct it55_gt_io *ops) { io=*ops; }
int it55_gt_test_ioports(FILE *f,char *detail,size_t size) { return check_ioports(f,detail,size); }
int it55_gt_test_acpi_devices(const char *path,char *detail,size_t size) {
    DIR *devices=opendir(path);
    if (!devices) return -errno;
    int r=check_acpi_devices(devices,detail,size);
    closedir(devices);
    return r;
}
#endif

static int wait_status(uint8_t mask,bool set)
{
    for (unsigned int n=0;n<50;++n) {
        uint8_t status=io.input(CMD);
        if (status==0xff) return -ENODEV;
        if (!!(status & mask)==set) return 0;
        io.pause();
    }
    return -ETIMEDOUT;
}
static int send(uint8_t value,unsigned short port)
{
    int r=wait_status(2,false);
    if (!r) io.output(value,port);
    return r;
}
static int read_mem(uint8_t reg,uint8_t *value)
{
    /* Unconsumed output belongs to another transaction; never drain it. */
    uint8_t status=io.input(CMD);
    if (status==0xff) return -ENODEV;
    if (status&1) return -EBUSY;
    int r=send(0x80,CMD);
    if (!r) r=send(reg,DATA);
    if (!r) r=wait_status(1,true);
    if (!r) *value=io.input(DATA);
    return r;
}
static int write_mem(uint8_t reg,uint8_t value)
{
    int r=send(0x81,CMD);
    if (!r) r=send(reg,DATA);
    if (!r) r=send(value,DATA);
    if (!r) r=wait_status(2,false);
    return r;
}
struct session {
    struct ugreenctl_machine_identity id;
    uint8_t version[3];
    bool acquired;
    const char *stage;
    char owner[160];
};
static int identity(struct ugreenctl_machine_identity *id)
{
    int r=io.identity(id);
    if (r) return r;
    if (strcmp(id->product_name,"DXP4800 GT") || strcmp(id->manufacturer,"UGREEN")) return -ENODEV;
    return 0;
}
static int version(uint8_t v[3])
{
    int r=read_mem(0,&v[0]);
    if (!r) r=read_mem(1,&v[1]);
    if (!r) r=read_mem(3,&v[2]);
    if (!r && ((!v[0] && !v[1] && !v[2]) || v[0]==255 || v[1]==255 || v[2]==255)) r=-ENODEV;
    return r;
}
static int begin(struct session *s)
{
    memset(s,0,sizeof(*s));
    s->stage="DMI identity";
    int r=identity(&s->id);
    if (!r) { s->stage="controller owner check"; r=io.owners(s->owner,sizeof(s->owner)); }
    if (!r) { s->stage="exclusive lock / port permission"; s->acquired=true; r=io.acquire(); }
    if (!r) { s->stage="controller owner recheck"; r=io.owners(s->owner,sizeof(s->owner)); }
    if (!r) { s->stage="EC version read (busy means unread output at 0x66)"; r=version(s->version); }
    if (!r) s->stage="EC transaction";
    return r;
}
static int recheck(struct session *s)
{
    struct ugreenctl_machine_identity id={0}; uint8_t v[3];
    s->stage="DMI recheck";
    int r=identity(&id);
    if (!r && (strcmp(id.product_version,s->id.product_version) || strcmp(id.bios_version,s->id.bios_version))) r=-ESTALE;
    if (!r) { s->stage="controller owner recheck"; r=io.owners(s->owner,sizeof(s->owner)); }
    if (!r) { s->stage="EC version recheck"; r=version(v); }
    if (!r && memcmp(v,s->version,sizeof(v))) r=-ESTALE;
    if (!r) s->stage="EC transaction";
    return r;
}
static int finish(struct session *s,int r,char *error,size_t size)
{
    if (s->acquired) io.release();
    if (r && error && size) snprintf(error,size,"DXP4800 GT EC [%s]: %s%s%s",s->stage?s->stage:"argument validation",
        strerror(-r),s->owner[0]?"; ":"",s->owner);
    return r;
}
int it55_gt_read_fans(struct ugreenctl_fan_status *fans,size_t *count,char *error,size_t size)
{
    struct session s; int r=begin(&s); *count=0;
    for (unsigned int i=0;!r && i<2;++i) {
        uint8_t hi,lo,mode,pwm,base=i?0xb4:0xb0,tach=i?0x38:0x34;
        r=read_mem(tach,&hi); if (!r) r=read_mem(tach+1,&lo);
        if (!r) r=read_mem(base,&mode);
        if (!r) r=read_mem(base+1,&pwm);
        if (!r) {
            struct ugreenctl_fan_status *f=&fans[i]; memset(f,0,sizeof(*f));
            snprintf(f->id,sizeof(f->id),"%s",i?"sys":"cpu");
            f->rpm=((unsigned int)hi<<8)|lo; f->tachometer=(uint16_t)f->rpm;
            f->mode_known=mode<=1; f->manual=mode==1;
            f->pwm_known=pwm<=198; f->pwm=pwm; ++*count;
        }
    }
    if (!r) r=recheck(&s);
    if (r) *count=0;
    return finish(&s,r,error,size);
}
int it55_gt_set_fan(bool force,const char *fan,uint8_t pwm,char *error,size_t size)
{
    struct session s={0}; int r=0; uint8_t base,actual;
    if (!force) r=-EPERM;
    else if (pwm<40 || pwm>198) r=-ERANGE;
    else if (strcmp(fan,"cpu") && strcmp(fan,"sys")) r=-EINVAL;
    if (r) return finish(&s,r,error,size);
    base=!strcmp(fan,"cpu")?0xb0:0xb4;
    r=begin(&s);
    if (!r) r=recheck(&s);
    if (!r) r=write_mem(base,1);
    if (!r) r=read_mem(base,&actual);
    if (!r && actual!=1) r=-EIO;
    if (!r) r=write_mem(base+1,pwm);
    if (!r) r=read_mem(base+1,&actual);
    if (!r && actual!=pwm) r=-EIO;
    return finish(&s,r,error,size);
}
int it55_gt_read_startup(enum ugreenctl_startup_policy *policy,char *error,size_t size)
{
    struct session s; int r=begin(&s);
    *policy=UGREENCTL_STARTUP_UNKNOWN;
    /* Stock get returns a cached variable. 0x9a is a separate G3 control;
     * neither is evidence of the persistent 0x88/0x89 startup policy. */
    return finish(&s,r,error,size);
}
int it55_gt_set_startup(bool force,enum ugreenctl_startup_policy policy,char *error,size_t size)
{
    struct session s={0}; int r=0;
    if (!force) r=-EPERM;
    else if (policy!=UGREENCTL_STARTUP_ON && policy!=UGREENCTL_STARTUP_OFF) r=-ENOTSUP;
    if (r) return finish(&s,r,error,size);
    r=begin(&s);
    if (!r) r=recheck(&s);
    if (!r) r=send(policy==UGREENCTL_STARTUP_ON?0x88:0x89,CMD);
    if (!r) r=wait_status(2,false);
    /* Same command-status check as stock; this is NOT policy readback. */
    if (!r && !(io.input(CMD)&8)) r=-EIO;
    return finish(&s,r,error,size);
}
