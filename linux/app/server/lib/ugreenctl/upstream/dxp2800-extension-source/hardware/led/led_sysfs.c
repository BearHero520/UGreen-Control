#define _XOPEN_SOURCE 700
#include "led/led_sysfs.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct ugreenctl_led_sysfs {
    const struct ugreenctl_led_profile *profile;
    char root[PATH_MAX];
    char directory[UGREENCTL_MAX_LEDS][PATH_MAX];
    int (*authorize)(void *,char *,size_t);
    void *authorize_opaque;
#ifdef UGREENCTL_LED_TESTING
    void (*observer)(void *,const char *,const char *);
    void *observer_opaque;
#endif
};

static int fail(char *error,size_t size,int result,const char *format,...)
{
    va_list arguments;
    if (error != NULL && size != 0) {
        va_start(arguments,format); (void)vsnprintf(error,size,format,arguments); va_end(arguments);
    }
    return result;
}

static int read_text(const char *path,char *value,size_t capacity)
{
    int fd = open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    ssize_t count;
    if (fd < 0) return -errno;
    count = read(fd,value,capacity-1);
    if (count < 0) { int result=-errno; (void)close(fd); return result; }
    (void)close(fd);
    if ((size_t)count >= capacity-1) return -EOVERFLOW;
    value[count]='\0';
    while (count > 0 && isspace((unsigned char)value[count-1])) value[--count]='\0';
    return 0;
}

static int read_number(const char *path,unsigned int *value)
{
    char text[64],*end;
    unsigned long number;
    int result=read_text(path,text,sizeof(text));
    if (result < 0) return result;
    if (!isdigit((unsigned char)text[0])) return -EBADMSG;
    errno=0; number=strtoul(text,&end,10);
    if (errno || *end || number>UINT_MAX) return -EBADMSG;
    *value=(unsigned int)number;
    return 0;
}

static int write_text(const char *path,const char *value)
{
    size_t length=strlen(value);
    int fd=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW|O_TRUNC);
    ssize_t count;
    if (fd<0) return -errno;
    count=write(fd,value,length);
    if (count<0) { int result=-errno;(void)close(fd);return result; }
    (void)close(fd);
    return (size_t)count==length ? 0 : -EIO;
}

static const char *base_name(const char *path)
{
    const char *slash=strrchr(path,'/');
    return slash==NULL ? path : slash+1;
}

static bool module_matches(const char *actual,const char *expected)
{
    size_t i;
    if (strlen(actual)!=strlen(expected)) return false;
    for (i=0;actual[i];++i)
        if ((actual[i]=='-'?'_':actual[i])!=(expected[i]=='-'?'_':expected[i])) return false;
    return true;
}

static int target_index(const struct ugreenctl_led_sysfs *context,uint8_t id)
{
    size_t i;
    for (i=0;i<context->profile->led_count;++i)
        if (context->profile->leds[i].id==id) return (int)i;
    return -1;
}

static int verify_target(struct ugreenctl_led_sysfs *context,size_t index,char *error,size_t size)
{
    const struct ugreenctl_led_descriptor *target=&context->profile->leds[index];
    char link[PATH_MAX],resolved[PATH_MAX],path[PATH_MAX],binding[PATH_MAX];
    unsigned int maximum;
    if (target->sysfs_name==NULL || strchr(target->sysfs_name,'/'))
        return fail(error,size,-EINVAL,"invalid LED sysfs descriptor");
    if (snprintf(link,sizeof(link),"%s/class/leds/%s",context->root,target->sysfs_name)>=(int)sizeof(link) ||
        realpath(link,resolved)==NULL)
        return fail(error,size,-ENOTSUP,"expected LED class node %s is absent; the required hardware driver is unavailable",target->sysfs_name);
    if (strncmp(resolved,context->root,strlen(context->root))!=0 ||
        strncmp(resolved+strlen(context->root),"/devices/",9)!=0)
        return fail(error,size,-EPERM,"LED class node escaped the real sysfs device tree");
    if (context->directory[index][0] && strcmp(context->directory[index],resolved))
        return fail(error,size,-EBUSY,"LED device binding changed during the operation");
    (void)snprintf(path,sizeof(path),"%s/device/driver/module",resolved);
    if (realpath(path,binding)!=NULL) {
        char prefix[PATH_MAX];
        (void)snprintf(prefix,sizeof(prefix),"%s/module/",context->root);
        if (strncmp(binding,prefix,strlen(prefix)) ||
            !module_matches(base_name(binding),context->profile->sysfs_driver))
            return fail(error,size,-ENOTSUP,"LED %s has an unexpected driver module binding",target->name);
    } else {
        int binding_error=errno;
        /* Built-in GPIO drivers have no module link. Their OF-matched driver
         * name still identifies the documented platform binding. */
        (void)snprintf(path,sizeof(path),"%s/device/driver",resolved);
        if (binding_error!=ENOENT || strcmp(context->profile->protocol,"sysfs-gpio")!=0 || realpath(path,binding)==NULL ||
            !module_matches(base_name(binding),context->profile->sysfs_driver))
            return fail(error,size,-ENOTSUP,"LED %s is not bound to expected driver %s",target->name,context->profile->sysfs_driver);
    }
    (void)snprintf(path,sizeof(path),"%s/max_brightness",resolved);
    if (read_number(path,&maximum)<0 ||
        (strcmp(context->profile->protocol,"sysfs-gpio")==0 ?
         (maximum!=1 && maximum!=255) :
         (!strcmp(context->profile->product_name,"DH2600") ? maximum!=254 : maximum!=target->max_brightness)))
        return fail(error,size,-ENOTSUP,"LED %s brightness range does not match the verified driver",target->name);
    (void)snprintf(context->directory[index],sizeof(context->directory[index]),"%s",resolved);
    return 0;
}

static int attribute(struct ugreenctl_led_sysfs *context,size_t index,const char *name,
                      const char *text,char *error,size_t size)
{
    char path[PATH_MAX];
    int result=context->authorize(context->authorize_opaque,error,size);
    if (result<0) return result;
    result=verify_target(context,index,error,size);
    if (result<0) return result;
    if (snprintf(path,sizeof(path),"%s/%s",context->directory[index],name)>=(int)sizeof(path))
        return fail(error,size,-ENAMETOOLONG,"LED attribute path is too long");
    result=write_text(path,text);
    if (result<0) return fail(error,size,result,"LED %s attribute %s write failed: %s",context->profile->leds[index].name,name,strerror(-result));
#ifdef UGREENCTL_LED_TESTING
    if (context->observer) context->observer(context->observer_opaque,path,text);
#endif
    return 0;
}

static int number_attribute(struct ugreenctl_led_sysfs *context,size_t index,const char *name,
                             unsigned int value,char *error,size_t size)
{
    char text[32];
    (void)snprintf(text,sizeof(text),"%u",value);
    return attribute(context,index,name,text,error,size);
}

static int wait_configured_brightness(struct ugreenctl_led_sysfs *context,size_t index,
                                      unsigned int expected,char *error,size_t size)
{
    char path[PATH_MAX];unsigned int attempt,actual;
    const struct timespec interval={0,5000000L};
    if (strcmp(context->profile->protocol,"sysfs-n76") || context->profile->leds[index].max_brightness==1) return 0;
    (void)snprintf(path,sizeof(path),"%s/rbrightness",context->directory[index]);
    for (attempt=0;attempt<21;++attempt) {
        int result=read_number(path,&actual);
        if (result<0) return fail(error,size,result,"N76 configured-brightness cache read failed");
        if (actual==expected) return 0;
        if (attempt!=20) (void)nanosleep(&interval,NULL);
    }
    return fail(error,size,-EIO,"N76 driver did not update its configured-brightness cache");
}

int ugreenctl_led_sysfs_open(const struct ugreenctl_led_profile *profile,
                            const char *root,
                            int (*authorize)(void *,char *,size_t),void *opaque,
                            struct ugreenctl_led_sysfs **output,
                            char *error,size_t size)
{
    struct ugreenctl_led_sysfs *context;
    size_t i;
    int result;
    if (!profile || !root || !authorize || !output || !profile->sysfs_driver ||
        profile->led_count==0 || profile->led_count>UGREENCTL_MAX_LEDS) return -EINVAL;
    *output=NULL;
    context=calloc(1,sizeof(*context));
    if (!context) return -ENOMEM;
    context->profile=profile;
    context->authorize=authorize;context->authorize_opaque=opaque;
    if (!realpath(root,context->root)) { result=-errno;free(context);return result; }
    for (i=0;i<profile->led_count;++i) {
        result=verify_target(context,i,error,size);
        if (result<0) { free(context);return result; }
    }
    *output=context;
    return 0;
}

bool ugreenctl_led_sysfs_identity_confirmed(struct ugreenctl_led_sysfs *context)
{
    size_t i;
    if (!context) return false;
    for (i=0;i<context->profile->led_count;++i)
        if (verify_target(context,i,NULL,0)<0) return false;
    return true;
}

int ugreenctl_led_sysfs_read(struct ugreenctl_led_sysfs *context,uint8_t id,
                            struct ugreenctl_led_value *value,char *error,size_t size)
{
    char path[PATH_MAX],trigger[1024],color[128],*active,*end;
    int index,result;
    unsigned int number,core_brightness;
    if (!context || !value || (index=target_index(context,id))<0) return -EINVAL;
    result=verify_target(context,(size_t)index,error,size);
    if (result<0) return result;
    memset(value,0,sizeof(*value));
    value->unknown_fields=UGREENCTL_LED_PATCH_RGB|UGREENCTL_LED_PATCH_COLOR|UGREENCTL_LED_PATCH_TIMING;
    value->cached_fields=UGREENCTL_LED_PATCH_MODE|UGREENCTL_LED_PATCH_BRIGHTNESS;
    (void)snprintf(path,sizeof(path),"%s/brightness",context->directory[index]);
    result=read_number(path,&value->brightness);
    if (result<0) return fail(error,size,result,"LED brightness read failed");
    if (context->profile->leds[index].max_brightness==1) value->brightness=value->brightness!=0;
    core_brightness=value->brightness;
    if (!strcmp(context->profile->protocol,"sysfs-n76") && context->profile->leds[index].max_brightness>1) {
        (void)snprintf(path,sizeof(path),"%s/rbrightness",context->directory[index]);
        result=read_number(path,&value->brightness);
        if (result<0 || value->brightness>255)
            return fail(error,size,result<0 ? result : -EBADMSG,"N76 configured-brightness cache is unavailable");
    }
    (void)snprintf(path,sizeof(path),"%s/trigger",context->directory[index]);
    result=read_text(path,trigger,sizeof(trigger));
    if (result<0) return fail(error,size,result,"LED trigger read failed");
    active=strchr(trigger,'[');
    if (active) { ++active;end=strchr(active,']');if (end) *end='\0';else return -EBADMSG; }
    else active=trigger; /* fixtures and kernels exposing only active value */
    if (!strcmp(active,"none") || !strcmp(active,"normal"))
        value->mode=core_brightness ? UGREENCTL_LED_MODE_ON : UGREENCTL_LED_MODE_OFF;
    else if (!strcmp(active,"timer") || !strcmp(active,"timer2")) value->mode=UGREENCTL_LED_MODE_BLINK;
    else if (!strcmp(active,"breath")) value->mode=UGREENCTL_LED_MODE_BREATH;
    else value->unknown_fields|=UGREENCTL_LED_PATCH_MODE;
    (void)snprintf(path,sizeof(path),"%s/%s",context->directory[index],
                   !strcmp(context->profile->protocol,"sysfs-gpio") ? "state" : "color");
    if (read_text(path,color,sizeof(color))==0) {
        bool known=true;
        if (!strcmp(context->profile->protocol,"sysfs-gpio")) {
            const char *field=strstr(color,",color:");
            if (!field || !isdigit((unsigned char)field[7])) known=false;
            else {
                char *tail;
                unsigned long selected=strtoul(field+7,&tail,10);
                if (*tail!=',' || selected<1 || selected>5) known=false;
                else value->color=selected==1 ? UGREENCTL_LED_COLOR_WHITE :
                    (!strcmp(context->profile->product_name,"DH2300") ? UGREENCTL_LED_COLOR_ORANGE : UGREENCTL_LED_COLOR_RED);
            }
        }
        else if (!strcmp(color,"white")) value->color=UGREENCTL_LED_COLOR_WHITE;
        else if (!strcmp(color,"orange")) value->color=UGREENCTL_LED_COLOR_ORANGE;
        else if (!strcmp(color,"white + orange")) value->color=UGREENCTL_LED_COLOR_WHITE_ORANGE;
        else if (!strcmp(color,"None") || !strcmp(color,"none")) value->color=UGREENCTL_LED_COLOR_NONE;
        else if (read_number(path,&number)==0 && number<=3) value->color=(enum ugreenctl_led_color)number;
        else known=false;
        if (known) { value->unknown_fields&=~UGREENCTL_LED_PATCH_COLOR;value->cached_fields|=UGREENCTL_LED_PATCH_COLOR; }
    }
    (void)snprintf(path,sizeof(path),"%s/delay_on",context->directory[index]);
    if (read_number(path,&value->on_ms)==0) {
        (void)snprintf(path,sizeof(path),"%s/delay_off",context->directory[index]);
        if (read_number(path,&value->off_ms)==0) {
            value->unknown_fields&=~UGREENCTL_LED_PATCH_TIMING;
            value->cached_fields|=UGREENCTL_LED_PATCH_TIMING;
        }
    }
    return 0;
}

static bool trigger_available(const char *list,const char *required)
{
    const char *start,*end;
    size_t length=strlen(required);
    while (*list) {
        while (isspace((unsigned char)*list)) ++list;
        start=list;
        while (*list && !isspace((unsigned char)*list)) ++list;
        end=list;
        if (end-start>=2 && *start=='[' && end[-1]==']') { ++start;--end; }
        if ((size_t)(end-start)==length && !memcmp(start,required,length)) return true;
    }
    return false;
}

int ugreenctl_led_sysfs_preflight(struct ugreenctl_led_sysfs *context,uint8_t id,
    const struct ugreenctl_led_value *before,const struct ugreenctl_led_patch *patch,
    char *error,size_t size)
{
    char path[PATH_MAX],triggers[1024];
    const char *required;
    enum ugreenctl_led_mode mode;
    int index,result;
    if (!context || (index=target_index(context,id))<0) return -EINVAL;
    result=ugreenctl_led_validate_state_patch(context->profile,&context->profile->leds[index],before,patch,error,size);
    if (result<0) return result;
    result=verify_target(context,(size_t)index,error,size);
    if (result<0 || strcmp(context->profile->protocol,"sysfs-n76")) return result;
    mode=(patch->fields&UGREENCTL_LED_PATCH_MODE) ? patch->value.mode : before->mode;
    required=mode<=UGREENCTL_LED_MODE_ON ? "normal" : mode==UGREENCTL_LED_MODE_BLINK ? "timer" : "breath";
    (void)snprintf(path,sizeof(path),"%s/trigger",context->directory[index]);
    result=read_text(path,triggers,sizeof(triggers));
    if (result<0) return fail(error,size,result,"N76 available-trigger list is unreadable");
    if (!trigger_available(triggers,required))
        return fail(error,size,-ENOTSUP,"N76 requested mode requires the registered exact '%s' trigger",required);
    return 0;
}

int ugreenctl_led_sysfs_apply(struct ugreenctl_led_sysfs *context,uint8_t id,
                             const struct ugreenctl_led_patch *patch,
                             struct ugreenctl_led_value *output,char *error,size_t size)
{
    struct ugreenctl_led_value before,after;
    struct ugreenctl_led_patch effective;
    int index,result;
    if (!context || (index=target_index(context,id))<0) return -EINVAL;
    result=ugreenctl_led_validate_target_patch(&context->profile->leds[index],patch,error,size);
    if (result<0) return result;
    if (!strcmp(context->profile->protocol,"sysfs-gpio") &&
        (patch->fields&UGREENCTL_LED_PATCH_MODE) && (patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS) &&
        ((patch->value.mode==UGREENCTL_LED_MODE_ON && patch->value.brightness==0) ||
         (patch->value.mode==UGREENCTL_LED_MODE_OFF && patch->value.brightness!=0)))
        return fail(error,size,-ENOTSUP,"sysfs cannot represent the requested on/off mode and brightness together");
    result=ugreenctl_led_sysfs_read(context,id,&before,error,size);
    if (result<0) return result;
    result=ugreenctl_led_sysfs_preflight(context,id,&before,patch,error,size);
    if (result<0) return result;
    if (!strcmp(context->profile->protocol,"sysfs-n76")) {
        if (!(patch->fields&UGREENCTL_LED_PATCH_MODE)) {
            effective=*patch;effective.fields|=UGREENCTL_LED_PATCH_MODE;effective.value.mode=before.mode;patch=&effective;
        }
    }
    if (patch->fields&UGREENCTL_LED_PATCH_COLOR) {
        unsigned int color=(unsigned int)patch->value.color;
        if (!strcmp(context->profile->protocol,"sysfs-gpio"))
            color=patch->value.color==UGREENCTL_LED_COLOR_CHANNEL1 || patch->value.color==UGREENCTL_LED_COLOR_WHITE ? 1 : 2;
        result=number_attribute(context,(size_t)index,"color",color,error,size);
        if (result<0) return result;
    }
    if (patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS) {
        result=number_attribute(context,(size_t)index,"brightness",patch->value.brightness,error,size);
        if (result==0) result=wait_configured_brightness(context,(size_t)index,patch->value.brightness,error,size);
        if (result<0) return result;
    }
    if (patch->fields&UGREENCTL_LED_PATCH_MODE) {
        const char *trigger="none";
        if (patch->value.mode==UGREENCTL_LED_MODE_BLINK) trigger="timer";
        else if (patch->value.mode==UGREENCTL_LED_MODE_BREATH) trigger="breath";
        else if (!strcmp(context->profile->protocol,"sysfs-n76")) trigger="normal";
        if (!strcmp(context->profile->protocol,"sysfs-n76")) {
            enum ugreenctl_led_color selected=(patch->fields&UGREENCTL_LED_PATCH_COLOR) ? patch->value.color : before.color;
            /* A normal activation with color=3 is ignored by this driver.
             * Activate on white to reset both channel mode registers, then
             * restore the desired combined/empty palette before on/off. */
            if (!strcmp(trigger,"normal") && selected!=UGREENCTL_LED_COLOR_WHITE && selected!=UGREENCTL_LED_COLOR_ORANGE) {
                result=number_attribute(context,(size_t)index,"color",1,error,size);if (result<0) return result;
            }
            result=attribute(context,(size_t)index,"trigger","none",error,size);if (result<0) return result;
            result=attribute(context,(size_t)index,"trigger",trigger,error,size);if (result<0) return result;
            if (!strcmp(trigger,"normal") && selected!=UGREENCTL_LED_COLOR_WHITE && selected!=UGREENCTL_LED_COLOR_ORANGE) {
                result=number_attribute(context,(size_t)index,"color",(unsigned int)selected,error,size);if (result<0) return result;
            }
        } else
        result=attribute(context,(size_t)index,"trigger",trigger,error,size);
        if (result<0) return result;
        if (patch->value.mode==UGREENCTL_LED_MODE_OFF || patch->value.mode==UGREENCTL_LED_MODE_ON) {
            unsigned int brightness=patch->value.mode==UGREENCTL_LED_MODE_OFF ? 0 :
                ((patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS) ? patch->value.brightness :
                 (before.brightness ? before.brightness : context->profile->leds[index].max_brightness));
            /* N76's value 1 means ON without rewriting configured PWM.
             * In particular it preserves the stock initial cache 255 even
             * though this driver's writable class maximum is only 254. */
            if (!strcmp(context->profile->protocol,"sysfs-n76") && patch->value.mode==UGREENCTL_LED_MODE_ON) brightness=1;
            result=number_attribute(context,(size_t)index,"brightness",brightness,error,size);
        } else {
            result=number_attribute(context,(size_t)index,"delay_on",patch->value.on_ms,error,size);
            if (result==0) result=number_attribute(context,(size_t)index,"delay_off",patch->value.off_ms,error,size);
        }
        if (result<0) return result;
    }
    /* N76's workqueue updates rbrightness after the sysfs store returns.
     * Wait boundedly for the public cache, never call that hardware readback. */
    {
        unsigned int attempt;
        for (attempt=0;attempt<21;++attempt) {
            const struct timespec interval={0,5000000L};
            result=ugreenctl_led_sysfs_read(context,id,&after,error,size);
            if (result<0 || !(patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS) ||
                after.brightness==patch->value.brightness || attempt==20) break;
            (void)nanosleep(&interval,NULL);
        }
    }
    if (result<0) return result;
    if ((after.unknown_fields&patch->fields) ||
        ((patch->fields&UGREENCTL_LED_PATCH_COLOR) && after.color!=patch->value.color &&
         !(patch->value.color==UGREENCTL_LED_COLOR_CHANNEL1 && after.color==UGREENCTL_LED_COLOR_WHITE) &&
         !(patch->value.color==UGREENCTL_LED_COLOR_CHANNEL2 &&
           (after.color==UGREENCTL_LED_COLOR_ORANGE || after.color==UGREENCTL_LED_COLOR_RED))) ||
        ((patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS) && after.brightness!=patch->value.brightness) ||
        ((patch->fields&UGREENCTL_LED_PATCH_MODE) && after.mode!=patch->value.mode) ||
        ((patch->fields&UGREENCTL_LED_PATCH_TIMING) &&
         (after.on_ms!=patch->value.on_ms || after.off_ms!=patch->value.off_ms)))
        return fail(error,size,-EIO,"LED sysfs readback did not match requested fields; verification is at the driver's public attribute layer");
    if (output) *output=after;
    return 0;
}

void ugreenctl_led_sysfs_close(struct ugreenctl_led_sysfs *context) { free(context); }

int ugreenctl_led_sysfs_identify(struct ugreenctl_led_sysfs *context,uint8_t id,unsigned int seconds,
    const volatile sig_atomic_t *cancelled,int (*authorize)(void *,char *,size_t),
    void (*pause_ms)(void *,unsigned int),void *opaque,char *error,size_t size)
{
    struct ugreenctl_led_value saved,current;
    struct ugreenctl_led_patch patch={0};
    char path[PATH_MAX],triggers[1024],original_trigger[64],*active,*end,restore_error[256]="";
    unsigned int brightness,color,tick;int index,result=0,restored;
    if (!context || !authorize || !pause_ms || seconds<1 || seconds>60 || (index=target_index(context,id))<0) return -EINVAL;
    result=ugreenctl_led_sysfs_read(context,id,&saved,error,size);if (result<0) return result;
    if (saved.unknown_fields&(UGREENCTL_LED_PATCH_MODE|UGREENCTL_LED_PATCH_COLOR|UGREENCTL_LED_PATCH_BRIGHTNESS))
        return fail(error,size,-ENOTSUP,"identify requires readable original mode, palette and brightness for restoration");
    if (!strcmp(context->profile->protocol,"sysfs-n76") && context->profile->leds[index].max_brightness>1 &&
        (saved.brightness<2 || saved.brightness>context->profile->leds[index].max_brightness))
        return fail(error,size,-ENOTSUP,"identify cannot restore an uninitialized or out-of-writable-range N76 intensity cache");
    (void)snprintf(path,sizeof(path),"%s/trigger",context->directory[index]);
    result=read_text(path,triggers,sizeof(triggers));if (result<0) return result;
    active=strchr(triggers,'[');active=active ? active+1 : triggers;end=strchr(active,']');if (end) *end=0;
    if (strcmp(active,"none") && strcmp(active,"normal") && strcmp(active,"timer") && strcmp(active,"timer2") && strcmp(active,"breath"))
        return fail(error,size,-ENOTSUP,"identify cannot restore the active trigger's private configuration");
    if (strlen(active)>=sizeof(original_trigger)) return -EBADMSG;
    (void)snprintf(original_trigger,sizeof(original_trigger),"%s",active);
    if (saved.mode==UGREENCTL_LED_MODE_BLINK && (saved.unknown_fields&UGREENCTL_LED_PATCH_TIMING))
        return fail(error,size,-ENOTSUP,"identify cannot restore unreadable active blink timings");
    (void)snprintf(path,sizeof(path),"%s/brightness",context->directory[index]);
    result=read_number(path,&brightness);if (result<0) return result;
    color=(unsigned int)saved.color;
    if (!strcmp(context->profile->protocol,"sysfs-gpio")) {
        char state[256],*field,*tail;unsigned long number;
        (void)snprintf(path,sizeof(path),"%s/state",context->directory[index]);
        if (read_text(path,state,sizeof(state))<0 || !(field=strstr(state,",color:"))) return -EBADMSG;
        number=strtoul(field+7,&tail,10);if (*tail!=',' || number<1 || number>5) return -EBADMSG;color=(unsigned int)number;
    }
    patch.fields=UGREENCTL_LED_PATCH_MODE|UGREENCTL_LED_PATCH_COLOR|UGREENCTL_LED_PATCH_BRIGHTNESS;
    patch.value.mode=UGREENCTL_LED_MODE_ON;patch.value.color=UGREENCTL_LED_COLOR_WHITE;
    patch.value.brightness=context->profile->leds[index].max_brightness;
    result=ugreenctl_led_sysfs_preflight(context,id,&saved,&patch,error,size);
    if (result<0) return result;
    /* Software flashes work on real binary GPIO hardware and do not pretend
     * that N76's fixed private breath trigger accepts arbitrary durations. */
    for (tick=0;tick<seconds*4U;++tick) {
        result=authorize(opaque,error,size);if (result<0) break;
        patch.fields=UGREENCTL_LED_PATCH_MODE;
        patch.value.mode=(tick&1U) ? UGREENCTL_LED_MODE_OFF : UGREENCTL_LED_MODE_ON;
        if (!(tick&1U)) {
            patch.fields|=UGREENCTL_LED_PATCH_COLOR|UGREENCTL_LED_PATCH_BRIGHTNESS;
            patch.value.color=UGREENCTL_LED_COLOR_WHITE;
            patch.value.brightness=context->profile->leds[index].max_brightness;
        }
        result=ugreenctl_led_sysfs_apply(context,id,&patch,&current,error,size);if (result<0) break;
        if (cancelled && *cancelled) break;
        pause_ms(opaque,250);
    }
    restored=authorize(opaque,restore_error,sizeof(restore_error));
    if (restored==0) restored=attribute(context,(size_t)index,"trigger","none",restore_error,sizeof(restore_error));
    if (restored==0) restored=number_attribute(context,(size_t)index,"color",color,restore_error,sizeof(restore_error));
    if (restored==0 && !strcmp(context->profile->protocol,"sysfs-n76") && context->profile->leds[index].max_brightness>1 && saved.brightness>=2)
        restored=number_attribute(context,(size_t)index,"brightness",saved.brightness,restore_error,sizeof(restore_error));
    if (restored==0 && !strcmp(context->profile->protocol,"sysfs-n76"))
        restored=wait_configured_brightness(context,(size_t)index,saved.brightness,restore_error,sizeof(restore_error));
    if (restored==0) restored=number_attribute(context,(size_t)index,"brightness",brightness,restore_error,sizeof(restore_error));
    if (restored==0) restored=attribute(context,(size_t)index,"trigger",original_trigger,restore_error,sizeof(restore_error));
    if (restored==0 && saved.mode==UGREENCTL_LED_MODE_BLINK) {
        restored=number_attribute(context,(size_t)index,"delay_on",saved.on_ms,restore_error,sizeof(restore_error));
        if (restored==0) restored=number_attribute(context,(size_t)index,"delay_off",saved.off_ms,restore_error,sizeof(restore_error));
    }
    if (restored==0) restored=ugreenctl_led_sysfs_read(context,id,&current,restore_error,sizeof(restore_error));
    if (restored<0) return fail(error,size,restored,"identify restoration failed: %s",restore_error);
    if ((current.unknown_fields&(UGREENCTL_LED_PATCH_MODE|UGREENCTL_LED_PATCH_COLOR|UGREENCTL_LED_PATCH_BRIGHTNESS)) ||
        (saved.mode==UGREENCTL_LED_MODE_BLINK && (current.unknown_fields&UGREENCTL_LED_PATCH_TIMING)))
        return fail(error,size,-EIO,"identify driver-state restoration could not verify the saved fields");
    if (current.mode!=saved.mode || current.brightness!=saved.brightness || current.color!=saved.color ||
        (saved.mode==UGREENCTL_LED_MODE_BLINK && (current.on_ms!=saved.on_ms || current.off_ms!=saved.off_ms)))
        return fail(error,size,-EIO,"identify driver-state restoration did not match the saved fields");
    return result;
}
#ifdef UGREENCTL_LED_TESTING
void ugreenctl_led_sysfs_test_observer(struct ugreenctl_led_sysfs *context,
    void (*observer)(void *,const char *,const char *),void *opaque)
{
    context->observer=observer;context->observer_opaque=opaque;
}
#endif
