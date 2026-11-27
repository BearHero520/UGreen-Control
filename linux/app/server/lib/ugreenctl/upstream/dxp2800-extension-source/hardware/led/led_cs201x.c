#define _POSIX_C_SOURCE 200809L
#include "led/led_cs201x.h"
#include "led/led_gpio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
#include <sys/io.h>
#define CS_NATIVE_IO 1
#else
#define CS_NATIVE_IO 0
#endif

/* Register facts: official kernel-6.12 c75c5abd, ug_201x.c and
 * leds-sio-201x.c. No vendor initialization or global-flow writes. */
#define CS_INDEX 0x2e
#define CS_VALUE 0x2f
#define CS_FLOW UINT32_C(0x20005440)
enum { CS_MODE,CS_BRIGHTNESS,CS_PERIOD,CS_DUTY,CS_RED,CS_GREEN,CS_BLUE,CS_FIELDS };
struct cs_raw { uint8_t byte[CS_FIELDS]; };
struct cs_io {
    int (*detect)(void *,struct ugreenctl_led_identity *,char *,size_t);
    int (*owners)(void *,uint16_t);
    int (*lock)(void *);
    void (*unlock)(void *);
    int (*permission)(void *,uint16_t,unsigned int,bool);
    int (*read_port)(void *,uint16_t,uint8_t *);
    int (*write_port)(void *,uint16_t,uint8_t);
    void (*pause_ms)(void *,unsigned int);
};
struct ugreenctl_led_cs201x {
    struct ugreenctl_led_identity identity;
    struct ugreenctl_led_access access;
    struct cs_io io;
    void *opaque;
    uint16_t base,chip_id,revision;
    int lock_fd[2];
    bool locked,config_permission,runtime_permission,confirmed;
    bool holder,restore_pending,identify_saved;
    uint8_t original_flow,identify_id;
    struct cs_raw original[4],identify_original;
    pid_t client_pid;
    char bus[64];
};

static int fail(char *error,size_t size,int result,const char *format,...)
{
    va_list arguments;
    if (error && size) {
        va_start(arguments,format);(void)vsnprintf(error,size,format,arguments);va_end(arguments);
    }
    return result;
}
static int production_detect(void *opaque,struct ugreenctl_led_identity *identity,char *error,size_t size)
{
    (void)opaque;return ugreenctl_led_detect(identity,error,size);
}
static int exists(const char *path)
{
    struct stat metadata;
    if (!lstat(path,&metadata)) return 1;
    return errno==ENOENT || errno==ENOTDIR ? 0 : -errno;
}
static int production_owners(void *opaque,uint16_t base)
{
    static const char *const owners[]={
        "/proc/it86","/sys/module/ug_201x_sio","/sys/module/ug_201x",
        "/sys/module/ug_it86x_sio","/sys/module/ug_it86x_cpufan",
        "/sys/module/ug_4800gt_sio","/sys/module/it87","/sys/module/nct6775",
        "/sys/bus/platform/drivers/ug-sio201/ug-sio201",
        "/sys/bus/platform/drivers/ug-sio201/ug-sio201.0"
    };
    struct ugreenctl_led_cs201x *context=opaque;
    FILE *stream;char line[512];size_t i;int result;
    for (i=0;i<sizeof(owners)/sizeof(owners[0]);++i) {
        result=exists(owners[i]);if (result) return result<0 ? result : -EBUSY;
    }
    result=ugreenctl_led_process_owners_except(context->client_pid);if (result<0) return result;
    stream=fopen("/proc/ioports","r");if (!stream) return -errno;
    result=0;
    while (fgets(line,sizeof(line),stream)) {
        unsigned long first,last;char name[400];
        if (sscanf(line," %lx-%lx : %399[^\n]",&first,&last,name)!=3) continue;
        /* A PCI bus container is not a device holding an I/O resource. */
        if (!strncmp(name,"PCI Bus ",8)) continue;
        if ((first<=CS_VALUE && last>=CS_INDEX) ||
            (base && first<=(unsigned long)base+12 && last>=base)) { result=-EBUSY;break; }
    }
    if (ferror(stream)) result=-EIO;
    (void)fclose(stream);return result;
}
static void production_unlock(void *opaque)
{
    struct ugreenctl_led_cs201x *context=opaque;int i;
    for (i=1;i>=0;--i) if (context->lock_fd[i]>=0) {
        (void)flock(context->lock_fd[i],LOCK_UN);(void)close(context->lock_fd[i]);context->lock_fd[i]=-1;
    }
}
static int production_lock(void *opaque)
{
    static const char *const paths[]={"/run/ugreenctl-led-mcu.lock","/run/ugreenctl-it8613.lock"};
    struct ugreenctl_led_cs201x *context=opaque;size_t i;int result=0;
    for (i=0;i<2;++i) {
        struct stat metadata;
        context->lock_fd[i]=open(paths[i],O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
        if (context->lock_fd[i]<0) { result=-errno;break; }
        if (fstat(context->lock_fd[i],&metadata)<0) { result=-errno;break; }
        if (!S_ISREG(metadata.st_mode) || metadata.st_uid!=0 || metadata.st_nlink!=1 ||
            (metadata.st_mode&0022)) { result=-EPERM;break; }
        if (flock(context->lock_fd[i],LOCK_EX|LOCK_NB)<0) { result=errno==EWOULDBLOCK ? -EBUSY : -errno;break; }
    }
    if (result) production_unlock(context);
    return result;
}
static int production_permission(void *opaque,uint16_t port,unsigned int count,bool enable)
{
    (void)opaque;
#if CS_NATIVE_IO
    return ioperm(port,count,enable ? 1 : 0)<0 ? -errno : 0;
#else
    (void)port;(void)count;(void)enable;return -ENOTSUP;
#endif
}
static int production_read(void *opaque,uint16_t port,uint8_t *value)
{
    (void)opaque;
#if CS_NATIVE_IO
    *value=inb(port);return 0;
#else
    (void)port;(void)value;return -ENOTSUP;
#endif
}
static int production_write(void *opaque,uint16_t port,uint8_t value)
{
    (void)opaque;
#if CS_NATIVE_IO
    outb(value,port);return 0;
#else
    (void)port;(void)value;return -ENOTSUP;
#endif
}
static void production_pause(void *opaque,unsigned int milliseconds)
{
    struct timespec duration; (void)opaque;
    duration.tv_sec=(time_t)(milliseconds/1000);duration.tv_nsec=(long)(milliseconds%1000)*1000000L;
    while (nanosleep(&duration,&duration)<0 && errno==EINTR) {}
}
static int port_write(struct ugreenctl_led_cs201x *c,uint16_t port,uint8_t value)
{
    return c->io.write_port(c->opaque,port,value);
}
static int sio_read(struct ugreenctl_led_cs201x *c,uint8_t index,uint8_t *value)
{
    int r=port_write(c,CS_INDEX,index);
    return r<0 ? r : c->io.read_port(c->opaque,CS_VALUE,value);
}
static int sio_word(struct ugreenctl_led_cs201x *c,uint8_t index,uint16_t *value)
{
    uint8_t high,low;int r=sio_read(c,index,&high);
    if (!r) r=sio_read(c,(uint8_t)(index+1),&low);
    if (!r) *value=(uint16_t)((uint16_t)high<<8|low);
    return r;
}
static bool sane_base(uint16_t base)
{
    /* The interface occupies offsets 0..12. Refuse low/unaligned/wrapped
     * addresses and any overlap with configuration ports. */
    return base>=0x100 && base<=0xfff0 && !(base&0x0f);
}
static int probe(struct ugreenctl_led_cs201x *c,uint16_t *base)
{
    static const uint8_t enter[]={0xa5,0x69,0x87},leave[]={0x87,0x69,0xa5};
    uint8_t old_ldn=0,active=0;uint16_t chip=0,revision=0;size_t i;
    int r=0,cleanup;bool selected=false;
    for (i=0;i<3 && !r;++i) { r=port_write(c,CS_INDEX,enter[i]);c->io.pause_ms(c->opaque,1); }
    if (!r) r=sio_word(c,0x20,&chip);
    if (!r && chip!=0x2011) r=-ENODEV;
    if (!r) r=sio_word(c,0x22,&revision);
    if (!r) r=sio_read(c,0x07,&old_ldn);
    if (!r) {
        r=port_write(c,CS_INDEX,0x07);
        if (!r) { selected=true;r=port_write(c,CS_VALUE,0x0a); }
    }
    if (!r) r=sio_read(c,0x30,&active);
    if (!r && active!=1) r=-ENODEV;
    if (!r) r=sio_word(c,0x60,base);
    if (!r && !sane_base(*base)) r=-ENODEV;
    if (selected) {
        cleanup=port_write(c,CS_INDEX,0x07);if (!cleanup) cleanup=port_write(c,CS_VALUE,old_ldn);
        if (!r && cleanup<0) r=cleanup;
    }
    for (i=0;i<3;++i) { cleanup=port_write(c,CS_INDEX,leave[i]);if (!r && cleanup<0) r=cleanup; }
    if (!r) { c->chip_id=chip;c->revision=revision; }
    return r;
}
static int address(struct ugreenctl_led_cs201x *c,uint32_t reg)
{
    int shift,r=port_write(c,(uint16_t)(c->base+1),0);
    for (shift=24;shift>=0 && !r;shift-=8)
        r=port_write(c,(uint16_t)(c->base+4+(unsigned int)shift/8),(uint8_t)(reg>>(unsigned int)shift));
    return r;
}
static int ec_read(struct ugreenctl_led_cs201x *c,uint32_t reg,uint8_t *value)
{
    int r=address(c,reg);if (r<0) return r;
    c->io.pause_ms(c->opaque,1);return c->io.read_port(c->opaque,(uint16_t)(c->base+8),value);
}
static uint32_t reg_address(uint8_t id,unsigned int field)
{
    static const uint32_t registers[]={0x20005400,0x20005404,0x20005408,0x2000540c,0x20005445,0x20005446,0x20005444};
    return registers[field]+(field>=CS_RED ? (uint32_t)id*4 : id);
}
/* expected_flow=-1 accepts only the two published values during initial
 * holder inspection. Normal operations always require exactly FLOW=1. */
static int guard_flow(struct ugreenctl_led_cs201x *c,bool writing,int expected_flow,
    uint8_t *observed_flow,char *error,size_t size)
{
    struct ugreenctl_led_identity identity;uint16_t base=0;uint8_t flow;int r;
    c->confirmed=false;
    if (writing && (!c->access.apply || !c->access.force))
        return fail(error,size,-EPERM,"CS2011 experimental writes require --force --apply");
    r=c->io.detect(c->opaque,&identity,error,size);if (r<0) return r;
    if (strcmp(identity.product_name,"DXP2800 GT") || strcmp(identity.source,"dmi") ||
        !identity.profile || !identity.profile->protocol || strcmp(identity.profile->protocol,"cs201x-sio") ||
        (c->identity.product_name[0] && !ugreenctl_led_same_identity(&identity,&c->identity)))
        return fail(error,size,-ENOTSUP,"CS2011 requires unchanged exact real DMI DXP2800 GT");
    r=c->io.owners(c->opaque,c->base);
    if (r<0) return fail(error,size,r,"CS2011 controller owner or reserved I/O resource is active");
    r=probe(c,&base);
    if (r<0) return fail(error,size,r,"CS2011 chip identity, active LDN or runtime base check failed");
    if (c->base && c->base!=base) return fail(error,size,-ESTALE,"CS2011 runtime base changed");
    if (!c->runtime_permission) {
        r=c->io.owners(c->opaque,base);if (r<0) return fail(error,size,r,"CS2011 runtime ports have an owner");
        r=c->io.permission(c->opaque,base,13,true);
        if (r<0) return fail(error,size,r,"cannot acquire CS2011 runtime I/O permission");
        c->base=base;c->runtime_permission=true;
    }
    r=ec_read(c,CS_FLOW,&flow);
    if (r<0) return fail(error,size,r,"cannot read CS2011 autonomous flow control");
    if (flow>1) return fail(error,size,-EBADMSG,"CS2011 flow value is outside the published 0/1 states");
    if (expected_flow>=0 && flow!=(uint8_t)expected_flow)
        return fail(error,size,-EBUSY,"CS2011 LED flow state changed; FLOW=0 requires ugreenctl-ledd --takeover-flow --force --apply with an already active LDN");
    if (observed_flow) *observed_flow=flow;
    c->identity=identity;c->confirmed=true;
    (void)snprintf(c->bus,sizeof(c->bus),"sio:0x2e,h2ec:0x%04x",c->base);
    return 0;
}
static int guard(struct ugreenctl_led_cs201x *c,bool writing,char *error,size_t size)
{ return guard_flow(c,writing,1,NULL,error,size); }
static int valid_raw(const struct cs_raw *raw)
{
    unsigned int mode=raw->byte[CS_MODE],on=raw->byte[CS_DUTY]*50U;
    unsigned int period=raw->byte[CS_PERIOD]*50U;
    if (mode>3) return -EBADMSG;
    if (mode>=2 && (period<=on || on<(mode==2 ? 100U : 1000U) || period-on<(mode==2 ? 100U : 500U))) return -EBADMSG;
    return 0;
}
static int snapshot(struct ugreenctl_led_cs201x *c,uint8_t id,struct cs_raw *raw,char *error,size_t size)
{
    struct cs_raw again;unsigned int i;int r;
    if (id>=4) return fail(error,size,-EINVAL,"CS2011 LED id must be 0..3");
    r=guard(c,false,error,size);if (r<0) return r;
    for (i=0;i<CS_FIELDS;++i) { r=ec_read(c,reg_address(id,i),&raw->byte[i]);if (r<0) break; }
    if (r<0) return fail(error,size,r,"cannot read complete CS2011 LED snapshot");
    for (i=0;i<CS_FIELDS;++i) { r=ec_read(c,reg_address(id,i),&again.byte[i]);if (r<0) break; }
    if (r<0) return fail(error,size,r,"cannot verify CS2011 LED snapshot");
    if (memcmp(raw,&again,sizeof(*raw)) || valid_raw(raw)<0)
        return fail(error,size,-EBADMSG,"CS2011 LED snapshot is unstable or invalid; refusing writes");
    return guard(c,false,error,size);
}
static int snapshot_all(struct ugreenctl_led_cs201x *c,int flow,struct cs_raw raw[4],char *error,size_t size)
{
    struct cs_raw again[4];unsigned int id,field;int r;
    r=guard_flow(c,false,flow,NULL,error,size);if (r<0) return r;
    for (id=0;id<4;++id) for (field=0;field<CS_FIELDS;++field) {
        r=ec_read(c,reg_address((uint8_t)id,field),&raw[id].byte[field]);
        if (r<0) return fail(error,size,r,"cannot read complete 28-byte CS2011 holder snapshot");
    }
    for (id=0;id<4;++id) for (field=0;field<CS_FIELDS;++field) {
        r=ec_read(c,reg_address((uint8_t)id,field),&again[id].byte[field]);
        if (r<0) return fail(error,size,r,"cannot verify complete CS2011 holder snapshot");
    }
    if (memcmp(raw,again,sizeof(again))) return fail(error,size,-EBADMSG,"CS2011 holder snapshot is unstable");
    for (id=0;id<4;++id) if (valid_raw(&raw[id])<0)
        return fail(error,size,-EBADMSG,"CS2011 holder snapshot has an invalid LED mode or timing");
    return guard_flow(c,false,flow,NULL,error,size);
}
static int change_flow(struct ugreenctl_led_cs201x *c,uint8_t before,uint8_t wanted,char *error,size_t size)
{
    uint8_t actual;int r;
    if (!c->holder || !c->restore_pending || before>1 || wanted>1)
        return fail(error,size,-EPERM,"CS2011 flow writes require an explicit snapshotted holder lifecycle");
    r=guard_flow(c,true,before,NULL,error,size);if (r<0) return r;
    r=address(c,CS_FLOW);if (!r) r=port_write(c,(uint16_t)(c->base+8),wanted);
    if (r<0) return fail(error,size,r,"CS2011 global flow write failed; state may have changed");
    c->io.pause_ms(c->opaque,1);r=ec_read(c,CS_FLOW,&actual);
    if (r<0 || actual!=wanted) return fail(error,size,r<0 ? r : -EIO,"CS2011 global flow readback mismatch");
    return guard_flow(c,false,wanted,NULL,error,size);
}
static void decode(const struct cs_raw *raw,struct ugreenctl_led_value *value)
{
    memset(value,0,sizeof(*value));value->mode=(enum ugreenctl_led_mode)raw->byte[CS_MODE];
    value->brightness=raw->byte[CS_BRIGHTNESS];value->red=raw->byte[CS_RED];
    value->green=raw->byte[CS_GREEN];value->blue=raw->byte[CS_BLUE];
    if (raw->byte[CS_PERIOD]>=raw->byte[CS_DUTY]) {
        value->on_ms=raw->byte[CS_DUTY]*50U;value->off_ms=(raw->byte[CS_PERIOD]-raw->byte[CS_DUTY])*50U;
    } else value->unknown_fields=UGREENCTL_LED_PATCH_TIMING;
}
static int validate_patch(const struct ugreenctl_led_patch *patch,char *error,size_t size)
{
    const unsigned int allowed=UGREENCTL_LED_PATCH_MODE|UGREENCTL_LED_PATCH_RGB|UGREENCTL_LED_PATCH_BRIGHTNESS|UGREENCTL_LED_PATCH_TIMING;
    if (!patch || !patch->fields || (patch->fields&~allowed)) return fail(error,size,-EINVAL,"unsupported CS2011 patch fields");
    if ((patch->fields&UGREENCTL_LED_PATCH_RGB) && (patch->value.red>255 || patch->value.green>255 || patch->value.blue>255))
        return fail(error,size,-EINVAL,"CS2011 RGB must be 0..255");
    if ((patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS) && patch->value.brightness>255)
        return fail(error,size,-EINVAL,"CS2011 brightness must be 0..255");
    if ((patch->fields&UGREENCTL_LED_PATCH_MODE) && (unsigned int)patch->value.mode>3)
        return fail(error,size,-EINVAL,"CS2011 mode must be off/on/blink/breath");
    if ((patch->fields&UGREENCTL_LED_PATCH_TIMING) &&
        (!patch->value.on_ms || !patch->value.off_ms || patch->value.on_ms>12750 || patch->value.off_ms>12750 ||
         patch->value.on_ms+patch->value.off_ms>12750 || patch->value.on_ms%50 || patch->value.off_ms%50))
        return fail(error,size,-EINVAL,"CS2011 timing requires 50 ms multiples and total <=12750 ms");
    return 0;
}
static int write_byte(struct ugreenctl_led_cs201x *c,uint8_t id,unsigned int field,uint8_t value,char *error,size_t size)
{
    uint8_t actual;uint32_t reg=reg_address(id,field);int r=guard(c,true,error,size);
    if (r<0) return r;
    r=address(c,reg);
    if (!r) r=port_write(c,(uint16_t)(c->base+8),value);
    if (r<0) return fail(error,size,r,"CS2011 target write failed; state may have changed");
    c->io.pause_ms(c->opaque,1);r=ec_read(c,reg,&actual);
    if (r<0 || actual!=value) return fail(error,size,r<0 ? r : -EIO,"CS2011 target register readback mismatch");
    return 0;
}
static int commit(struct ugreenctl_led_cs201x *c,uint8_t id,const struct cs_raw *wanted,
                  const struct cs_raw *before,bool restore,char *error,size_t size)
{
    static const unsigned int order[]={CS_RED,CS_GREEN,CS_BLUE,CS_BRIGHTNESS,CS_PERIOD,CS_DUTY,CS_MODE};
    struct cs_raw actual;size_t i;int r;bool quiesce=restore ||
        ((before->byte[CS_MODE]>=2) && (wanted->byte[CS_PERIOD]!=before->byte[CS_PERIOD] || wanted->byte[CS_DUTY]!=before->byte[CS_DUTY]));
    if (quiesce) { r=write_byte(c,id,CS_MODE,0,error,size);if (r<0) return r; }
    for (i=0;i<CS_FIELDS;++i) {
        unsigned int field=order[i];
        if (restore || wanted->byte[field]!=before->byte[field] || (quiesce && field==CS_MODE)) {
            r=write_byte(c,id,field,wanted->byte[field],error,size);if (r<0) return r;
        }
    }
    r=snapshot(c,id,&actual,error,size);if (r<0) return r;
    return memcmp(&actual,wanted,sizeof(actual)) ? fail(error,size,-EIO,"CS2011 complete target state changed after writing") : 0;
}
static int restore_after_error(struct ugreenctl_led_cs201x *c,uint8_t id,const struct cs_raw *before,
    const struct cs_raw *wanted,int cause,char *error,size_t size)
{
    char original[192],restore_error[192];int r;
    (void)snprintf(original,sizeof(original),"%s",error && size ? error : "CS2011 write failed");
    r=commit(c,id,before,wanted,true,restore_error,sizeof(restore_error));
    if (r<0) return fail(error,size,r,"%s; restoration failed: %s; state may have changed",original,restore_error);
    return fail(error,size,cause,"%s; complete previous LED state restored",original);
}
static int restore_holder(struct ugreenctl_led_cs201x *c,char *error,size_t size)
{
    struct cs_raw actual[4];uint8_t flow;unsigned int id;int r;
    if (!c->holder || !c->restore_pending) return 0;
    /* First finish an interrupted per-client identify. A complete baseline
     * restoration below may still recover a transient END readback failure. */
    if (c->identify_saved)
        (void)ugreenctl_led_cs201x_end_identify(c,c->identify_id,error,size);
    r=guard_flow(c,true,-1,&flow,error,size);if (r<0) return r;
    if (!flow) {
        /* Do not silently reacquire a flow lease that changed underneath us.
         * A failed initial takeover can already be back at the full baseline. */
        if (c->original_flow!=0) return fail(error,size,-EBUSY,"CS2011 flow changed during holder lifetime; restoration refused");
        r=snapshot_all(c,0,actual,error,size);if (r<0) return r;
        if (memcmp(actual,c->original,sizeof(actual)))
            return fail(error,size,-EBUSY,"CS2011 flow is autonomous and saved LED state differs; refusing a second takeover");
    } else {
        for (id=0;id<4;++id) {
            r=commit(c,(uint8_t)id,&c->original[id],&c->original[id],true,error,size);
            if (r<0) return r;
        }
        r=snapshot_all(c,1,actual,error,size);if (r<0) return r;
        if (memcmp(actual,c->original,sizeof(actual)))
            return fail(error,size,-EIO,"CS2011 complete 28-byte baseline did not restore");
        if (!c->original_flow) { r=change_flow(c,1,0,error,size);if (r<0) return r; }
    }
    c->restore_pending=false;c->identify_saved=false;
    if (error && size) error[0]=0;
    return 0;
}
static void release_context(struct ugreenctl_led_cs201x *c)
{
    if (!c) return;
    if (c->runtime_permission) (void)c->io.permission(c->opaque,c->base,13,false);
    if (c->config_permission) (void)c->io.permission(c->opaque,CS_INDEX,2,false);
    if (c->locked) c->io.unlock(c->opaque);
    free(c);
}

static int open_context(const struct ugreenctl_led_access *access,const struct cs_io *io,void *opaque,
    bool holder,bool takeover_flow,struct ugreenctl_led_cs201x **out,char *error,size_t size)
{
    struct ugreenctl_led_cs201x *c;struct ugreenctl_led_identity identity;struct cs_raw actual[4];int r;
    if (!out || !access) return fail(error,size,-EINVAL,"missing CS2011 access or output");
    *out=NULL;c=calloc(1,sizeof(*c));if (!c) return -ENOMEM;
    c->access=*access;c->holder=holder;c->lock_fd[0]=c->lock_fd[1]=-1;
    if (holder && (!access->force || !access->apply)) {
        free(c);return fail(error,size,-EPERM,"starting a CS2011 holder requires --force --apply");
    }
    if (io) { c->io=*io;c->opaque=opaque; }
    else { c->io=(struct cs_io){production_detect,production_owners,production_lock,production_unlock,
        production_permission,production_read,production_write,production_pause};c->opaque=c; }
    /* Reject wrong machines before locks, ioperm or any port access. */
    r=c->io.detect(c->opaque,&identity,error,size);if (r<0) goto failed;
    if (strcmp(identity.product_name,"DXP2800 GT") || strcmp(identity.source,"dmi") ||
        !identity.profile || !identity.profile->protocol || strcmp(identity.profile->protocol,"cs201x-sio")) {
        r=fail(error,size,-ENOTSUP,"CS2011 transport requires exact real DMI DXP2800 GT");goto failed;
    }
    c->identity=identity;r=c->io.owners(c->opaque,0);if (r<0) goto failed;
    r=c->io.lock(c->opaque);if (r<0) goto failed;c->locked=true;
    r=c->io.owners(c->opaque,0);if (r<0) goto failed;
    r=c->io.permission(c->opaque,CS_INDEX,2,true);if (r<0) goto failed;c->config_permission=true;
    if (!holder) { r=guard(c,false,error,size);if (r<0) goto failed; }
    else {
        r=guard_flow(c,false,-1,&c->original_flow,error,size);if (r<0) goto failed;
        if (!c->original_flow && !takeover_flow) {
            r=fail(error,size,-EBUSY,"CS2011 FLOW=0 needs the explicit holder --takeover-flow option");goto failed;
        }
        r=snapshot_all(c,c->original_flow,c->original,error,size);if (r<0) goto failed;
        c->restore_pending=true;
        if (!c->original_flow) { r=change_flow(c,0,1,error,size);if (r<0) goto failed; }
        r=snapshot_all(c,1,actual,error,size);if (r<0) goto failed;
        if (memcmp(actual,c->original,sizeof(actual))) {
            r=fail(error,size,-EIO,"CS2011 flow takeover changed LED registers; refusing holder startup");goto failed;
        }
    }
    *out=c;return 0;
failed:
    if (c->restore_pending) {
        char cause[192],restoration[192];int recovered;
        (void)snprintf(cause,sizeof(cause),"%s",error && size && error[0] ? error : "CS2011 holder startup failed");
        recovered=restore_holder(c,restoration,sizeof(restoration));
        if (recovered<0) r=fail(error,size,recovered,"%s; baseline restoration failed: %s; state may have changed",cause,restoration);
        else (void)fail(error,size,r,"%s; full previous holder state restored",cause);
    }
    release_context(c);
    if (error && size && !error[0]) (void)snprintf(error,size,"CS2011 open failed: %s",strerror(-r));
    return r;
}
int ugreenctl_led_cs201x_open(const struct ugreenctl_led_access *access,struct ugreenctl_led_cs201x **out,char *error,size_t size)
{
    if (error && size) error[0]=0;
    if (!CS_NATIVE_IO) {
        if (out) *out=NULL;
        return fail(error,size,-ENOTSUP,"CS2011 port I/O is only available on Linux x86/x86_64");
    }
    return open_context(access,NULL,NULL,false,false,out,error,size);
}
int ugreenctl_led_cs201x_open_holder(const struct ugreenctl_led_access *access,bool takeover_flow,
    struct ugreenctl_led_cs201x **out,char *error,size_t size)
{
    if (error && size) error[0]=0;
    if (!CS_NATIVE_IO) {
        if (out) *out=NULL;
        return fail(error,size,-ENOTSUP,"CS2011 port I/O is only available on Linux x86/x86_64");
    }
    return open_context(access,NULL,NULL,true,takeover_flow,out,error,size);
}
void ugreenctl_led_cs201x_set_client(struct ugreenctl_led_cs201x *c,pid_t authenticated_pid)
{ if (c) c->client_pid=authenticated_pid>0 ? authenticated_pid : 0; }
const struct ugreenctl_led_identity *ugreenctl_led_cs201x_identity(const struct ugreenctl_led_cs201x *c) { return c ? &c->identity : NULL; }
bool ugreenctl_led_cs201x_identity_confirmed(const struct ugreenctl_led_cs201x *c) { return c && c->confirmed; }
uint16_t ugreenctl_led_cs201x_chip_id(const struct ugreenctl_led_cs201x *c) { return c ? c->chip_id : 0; }
const char *ugreenctl_led_cs201x_bus(const struct ugreenctl_led_cs201x *c) { return c ? c->bus : ""; }
int ugreenctl_led_cs201x_read(struct ugreenctl_led_cs201x *c,uint8_t id,struct ugreenctl_led_value *value,char *error,size_t size)
{
    struct cs_raw raw;int r;
    if (!c || !value) return -EINVAL;
    r=snapshot(c,id,&raw,error,size);if (!r) decode(&raw,value);return r;
}
int ugreenctl_led_cs201x_apply(struct ugreenctl_led_cs201x *c,uint8_t id,const struct ugreenctl_led_patch *patch,
    struct ugreenctl_led_value *readback,char *error,size_t size)
{
    struct cs_raw before,wanted;int r;
    if (!c || id>=4) return -EINVAL;
    if (c->identify_saved) return fail(error,size,-EBUSY,"CS2011 identify must be restored before another write");
    r=validate_patch(patch,error,size);if (r<0) return r;
    r=guard(c,true,error,size);if (r<0) return r;
    r=snapshot(c,id,&before,error,size);if (r<0) return r;wanted=before;
    if (patch->fields&UGREENCTL_LED_PATCH_RGB) {
        wanted.byte[CS_RED]=(uint8_t)patch->value.red;wanted.byte[CS_GREEN]=(uint8_t)patch->value.green;wanted.byte[CS_BLUE]=(uint8_t)patch->value.blue;
    }
    if (patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS) wanted.byte[CS_BRIGHTNESS]=(uint8_t)patch->value.brightness;
    if (patch->fields&UGREENCTL_LED_PATCH_MODE) wanted.byte[CS_MODE]=(uint8_t)patch->value.mode;
    if (patch->fields&UGREENCTL_LED_PATCH_TIMING) {
        wanted.byte[CS_PERIOD]=(uint8_t)((patch->value.on_ms+patch->value.off_ms)/50);
        wanted.byte[CS_DUTY]=(uint8_t)(patch->value.on_ms/50);
    }
    if (valid_raw(&wanted)<0) return fail(error,size,-EINVAL,"CS2011 blink/breath timing does not meet mode limits");
    r=commit(c,id,&wanted,&before,false,error,size);
    if (r<0) return restore_after_error(c,id,&before,&wanted,r,error,size);
    if (readback) decode(&wanted,readback);
    return 0;
}
int ugreenctl_led_cs201x_begin_identify(struct ugreenctl_led_cs201x *c,uint8_t id,char *error,size_t size)
{
    struct cs_raw wanted;int r;
    if (!c || id>=4) return -EINVAL;
    if (c->identify_saved) return fail(error,size,-EBUSY,"CS2011 already has a saved identify state");
    r=guard(c,true,error,size);if (r<0) return r;
    r=snapshot(c,id,&c->identify_original,error,size);if (r<0) return r;
    c->identify_id=id;c->identify_saved=true;wanted=c->identify_original;
    wanted.byte[CS_MODE]=2;wanted.byte[CS_BRIGHTNESS]=255;wanted.byte[CS_PERIOD]=8;wanted.byte[CS_DUTY]=4;
    wanted.byte[CS_RED]=0;wanted.byte[CS_GREEN]=0;wanted.byte[CS_BLUE]=255;
    r=commit(c,id,&wanted,&c->identify_original,false,error,size);
    /* Keep the raw saved state even after successful best-effort rollback.
     * The service must acknowledge END or retry it while disconnecting. */
    return r<0 ? restore_after_error(c,id,&c->identify_original,&wanted,r,error,size) : 0;
}
int ugreenctl_led_cs201x_end_identify(struct ugreenctl_led_cs201x *c,uint8_t id,char *error,size_t size)
{
    int r;
    if (!c || id>=4) return -EINVAL;
    if (!c->identify_saved) return fail(error,size,-ENOENT,"CS2011 has no saved identify state");
    if (id!=c->identify_id) return fail(error,size,-EINVAL,"CS2011 identify target does not match the saved raw state");
    r=commit(c,id,&c->identify_original,&c->identify_original,true,error,size);
    if (r<0) {
        char detail[192];(void)snprintf(detail,sizeof(detail),"%s",error && size ? error : "I/O error");
        return fail(error,size,r,"CS2011 identify raw restoration failed: %s; state may have changed",detail);
    }
    c->identify_saved=false;return 0;
}
int ugreenctl_led_cs201x_identify(struct ugreenctl_led_cs201x *c,uint8_t id,unsigned int seconds,
    const volatile sig_atomic_t *cancelled,char *error,size_t size)
{
    struct cs_raw before,wanted;unsigned int ticks;int r;
    if (!c || id>=4 || seconds<1 || seconds>60) return -EINVAL;
    if (c->identify_saved) return fail(error,size,-EBUSY,"CS2011 already has a saved identify state");
    r=guard(c,true,error,size);if (r<0) return r;
    if (cancelled && *cancelled) return fail(error,size,-EINTR,"CS2011 identify cancelled before writing");
    r=snapshot(c,id,&before,error,size);if (r<0) return r;wanted=before;
    wanted.byte[CS_MODE]=2;wanted.byte[CS_BRIGHTNESS]=255;wanted.byte[CS_PERIOD]=8;wanted.byte[CS_DUTY]=4;
    wanted.byte[CS_RED]=0;wanted.byte[CS_GREEN]=0;wanted.byte[CS_BLUE]=255;
    r=commit(c,id,&wanted,&before,false,error,size);
    if (r<0) return restore_after_error(c,id,&before,&wanted,r,error,size);
    for (ticks=0;ticks<seconds*20 && !(cancelled && *cancelled);++ticks) c->io.pause_ms(c->opaque,50);
    r=commit(c,id,&before,&wanted,true,error,size);
    if (r<0) {
        char detail[192];(void)snprintf(detail,sizeof(detail),"%s",error && size ? error : "I/O error");
        return fail(error,size,r,"CS2011 identify restoration failed: %s; state may have changed",detail);
    }
    return cancelled && *cancelled ? fail(error,size,-EINTR,"CS2011 identify interrupted; complete previous state restored") : 0;
}
void ugreenctl_led_cs201x_close(struct ugreenctl_led_cs201x *c)
{
    if (!c) return;
    if (c->holder) (void)restore_holder(c,NULL,0);
    else if (c->identify_saved) (void)ugreenctl_led_cs201x_end_identify(c,c->identify_id,NULL,0);
    release_context(c);
}
int ugreenctl_led_cs201x_restore_close(struct ugreenctl_led_cs201x *c,char *error,size_t size)
{
    int r=0;
    if (!c) return 0;
    if (c->holder) r=restore_holder(c,error,size);
    else if (c->identify_saved) r=ugreenctl_led_cs201x_end_identify(c,c->identify_id,error,size);
    if (r<0) {
        char detail[192];(void)snprintf(detail,sizeof(detail),"%s",error && size ? error : "I/O error");
        (void)fail(error,size,r,"CS2011 holder restoration failed: %s; state may have changed",detail);
    }
    release_context(c);return r;
}
#ifdef UGREENCTL_LED_TESTING
int ugreenctl_led_cs201x_test_open(const struct ugreenctl_led_access *access,
    const struct ugreenctl_led_cs201x_test_io *io,void *opaque,struct ugreenctl_led_cs201x **out,char *error,size_t size)
{
    struct cs_io transport;
    if (error && size) error[0]=0;
    if (!io || !io->detect || !io->owners || !io->lock || !io->unlock || !io->permission ||
        !io->read_port || !io->write_port || !io->pause_ms) return -EINVAL;
    transport=(struct cs_io){io->detect,io->owners,io->lock,io->unlock,io->permission,io->read_port,io->write_port,io->pause_ms};
    return open_context(access,&transport,opaque,false,false,out,error,size);
}
int ugreenctl_led_cs201x_test_open_holder(const struct ugreenctl_led_access *access,bool takeover_flow,
    const struct ugreenctl_led_cs201x_test_io *io,void *opaque,struct ugreenctl_led_cs201x **out,char *error,size_t size)
{
    struct cs_io transport;
    if (error && size) error[0]=0;
    if (!io || !io->detect || !io->owners || !io->lock || !io->unlock || !io->permission ||
        !io->read_port || !io->write_port || !io->pause_ms) return -EINVAL;
    transport=(struct cs_io){io->detect,io->owners,io->lock,io->unlock,io->permission,io->read_port,io->write_port,io->pause_ms};
    return open_context(access,&transport,opaque,true,takeover_flow,out,error,size);
}
#endif
