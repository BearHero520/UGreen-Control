#define _XOPEN_SOURCE 700
#include "led/led_gpio.h"
#include "core/identity.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/gpio.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Hardware facts recovered from official DTBs and ug-gpio-leds; this file
 * implements the documented Linux GPIO v2 userspace ABI independently. */
struct gpio_bank {
    const char *node;
    unsigned int offsets[12],count;
    char chip[64],device[PATH_MAX],of_node[PATH_MAX];
    int chip_fd,line_fd;
};
struct gpio_io {
    int (*open_chip)(void *,const char *);
    int (*control)(void *,int,unsigned long,void *);
    void (*close_fd)(void *,int);
    int (*owners)(void *),(*lock)(void *);
    void (*unlock)(void *);
};
struct ugreenctl_led_gpio {
    struct ugreenctl_led_identity identity;
    struct ugreenctl_machine_identity machine;
    struct ugreenctl_led_access access;
    struct gpio_io io;
    void *opaque;
    char root[PATH_MAX],device_root[PATH_MAX];
    struct gpio_bank banks[2];
    unsigned int bank_count;
    unsigned int bank_for_led[UGREENCTL_MAX_LEDS],index_for_led[UGREENCTL_MAX_LEDS];
    int lock_fd;
    pid_t client_pid;
    bool locked;
};

static int fail(char *error,size_t size,int result,const char *format,...)
{
    va_list arguments;
    if (error && size) {
        va_start(arguments,format);(void)vsnprintf(error,size,format,arguments);va_end(arguments);
    }
    return result;
}
static int read_binary(const char *path,unsigned char *buffer,size_t capacity,size_t *length)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    ssize_t count;
    if (fd<0) return -errno;
    count=read(fd,buffer,capacity);
    if (count<0) { int result=-errno;(void)close(fd);return result; }
    (void)close(fd);
    if ((size_t)count==capacity) return -EOVERFLOW;
    *length=(size_t)count;return 0;
}
static unsigned int be32(const unsigned char *p)
{
    return (unsigned int)p[0]<<24|(unsigned int)p[1]<<16|(unsigned int)p[2]<<8|p[3];
}
static int property(struct ugreenctl_led_gpio *context,const char *node,const char *name,
                    unsigned char *data,size_t capacity,size_t *length)
{
    char path[PATH_MAX];
    if (snprintf(path,sizeof(path),"%s/firmware/devicetree/base%s/%s",context->root,node,name)>=(int)sizeof(path)) return -ENAMETOOLONG;
    return read_binary(path,data,capacity,length);
}
static bool property_string(struct ugreenctl_led_gpio *context,const char *node,const char *name,const char *expected)
{
    unsigned char data[128];size_t length;
    return property(context,node,name,data,sizeof(data),&length)==0 &&
        length==strlen(expected)+1 && !memcmp(data,expected,length);
}
static int owners(void *opaque)
{
    struct ugreenctl_led_gpio *context=opaque;
    return ugreenctl_led_process_owners_except(context->client_pid);
}
static int open_chip(void *opaque,const char *path)
{
    struct stat metadata;int fd;(void)opaque;
    fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return -errno;
    if (fstat(fd,&metadata)<0) { int result=-errno;(void)close(fd);return result; }
    if (!S_ISCHR(metadata.st_mode)) { (void)close(fd);return -ENODEV; }
    return fd;
}
static int control(void *opaque,int fd,unsigned long request,void *argument)
{
    (void)opaque;return ioctl(fd,request,argument)<0 ? -errno : 0;
}
static void close_fd(void *opaque,int fd) { (void)opaque;(void)close(fd); }
static int lock_controller(void *opaque)
{
    struct ugreenctl_led_gpio *context=opaque;struct stat metadata;
    context->lock_fd=open("/run/ugreenctl-led-mcu.lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if (context->lock_fd<0) return -errno;
    if (fstat(context->lock_fd,&metadata)<0) return -errno;
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid!=0 || metadata.st_nlink!=1) return -EPERM;
    return flock(context->lock_fd,LOCK_EX|LOCK_NB)<0 ? (errno==EWOULDBLOCK ? -EBUSY : -errno) : 0;
}
static void unlock_controller(void *opaque)
{
    struct ugreenctl_led_gpio *context=opaque;
    if (context->lock_fd>=0) { (void)flock(context->lock_fd,LOCK_UN);(void)close(context->lock_fd);context->lock_fd=-1; }
}

static int configure_layout(struct ugreenctl_led_gpio *context)
{
    static const unsigned int rk3576[]={30,31,28,29,15,13,14,12};
    static const unsigned int rk3588a[]={22,4,8,20},rk3588b[]={13,14,9,10,11,12,7,8};
    size_t i;
    const char *name=ugreenctl_identity_model(&context->machine);
    if (!strcmp(name,"DH2300")) {
        context->banks[0].node="/pinctrl/gpio@2ae20000";context->banks[0].count=8;
        memcpy(context->banks[0].offsets,rk3576,sizeof(rk3576));context->bank_count=1;
    } else if (!strcmp(name,"DH4300 Plus")) {
        context->banks[0].node="/pinctrl/gpio@fd8a0000";context->banks[0].count=4;
        context->banks[1].node="/pinctrl/gpio@fec50000";context->banks[1].count=8;
        memcpy(context->banks[0].offsets,rk3588a,sizeof(rk3588a));
        memcpy(context->banks[1].offsets,rk3588b,sizeof(rk3588b));context->bank_count=2;
    } else return -ENOTSUP;
    for (i=0;i<context->identity.profile->led_count;++i) {
        unsigned int bank=context->bank_count==2 && i>=2 ? 1 : 0;
        context->bank_for_led[i]=bank;context->index_for_led[i]=(unsigned int)(bank==1 ? i-2 : i)*2;
    }
    return 0;
}

static int verify_tree(struct ugreenctl_led_gpio *context,char *error,size_t size)
{
    struct ugreenctl_machine_identity current;
    unsigned char data[256],handle[8];size_t length,handle_length;
    unsigned int bank,index=0;
    int result=ugreenctl_identity_read_at(context->root,&current,error,size);
    if (result<0) return result;
    if (strcmp(current.source,"devicetree") || strcmp(current.product_name,context->machine.product_name) ||
        strcmp(current.compatible,context->machine.compatible))
        return fail(error,size,-ENOTSUP,"GPIO fallback requires the exact verified device-tree model and compatible");
    if (!property_string(context,"/ug-led","compatible","ug-rockchip-led") ||
        !property_string(context,"/ug-led","status","okay"))
        return fail(error,size,-ENOTSUP,"verified ug-led device-tree binding is absent");
    if (property(context,"/ug-led","led_num",data,sizeof(data),&length)<0 || length!=4 ||
        be32(data)!=context->identity.profile->led_count)
        return fail(error,size,-ENOTSUP,"GPIO LED count differs from official firmware");
    if (property(context,"/ug-led","leds-gpios",data,sizeof(data),&length)<0 || length!=144)
        return fail(error,size,-ENOTSUP,"GPIO wiring property differs from official firmware");
    /* Both official DTBs contain 12 triples, but DH2300 registers only the
     * first eight. Never request the four unused entries. */
    for (bank=0;bank<context->bank_count;++bank) {
        struct gpio_bank *entry=&context->banks[bank];unsigned int i,phandle;
        if (!property_string(context,entry->node,"compatible","rockchip,gpio-bank") ||
            property(context,entry->node,"phandle",handle,sizeof(handle),&handle_length)<0 || handle_length!=4)
            return fail(error,size,-ENOTSUP,"GPIO bank identity is unavailable");
        phandle=be32(handle);
        if (!phandle) return -ENOTSUP;
        for (i=0;i<entry->count;++i,++index)
            if (be32(data+index*12)!=phandle || be32(data+index*12+4)!=entry->offsets[i] || be32(data+index*12+8)!=0)
                return fail(error,size,-ENOTSUP,"GPIO pin routing differs from the exact official model");
    }
    return 0;
}

static int chip_path(struct ugreenctl_led_gpio *context,const char *name,char *device,char *of_node)
{
    char path[PATH_MAX],expected[PATH_MAX];
    if (snprintf(path,sizeof(path),"%s/bus/gpio/devices/%s",context->root,name)>=(int)sizeof(path)) return -ENAMETOOLONG;
    if (!realpath(path,device)) return -errno;
    if (strncmp(device,context->root,strlen(context->root)) || strncmp(device+strlen(context->root),"/devices/",9)) return -EPERM;
    (void)snprintf(path,sizeof(path),"%s/of_node",device);
    if (!realpath(path,of_node)) {
        (void)snprintf(path,sizeof(path),"%s/device/of_node",device);
        if (!realpath(path,of_node)) return -errno;
    }
    (void)snprintf(expected,sizeof(expected),"%s/firmware/devicetree/base/",context->root);
    if (strncmp(of_node,expected,strlen(expected))) return -EPERM;
    return 0;
}

static int discover(struct ugreenctl_led_gpio *context,char *error,size_t size)
{
    char directory[PATH_MAX],path[PATH_MAX],device[PATH_MAX],of_node[PATH_MAX],expected[PATH_MAX];
    struct dirent *item;DIR *stream;unsigned int bank;int result=0;
    (void)snprintf(directory,sizeof(directory),"%s/bus/gpio/devices",context->root);
    stream=opendir(directory);
    if (!stream) return fail(error,size,-ENOTSUP,"GPIO v2 character devices are unavailable");
    while ((item=readdir(stream))) {
        const char *number=item->d_name+8;size_t digits;
        if (strncmp(item->d_name,"gpiochip",8)) continue;
        digits=strspn(number,"0123456789");if (!digits || number[digits]) continue;
        result=chip_path(context,item->d_name,device,of_node);
        if (result<0) break;
        for (bank=0;bank<context->bank_count;++bank) {
            struct gpio_bank *entry=&context->banks[bank];
            (void)snprintf(expected,sizeof(expected),"%s/firmware/devicetree/base%s",context->root,entry->node);
            if (strcmp(of_node,expected)) continue;
            if (entry->chip[0]) { result=-ENOTSUP;break; }
            (void)snprintf(entry->chip,sizeof(entry->chip),"%s",item->d_name);
            (void)snprintf(entry->device,sizeof(entry->device),"%s",device);
            (void)snprintf(entry->of_node,sizeof(entry->of_node),"%s",of_node);
        }
        if (result<0) break;
    }
    (void)closedir(stream);
    if (result<0) return fail(error,size,result,"GPIO bank discovery was ambiguous or unsafe");
    for (bank=0;bank<context->bank_count;++bank) {
        struct gpio_bank *entry=&context->banks[bank];struct gpiochip_info info={0};
        struct gpio_v2_line_request request={0};unsigned int i;
        if (!entry->chip[0]) return fail(error,size,-ENOTSUP,"no GPIO chip is bound to required bank %s",entry->node);
        (void)snprintf(path,sizeof(path),"%s/%s",context->device_root,entry->chip);
        entry->chip_fd=context->io.open_chip(context->opaque,path);
        if (entry->chip_fd<0) return fail(error,size,entry->chip_fd,"GPIO device open failed");
        result=context->io.control(context->opaque,entry->chip_fd,GPIO_GET_CHIPINFO_IOCTL,&info);
        if (result<0 || strcmp(info.name,entry->chip) || info.lines!=32)
            return fail(error,size,result<0 ? result : -ENOTSUP,"GPIO chip identity/range does not match the Rockchip bank");
        for (i=0;i<entry->count;++i) {
            struct gpio_v2_line_info line={0};line.offset=entry->offsets[i];
            result=context->io.control(context->opaque,entry->chip_fd,GPIO_V2_GET_LINEINFO_IOCTL,&line);
            if (result<0) return fail(error,size,result,"GPIO line metadata is unavailable");
            if (line.flags&GPIO_V2_LINE_FLAG_USED) return fail(error,size,-EBUSY,"GPIO LED line %u has a kernel or userspace owner",line.offset);
            request.offsets[i]=entry->offsets[i];
        }
        request.num_lines=entry->count;
        (void)snprintf(request.consumer,sizeof(request.consumer),"ugreenctl-ledd");
        /* flags=0 is the documented as-is request: no pin direction/value
         * changes while opening for diagnostics or before --apply. */
        result=context->io.control(context->opaque,entry->chip_fd,GPIO_V2_GET_LINE_IOCTL,&request);
        if (result<0) return fail(error,size,result,"cannot exclusively request GPIO LED lines as-is");
        entry->line_fd=request.fd;
    }
    return 0;
}

static int verify(struct ugreenctl_led_gpio *context,char *error,size_t size)
{
    char device[PATH_MAX],of_node[PATH_MAX];unsigned int bank;
    int result=context->io.owners(context->opaque);
    if (result<0) return fail(error,size,result,"another LED controller process is active");
    result=verify_tree(context,error,size);if (result<0) return result;
    for (bank=0;bank<context->bank_count;++bank) {
        struct gpio_bank *entry=&context->banks[bank];struct gpiochip_info info={0};
        result=chip_path(context,entry->chip,device,of_node);
        if (result<0 || strcmp(device,entry->device) || strcmp(of_node,entry->of_node))
            return fail(error,size,-ENOTSUP,"GPIO chip binding changed during service lifetime");
        result=context->io.control(context->opaque,entry->chip_fd,GPIO_GET_CHIPINFO_IOCTL,&info);
        if (result<0 || strcmp(info.name,entry->chip) || info.lines!=32)
            return fail(error,size,-ENOTSUP,"GPIO chip identity changed");
    }
    return 0;
}

static int open_internal(const char *root,const char *device_root,const struct ugreenctl_led_access *access,
                         const struct gpio_io *io,void *opaque,struct ugreenctl_led_gpio **output,char *error,size_t size)
{
    struct ugreenctl_led_gpio *context;int result;unsigned int i;
    if (!root || !device_root || !access || !output) return -EINVAL;
    *output=NULL;context=calloc(1,sizeof(*context));if (!context) return -ENOMEM;
    context->lock_fd=-1;
    for (i=0;i<2;++i) context->banks[i].chip_fd=context->banks[i].line_fd=-1;
    if (!realpath(root,context->root) || !realpath(device_root,context->device_root)) { result=-errno;goto failed; }
    context->access=*access;
    if (io) { context->io=*io;context->opaque=opaque; }
    else { context->io=(struct gpio_io){open_chip,control,close_fd,owners,lock_controller,unlock_controller};context->opaque=context; }
    result=ugreenctl_identity_read_at(context->root,&context->machine,error,size);if (result<0) goto failed;
    if (strcmp(context->machine.source,"devicetree")) { result=-ENOTSUP;goto failed; }
    context->identity.profile=ugreenctl_led_profile_find(ugreenctl_identity_model(&context->machine),context->machine.product_version);
    if (!context->identity.profile || strcmp(context->identity.profile->protocol,"sysfs-gpio")) { result=-ENOTSUP;goto failed; }
    (void)snprintf(context->identity.source,sizeof(context->identity.source),"%s",context->machine.source);
    (void)snprintf(context->identity.product_name,sizeof(context->identity.product_name),"%s",context->machine.product_name);
    (void)snprintf(context->identity.compatible,sizeof(context->identity.compatible),"%s",context->machine.compatible);
    result=configure_layout(context);if (result<0) goto failed;
    result=verify_tree(context,error,size);if (result<0) goto failed;
    result=context->io.owners(context->opaque);if (result<0) goto failed;
    result=context->io.lock(context->opaque);if (result<0) goto failed;context->locked=true;
    result=discover(context,error,size);if (result<0) goto failed;
    *output=context;return 0;
failed:
    if (error && size && !error[0]) (void)snprintf(error,size,"GPIO fallback open failed: %s",strerror(-result));
    ugreenctl_led_gpio_close(context);return result;
}

int ugreenctl_led_gpio_open(const struct ugreenctl_led_access *access,struct ugreenctl_led_gpio **context,char *error,size_t size)
{
    if (error && size) error[0]=0;
    if (geteuid()!=0) return fail(error,size,-EPERM,"GPIO holder requires root privileges");
    return open_internal("/sys","/dev",access,NULL,NULL,context,error,size);
}
const struct ugreenctl_led_identity *ugreenctl_led_gpio_identity(const struct ugreenctl_led_gpio *context)
{
    return context ? &context->identity : NULL;
}
bool ugreenctl_led_gpio_identity_confirmed(struct ugreenctl_led_gpio *context)
{
    return context && verify(context,NULL,0)==0;
}
void ugreenctl_led_gpio_set_client(struct ugreenctl_led_gpio *context,pid_t authenticated_pid)
{
    if (context) context->client_pid=authenticated_pid>0 ? authenticated_pid : 0;
}
static int target_index(struct ugreenctl_led_gpio *context,uint8_t id)
{
    size_t i;for (i=0;i<context->identity.profile->led_count;++i)
        if (context->identity.profile->leds[i].id==id) return (int)i;
    return -1;
}
static enum ugreenctl_led_color second_color(const struct ugreenctl_led_gpio *context)
{
    return !strcmp(context->identity.profile->product_name,"DH2300") ? UGREENCTL_LED_COLOR_ORANGE : UGREENCTL_LED_COLOR_RED;
}
static unsigned int selected_channel(enum ugreenctl_led_color color)
{
    return color==UGREENCTL_LED_COLOR_WHITE || color==UGREENCTL_LED_COLOR_CHANNEL1 ? 1 : 2;
}
int ugreenctl_led_gpio_read(struct ugreenctl_led_gpio *context,uint8_t id,struct ugreenctl_led_value *value,char *error,size_t size)
{
    struct gpio_v2_line_values values={0};struct gpio_bank *entry;unsigned int offset,bits;int index,result;
    if (!context || !value || (index=target_index(context,id))<0) return -EINVAL;
    result=verify(context,error,size);if (result<0) return result;
    entry=&context->banks[context->bank_for_led[index]];offset=context->index_for_led[index];values.mask=3ULL<<offset;
    result=context->io.control(context->opaque,entry->line_fd,GPIO_V2_LINE_GET_VALUES_IOCTL,&values);
    if (result<0) return fail(error,size,result,"GPIO line readback failed");
    bits=(unsigned int)(values.bits>>offset)&3U;
    memset(value,0,sizeof(*value));value->unknown_fields=UGREENCTL_LED_PATCH_RGB|UGREENCTL_LED_PATCH_TIMING;
    value->mode=bits ? UGREENCTL_LED_MODE_ON : UGREENCTL_LED_MODE_OFF;value->brightness=bits!=0;
    if (bits==1) value->color=UGREENCTL_LED_COLOR_WHITE;
    else if (bits==2) value->color=second_color(context);
    else value->unknown_fields|=UGREENCTL_LED_PATCH_COLOR;
    return 0;
}
int ugreenctl_led_gpio_apply(struct ugreenctl_led_gpio *context,uint8_t id,const struct ugreenctl_led_patch *patch,
                            struct ugreenctl_led_value *output,char *error,size_t size)
{
    struct ugreenctl_led_value before,after;struct gpio_v2_line_config config={0};struct gpio_bank *entry;
    unsigned int offset,channel,on;int index,result;
    if (!context || (index=target_index(context,id))<0) return -EINVAL;
    result=ugreenctl_led_validate_target_patch(&context->identity.profile->leds[index],patch,error,size);if (result<0) return result;
    if (!context->access.force || !context->access.apply) return fail(error,size,-EPERM,"experimental GPIO writes require --force --apply");
    if ((patch->fields&UGREENCTL_LED_PATCH_MODE) && (patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS) &&
        (patch->value.mode==UGREENCTL_LED_MODE_ON)!=(patch->value.brightness!=0))
        return fail(error,size,-ENOTSUP,"binary GPIO cannot represent contradictory mode and brightness");
    result=ugreenctl_led_gpio_read(context,id,&before,error,size);if (result<0) return result;
    on=before.brightness;channel=(before.unknown_fields&UGREENCTL_LED_PATCH_COLOR) ? 1 : selected_channel(before.color);
    if (patch->fields&UGREENCTL_LED_PATCH_COLOR) channel=selected_channel(patch->value.color);
    if (patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS) on=patch->value.brightness!=0;
    if (patch->fields&UGREENCTL_LED_PATCH_MODE) on=patch->value.mode==UGREENCTL_LED_MODE_ON;
    /* A palette has no dormant hardware register while OFF. Refuse a color-
     * only request there instead of returning an invented cached color. */
    if ((patch->fields&UGREENCTL_LED_PATCH_COLOR) && !on)
        return fail(error,size,-ENOTSUP,"GPIO palette selection requires the LED to be on; request --mode on together");
    entry=&context->banks[context->bank_for_led[index]];offset=context->index_for_led[index];
    config.num_attrs=2;
    config.attrs[0].attr.id=GPIO_V2_LINE_ATTR_ID_FLAGS;config.attrs[0].attr.flags=GPIO_V2_LINE_FLAG_OUTPUT;config.attrs[0].mask=3ULL<<offset;
    config.attrs[1].attr.id=GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;config.attrs[1].attr.values=on ? 1ULL<<(offset+channel-1) : 0;config.attrs[1].mask=3ULL<<offset;
    /* Other requested LEDs retain their existing configuration. The pair is
     * configured with its target values in one kernel ioctl to avoid a flash. */
    result=context->io.control(context->opaque,entry->line_fd,GPIO_V2_LINE_SET_CONFIG_IOCTL,&config);
    if (result<0) return fail(error,size,result,"GPIO output configuration failed");
    result=ugreenctl_led_gpio_read(context,id,&after,error,size);if (result<0) return result;
    if (after.brightness!=on || ((patch->fields&UGREENCTL_LED_PATCH_COLOR) &&
        ((after.unknown_fields&UGREENCTL_LED_PATCH_COLOR) || selected_channel(after.color)!=channel)))
        return fail(error,size,-EIO,"GPIO readback did not match the requested output pair");
    if (output) *output=after;return 0;
}
void ugreenctl_led_gpio_close(struct ugreenctl_led_gpio *context)
{
    unsigned int bank;if (!context) return;
    for (bank=0;bank<2;++bank) {
        if (context->banks[bank].line_fd>=0) context->io.close_fd(context->opaque,context->banks[bank].line_fd);
        if (context->banks[bank].chip_fd>=0) context->io.close_fd(context->opaque,context->banks[bank].chip_fd);
    }
    if (context->locked || context->lock_fd>=0) context->io.unlock(context->opaque);
    free(context);
}
#ifdef UGREENCTL_LED_TESTING
int ugreenctl_led_gpio_test_open(const char *root,const char *device_root,const struct ugreenctl_led_access *access,
    const struct ugreenctl_led_gpio_test_io *io,void *opaque,struct ugreenctl_led_gpio **context,char *error,size_t size)
{
    struct gpio_io transport;
    if (!io || !io->open_chip || !io->control || !io->close_fd || !io->owners || !io->lock || !io->unlock) return -EINVAL;
    transport=(struct gpio_io){io->open_chip,io->control,io->close_fd,io->owners,io->lock,io->unlock};
    if (error && size) error[0]=0;
    return open_internal(root,device_root,access,&transport,opaque,context,error,size);
}
#endif
