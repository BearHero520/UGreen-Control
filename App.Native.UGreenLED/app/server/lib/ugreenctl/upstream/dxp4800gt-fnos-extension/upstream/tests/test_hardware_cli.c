#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/hardware_cli.h"
#include "core/identity.h"
#include "disk/disk.h"
#include "led/led.h"

/* In-process fakes verify parser preflight and execution ordering without
 * permissions, DMI overrides, a controller, or actual disk/LED access. */
struct ugreenctl_led_session { int unused; };
static struct ugreenctl_led_session fake_session;
static const struct ugreenctl_led_profile profile={
    .id="fixture", .product_name="DXP4800", .protocol="legacy-i2c-block",
    .disk_count=2, .experimental=true, .evidence="test only"
};
static const struct ugreenctl_led_profile n76_profile={
    .id="fixture-n76", .product_name="DH2600", .protocol="sysfs-n76",
    .disk_count=2, .experimental=true, .evidence="test only"
};
static const struct ugreenctl_led_descriptor direct_leds[]={
    {.name="power",.id=0,.mode_mask=7,.max_brightness=1,.color_mask=14,.min_on_ms=20,.min_off_ms=20}
};
static const struct ugreenctl_led_profile direct_profile={
    .id="dh2600-n76",.product_name="DH2600",.protocol="sysfs-n76",.experimental=true,
    .leds=direct_leds,.led_count=1
};
static bool direct_fixture, direct_open;
static unsigned int detects, opens, reads, writes, identifies, closes, scans;
static unsigned int preflights;
static int open_error, failed_read_id=-1;
static int failed_write_id=-1;
static unsigned int fixture_max_brightness;
static enum ugreenctl_led_mode fixture_status_mode;
static bool mapped=true;
static bool palette, off_state, no_led, no_identify, confirmed=true;
static bool n76, n76_last_effect, n76_last_combined, n76_last_unknown_mode;
static bool namespace_fixture;
static bool normal_available=true, timer_available=true;
static unsigned int unknown_fields, cached_fields;
static const char *expected_map_file;
static unsigned int seen_fields[128], seen_values[128], seen_ids[128], seen_brightness[128];

static void expect(bool condition, const char *message)
{
    if (!condition) { (void)fprintf(stderr,"FAIL: %s\n",message); exit(EXIT_FAILURE); }
}

static void reset(void)
{
    detects=opens=reads=writes=identifies=closes=scans=0;
    direct_fixture=direct_open=false;
    preflights=0; normal_available=timer_available=true;
    open_error=0; failed_read_id=-1; mapped=true; expected_map_file=NULL;
    failed_write_id=-1; fixture_max_brightness=0; fixture_status_mode=UGREENCTL_LED_MODE_OFF;
    n76=n76_last_effect=n76_last_combined=n76_last_unknown_mode=false;
    namespace_fixture=false;
    palette=false; off_state=false; no_led=false; no_identify=false; confirmed=true; unknown_fields=cached_fields=0;
    (void)memset(seen_fields,0,sizeof(seen_fields));
    (void)memset(seen_values,0,sizeof(seen_values));
    (void)memset(seen_ids,0,sizeof(seen_ids));
    (void)memset(seen_brightness,0,sizeof(seen_brightness));
}

int ugreenctl_led_detect(struct ugreenctl_led_identity *identity,
                        char *error, size_t size)
{
    (void)error; (void)size; ++detects;
    (void)memset(identity,0,sizeof(*identity));
    (void)snprintf(identity->product_name,sizeof(identity->product_name),"DXP4800");
    identity->profile=direct_fixture ? &direct_profile : n76 ? &n76_profile : &profile; return 0;
}

size_t ugreenctl_led_descriptors(const struct ugreenctl_led_profile *p,
                               struct ugreenctl_led_descriptor *out, size_t capacity)
{
    static const struct ugreenctl_led_descriptor values[]={
        {.name="power",.id=0,.rgb=true,.mode_mask=15,.max_brightness=255},
        {.name="netdev",.id=1,.rgb=true,.mode_mask=15,.max_brightness=255},
        {.name="disk1",.id=2,.bay=1,.rgb=true,.mode_mask=15,.max_brightness=255},
        {.name="disk2",.id=3,.bay=2,.rgb=true,.mode_mask=15,.max_brightness=255}
    };
    (void)p;
    expect(capacity>=4,"descriptor capacity");
    (void)memcpy(out,values,sizeof(values));
    if (fixture_max_brightness) for (size_t i=0;i<4;++i) out[i].max_brightness=fixture_max_brightness;
    if (n76) for (size_t i=0;i<4;++i) {
        out[i].rgb=false; out[i].max_brightness=1; out[i].mode_mask=7;
        out[i].color_mask=UGREENCTL_LED_COLOR_BIT(UGREENCTL_LED_COLOR_WHITE)|
            UGREENCTL_LED_COLOR_BIT(UGREENCTL_LED_COLOR_ORANGE)|
            UGREENCTL_LED_COLOR_BIT(UGREENCTL_LED_COLOR_WHITE_ORANGE);
    }
    if (palette) {
        out[3].rgb=false; out[3].max_brightness=off_state ? 1 : 2;
        out[3].min_on_ms=out[3].min_off_ms=20;
        out[3].color_mask=UGREENCTL_LED_COLOR_BIT(UGREENCTL_LED_COLOR_WHITE);
    }
    if (no_identify) out[2].mode_mask=UGREENCTL_LED_MODE_BIT(UGREENCTL_LED_MODE_OFF);
    return 4;
}

int ugreenctl_led_open(const struct ugreenctl_led_access *access,
                      struct ugreenctl_led_session **session,
                      char *error, size_t size)
{
    direct_open=access->n76_direct;
    if (direct_open) expect(access->n76_bus==1 && access->n76_address==0x31,"direct selector reaches backend");
    ++opens;
    if (open_error) {
        (void)snprintf(error,size,"fixture owner conflict"); return open_error;
    }
    *session=&fake_session; return 0;
}

int ugreenctl_led_read(struct ugreenctl_led_session *session, uint8_t id,
                      struct ugreenctl_led_value *value, char *error, size_t size)
{
    expect(session==&fake_session,"valid read session"); ++reads;
    if (id==failed_read_id) {
        (void)snprintf(error,size,"fixture read failure"); return -EIO;
    }
    (void)memset(value,0,sizeof(*value));
    value->mode=UGREENCTL_LED_MODE_ON; value->brightness=100; value->blue=255;
    if (fixture_status_mode) { value->mode=fixture_status_mode; value->on_ms=200; value->off_ms=800; }
    value->unknown_fields=unknown_fields; value->cached_fields=cached_fields;
    if (direct_open) { value->unknown_fields=31;value->n76_mode_valid=true;value->n76_version=7; }

    if (n76) {
        value->color=UGREENCTL_LED_COLOR_WHITE;
        if (id==3 && n76_last_effect) value->mode=UGREENCTL_LED_MODE_BLINK;
        if (id==3 && n76_last_combined) value->color=UGREENCTL_LED_COLOR_WHITE_ORANGE;
        if (id==3 && n76_last_unknown_mode) {
            value->mode=UGREENCTL_LED_MODE_OFF;
            value->unknown_fields|=UGREENCTL_LED_PATCH_MODE;
        }
    }
    if (id==3 && off_state) {
        value->mode=UGREENCTL_LED_MODE_OFF; value->brightness=0;
        value->unknown_fields|=UGREENCTL_LED_PATCH_COLOR;
    }
    return 0;
}

int ugreenctl_led_preflight(struct ugreenctl_led_session *session, uint8_t id,
                           const struct ugreenctl_led_value *before,
                           const struct ugreenctl_led_patch *patch,
                           char *error, size_t size)
{
    struct ugreenctl_led_descriptor targets[UGREENCTL_MAX_LEDS];
    const struct ugreenctl_led_profile *current=n76 ? &n76_profile : &profile;
    size_t count=ugreenctl_led_descriptors(current,targets,UGREENCTL_MAX_LEDS),i;
    enum ugreenctl_led_mode mode=(patch->fields&UGREENCTL_LED_PATCH_MODE) ? patch->value.mode : before->mode;
    int result;
    expect(session==&fake_session,"valid preflight session"); ++preflights;
    for (i=0;i<count && targets[i].id!=id;++i) { }
    expect(i<count,"preflight modeled target");
    if (direct_open) return 0; /* Actual direct preflight is covered by test-led. */
    result=ugreenctl_led_validate_state_patch(current,&targets[i],before,patch,error,size);
    if (result) return result;
    if (n76 && ((mode<=UGREENCTL_LED_MODE_ON && !normal_available) ||
                (mode==UGREENCTL_LED_MODE_BLINK && !timer_available))) {
        (void)snprintf(error,size,"fixture required backend trigger is unavailable"); return -ENOTSUP;
    }
    return 0;
}

int ugreenctl_led_apply(struct ugreenctl_led_session *session, uint8_t id,
                       const struct ugreenctl_led_patch *patch,
                       struct ugreenctl_led_value *readback, char *error, size_t size)
{
    expect(session==&fake_session,"valid apply session");
    if (id==failed_write_id) { (void)snprintf(error,size,"fixture write failure"); return -EIO; }
    expect(writes<128,"bounded operations");
    seen_ids[writes]=id; seen_fields[writes]=patch->fields;
    seen_brightness[writes]=patch->value.brightness;
    if (patch->fields&UGREENCTL_LED_PATCH_RGB) seen_values[writes]=patch->value.red;
    else if (patch->fields&UGREENCTL_LED_PATCH_COLOR) seen_values[writes]=(unsigned int)patch->value.color;
    else if (patch->fields&UGREENCTL_LED_PATCH_BRIGHTNESS) seen_values[writes]=patch->value.brightness;
    else seen_values[writes]=(unsigned int)patch->value.mode;
    ++writes; *readback=patch->value;
    if (direct_open) { readback->unknown_fields=31;readback->cached_fields=0;readback->n76_mode_valid=true;readback->unverified_write=true; }
    return 0;
}

int ugreenctl_led_identify(struct ugreenctl_led_session *session, uint8_t id,
                          unsigned int seconds, const volatile sig_atomic_t *cancelled,
                          char *error, size_t size)
{
    (void)error; (void)size;
    expect(session==&fake_session && id==2,"identify mapped disk LED");
    expect(seconds==7 && *cancelled==0,"identify duration and cancellation");
    ++identifies; return 0;
}

void ugreenctl_led_close(struct ugreenctl_led_session *session)
{
    if (session!=NULL) { expect(session==&fake_session,"valid close session"); ++closes; }
}

uint16_t ugreenctl_led_chip_id(const struct ugreenctl_led_session *session)
{
    (void)session; return 0xc5b2;
}

bool ugreenctl_led_identity_confirmed(const struct ugreenctl_led_session *session)
{
    (void)session; return confirmed;
}

const char *ugreenctl_led_bus(const struct ugreenctl_led_session *session)
{
    (void)session; return "/dev/i2c-fixture";
}

int ugreenctl_identity_read(struct ugreenctl_machine_identity *identity, char *error, size_t size)
{
    (void)error; (void)size; memset(identity,0,sizeof(*identity));
    (void)snprintf(identity->product_name,sizeof(identity->product_name),"DXP4800"); return 0;
}

const char *ugreenctl_identity_model(const struct ugreenctl_machine_identity *identity)
{
    return identity->product_name;
}

int ugreenctl_disk_scan(const char *product, const char *map_file,
                       struct ugreenctl_disk_scan *out, char *error, size_t size)
{
    (void)error; (void)size; ++scans;
    expect(strcmp(product,"DXP4800")==0,"real DMI passed to disk scan");
    expect((expected_map_file==NULL && map_file==NULL) ||
           (expected_map_file!=NULL && map_file!=NULL && strcmp(expected_map_file,map_file)==0),
           "explicit calibration path passed through");
    (void)memset(out,0,sizeof(*out)); out->count=1; out->profile_known=true;
    (void)snprintf(out->profile,sizeof(out->profile),"fixture");
    (void)snprintf(out->disks[0].device,sizeof(out->disks[0].device),"/dev/sda");
    (void)snprintf(out->disks[0].serial,sizeof(out->disks[0].serial),"SN1");
    (void)snprintf(out->disks[0].led_id,sizeof(out->disks[0].led_id),"disk1");
    (void)snprintf(out->disks[0].slot_name,sizeof(out->disks[0].slot_name),"ata1");
    (void)snprintf(out->disks[0].interface_type,sizeof(out->disks[0].interface_type),"sata");
    out->disks[0].led_supported=!no_led;
    out->disks[0].slot=1; out->disks[0].mapped=mapped;
    if (namespace_fixture) {
        (void)snprintf(out->disks[0].name,sizeof(out->disks[0].name),"nvme9n1");
        (void)snprintf(out->disks[0].device,sizeof(out->disks[0].device),"/dev/nvme9n1");
        (void)snprintf(out->disks[0].interface_type,sizeof(out->disks[0].interface_type),"nvme");
        out->namespace_count=2;
        for (size_t i=0;i<2;++i) {
            (void)snprintf(out->namespace_aliases[i].name,sizeof(out->namespace_aliases[i].name),"nvme9n%zu",i+1);
            (void)snprintf(out->namespace_aliases[i].disk_name,sizeof(out->namespace_aliases[i].disk_name),"nvme9n1");
            (void)snprintf(out->namespace_aliases[i].wwn,sizeof(out->namespace_aliases[i].wwn),"eui.fixture%zu",i+1);
        }
    }
    return 0;
}

int ugreenctl_disk_resolve(const struct ugreenctl_disk_scan *scan,
                          const char *selector, const struct ugreenctl_disk **out,
                          char *error, size_t size)
{
    expect(strcmp(selector,"/dev/sda")==0,"disk selector passed unchanged");
    if (!mapped) { (void)snprintf(error,size,"fixture unmapped topology"); return -ENOENT; }
    *out=&scan->disks[0]; return 0;
}

static void parse_tests(void)
{
    struct ugreenctl_hardware_options o;
    char error[512];
    char *new_command[]={"ugreenctl","--force","led","set","disk1","--color","#12aBef",
        "--json","--brightness","50%","--mode","blink","--on-ms","200","--off-ms","800","--apply"};
    char *legacy[]={"ugreen_leds_cli","power","disk1","-on","-color","255","0","3",
        "-status","--apply","-brightness","12","-off","--force","--json"};
    char *plugin[]={"ugreenctl","--plugin-dir","/tmp/plugins","led","list","--json"};
    char *foreign[]={"ugreenctl","--apply","fan","set","cpu","100"};
    char *rgb[]={"ugreenctl","led","set","power","--color","1,22,255"};
    char *hex_bad[]={"ugreenctl","led","set","power","--color","#ff000z"};
    char *overflow[]={"ugreenctl","led","set","power","--mode","blink","--on-ms","65535","--off-ms","1"};
    char *zero[]={"ugreenctl","led","set","power","--mode","blink","--on-ms","0","--off-ms","0"};
    char *incomplete[]={"ugreen_leds_cli","power","-color","1","2"};
    char *late_error[]={"ugreen_leds_cli","power","-on","-brightness","256","--apply"};
    char *negative[]={"ugreenctl","led","set","power","--brightness","-1"};
    char *percent_bad[]={"ugreenctl","led","set","power","--brightness","101%"};
    char *timing_bad[]={"ugreenctl","led","set","power","--on-ms","200"};
    char *dupe_mode[]={"ugreenctl","led","set","power","--mode","on","--mode","off"};
    char *mix_all[]={"ugreenctl","led","off","all","disk1"};
    char *missing_target[]={"ugreenctl","led","set","--color","blue"};
    char *missing_value[]={"ugreenctl","led","set","power","--color"};
    char *legacy_overflow[]={"ugreen_leds_cli","power","-blink","32768","32768"};
    char *status[]={"ugreenctl","led","status"};
    char *identify_all[]={"ugreenctl","led","identify","all"};
    char *identify_zero[]={"ugreenctl","led","identify","disk1","--seconds","0"};
    char *locate[]={"ugreenctl","disk","locate","SN:with spaces","--seconds","60","--map-file","/tmp/map"};
    char *direct[]={"ugreenctl","led","status","--n76-direct","12:0x26"};
    char *bad_direct[]={"ugreenctl","led","status","--n76-direct","1:0x3a"};
    char *direct_locate[]={"ugreenctl","disk","locate","slot1","--n76-direct","1:0x31"};
    char *direct_dupe[]={"ugreenctl","led","status","--n76-direct","1:0x31","--n76-direct","1:0x26"};
    char *wrong_map[]={"ugreenctl","led","list","--map-file","/tmp/map"};
    struct { char **argv; size_t count; bool compat; } invalid[]={
        {hex_bad,sizeof(hex_bad)/sizeof(*hex_bad),false},
        {overflow,sizeof(overflow)/sizeof(*overflow),false},
        {zero,sizeof(zero)/sizeof(*zero),false},
        {incomplete,sizeof(incomplete)/sizeof(*incomplete),true},
        {late_error,sizeof(late_error)/sizeof(*late_error),true},
        {negative,sizeof(negative)/sizeof(*negative),false},
        {percent_bad,sizeof(percent_bad)/sizeof(*percent_bad),false},
        {timing_bad,sizeof(timing_bad)/sizeof(*timing_bad),false},
        {dupe_mode,sizeof(dupe_mode)/sizeof(*dupe_mode),false},
        {mix_all,sizeof(mix_all)/sizeof(*mix_all),false},
        {missing_target,sizeof(missing_target)/sizeof(*missing_target),false},
        {missing_value,sizeof(missing_value)/sizeof(*missing_value),false},
        {legacy_overflow,sizeof(legacy_overflow)/sizeof(*legacy_overflow),true},
        {identify_all,sizeof(identify_all)/sizeof(*identify_all),false},
        {identify_zero,sizeof(identify_zero)/sizeof(*identify_zero),false},
        {wrong_map,sizeof(wrong_map)/sizeof(*wrong_map),false},
        {bad_direct,sizeof(bad_direct)/sizeof(*bad_direct),false},
        {direct_locate,sizeof(direct_locate)/sizeof(*direct_locate),false},
        {direct_dupe,sizeof(direct_dupe)/sizeof(*direct_dupe),false}
    };
    size_t i;
    expect(ugreenctl_hardware_parse(5,direct,false,&o,error,sizeof(error))==0 && o.n76_direct && o.n76_bus==12 && o.n76_address==0x26,"explicit N76 selector");
    expect(ugreenctl_hardware_parse((int)(sizeof(new_command)/sizeof(*new_command)),new_command,false,&o,error,sizeof(error))==0,
           "new LED options parse anywhere");
    expect(o.apply && o.force && o.json && o.target_count==1 && o.action_count==3,"new command flags/actions");
    expect(o.actions[0].red==0x12 && o.actions[0].green==0xab && o.actions[0].blue==0xef,"mixed case hex color");
    expect(o.actions[1].brightness==50 && o.actions[1].brightness_percent &&
           o.actions[2].on_ms==200 && o.actions[2].off_ms==800,"original percentage and timings are retained for each target");
    expect(ugreenctl_hardware_parse((int)(sizeof(legacy)/sizeof(*legacy)),legacy,true,&o,error,sizeof(error))==0,"legacy sequence parses");
    expect(o.action_count==5 && o.actions[0].kind==UGREENCTL_CLI_ON && o.actions[1].kind==UGREENCTL_CLI_COLOR &&
           o.actions[2].kind==UGREENCTL_CLI_STATUS && o.actions[3].kind==UGREENCTL_CLI_BRIGHTNESS &&
           o.actions[4].kind==UGREENCTL_CLI_OFF,"legacy action order preserved including status");
    expect(ugreenctl_hardware_parse(6,plugin,false,&o,error,sizeof(error))==0,"existing wrapper plugin directory accepted");
    expect(ugreenctl_hardware_parse(6,foreign,false,&o,error,sizeof(error))==-1,"old commands remain handled by main");
    expect(ugreenctl_hardware_parse(6,rgb,false,&o,error,sizeof(error))==0 && o.actions[0].green==22,"comma RGB parse");
    expect(ugreenctl_hardware_parse(3,status,false,&o,error,sizeof(error))==0 && strcmp(o.targets[0],"all")==0,"status defaults to all");
    expect(ugreenctl_hardware_parse(8,locate,false,&o,error,sizeof(error))==0 && o.seconds==60 &&
           strcmp(o.targets[0],"SN:with spaces")==0 && strcmp(o.map_file,"/tmp/map")==0,"disk selector is opaque and calibration explicit");
    for (i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        expect(ugreenctl_hardware_parse((int)invalid[i].count,invalid[i].argv,invalid[i].compat,&o,error,sizeof(error))==-2,
               "invalid syntax rejected before execution");
        expect(error[0]!=0,"invalid syntax has diagnostic");
    }
}

static int capture_cli(int argc, char **argv, bool compatibility, char *buffer, size_t size)
{
    FILE *capture=tmpfile();
    int saved, result;
    size_t length;
    expect(capture!=NULL,"create stdout capture");
    expect(fflush(stdout)==0,"flush before stdout capture");
    saved=dup(STDOUT_FILENO); expect(saved>=0,"save stdout");
    expect(dup2(fileno(capture),STDOUT_FILENO)>=0,"redirect stdout");
    result=compatibility ? ugreenctl_led_compat_cli(argc,argv) : ugreenctl_hardware_cli(argc,argv);
    expect(fflush(stdout)==0,"flush captured stdout");
    expect(fseek(capture,0,SEEK_SET)==0,"rewind captured stdout");
    length=fread(buffer,1,size-1,capture); buffer[length]=0;
    expect(dup2(saved,STDOUT_FILENO)>=0,"restore stdout");
    (void)close(saved); (void)fclose(capture); return result;
}

static void execution_tests(void)
{
    char *invalid[]={"ugreen_leds_cli","power","-on","-brightness","256","--apply","--force","--json"};
    char *preview[]={"ugreenctl","led","set","power","--color","blue","--json"};
    char *missing_force[]={"ugreenctl","led","on","power","--apply","--json"};
    char *unsupported[]={"ugreenctl","led","on","power","disk9","--apply","--force","--json"};
    char *write_command[]={"ugreenctl","led","on","power","disk1","--apply","--force","--json"};
    char *legacy[]={"ugreen_leds_cli","power","disk1","-on","-color","10","20","30",
        "-status","-brightness","7","-off","--apply","--force","--json"};
    char *locate[]={"ugreenctl","disk","locate","/dev/sda","--seconds","7","--apply","--force","--json"};
    char *disk_list[]={"ugreenctl","disk","list","--map-file","/tmp/calibrated-map","--json"};
    char *disk_tsv[]={"ugreenctl","disk","map","--tsv"};
    char *identify[]={"ugreenctl","led","identify","disk1","--seconds","7","--json"};
    char *legacy_silent[]={"ugreen_leds_cli","power","-on","--apply","--force"};
    char *legacy_status[]={"ugreen_leds_cli","power","disk1","-on","-status","--apply","--force"};
    char *mixed_cap[]={"ugreenctl","led","set","power","disk2","--color","blue","--apply","--force"};
    char *palette_set[]={"ugreenctl","led","set","disk2","--color","white","--brightness","50%","--apply","--force"};
    char *palette_raw[]={"ugreenctl","led","set","disk2","--brightness","3","--apply","--force"};
    char *channel_rgb[]={"ugreenctl","led","set","power","--color","channel1","--apply","--force"};
    char *palette_on[]={"ugreenctl","led","set","disk2","--color","white","--mode","on","--apply","--force"};
    char *palette_only[]={"ugreenctl","led","set","power","disk2","--color","white","--apply","--force"};
    char *state_json[]={"ugreenctl","led","status","disk2","--json"};
    char *list_json[]={"ugreenctl","led","list","--json"};
    char captured[2048];
    reset();
    expect(ugreenctl_led_compat_cli(8,invalid)==2 && detects==0 && opens==0 && writes==0,
           "a late invalid legacy flag prevents every hardware access");
    reset();
    expect(ugreenctl_hardware_cli(7,preview)==0 && detects==1 && opens==0 && writes==0,
           "dry run does not open hardware");
    reset();
    expect(ugreenctl_hardware_cli(6,missing_force)==4 && opens==0 && writes==0,
           "experimental apply requires force before access");
    reset();
    expect(ugreenctl_hardware_cli(8,unsupported)==3 && opens==0 && writes==0,
           "every target is checked before any write");
    reset(); open_error=-EBUSY;
    expect(ugreenctl_hardware_cli(8,write_command)==4 && opens==1 && writes==0,
           "controller conflict never bypassed by force");
    reset(); failed_read_id=2;
    expect(ugreenctl_hardware_cli(8,write_command)==5 && opens==1 && closes==1 && writes==0,
           "every target is readable before writes start");
    reset();
    expect(ugreenctl_led_compat_cli(15,legacy)==0 && opens==1 && closes==1 && writes==8,
           "legacy ordered actions use a single locked session");
    expect(seen_fields[0]==UGREENCTL_LED_PATCH_MODE && seen_ids[0]==0 && seen_ids[1]==0 && seen_ids[4]==2 &&
           seen_values[0]==UGREENCTL_LED_MODE_ON && seen_fields[1]==UGREENCTL_LED_PATCH_RGB &&
           seen_values[1]==10 && seen_fields[2]==UGREENCTL_LED_PATCH_BRIGHTNESS && seen_values[2]==7 &&
           seen_values[3]==UGREENCTL_LED_MODE_OFF,"legacy operation order matches upstream semantics");
    reset(); mapped=false;
    expect(ugreenctl_hardware_cli(9,locate)==6 && scans==1 && detects==0 && opens==0 && identifies==0,
           "unknown mapping refuses locate without accessing LEDs");
    reset();
    expect(ugreenctl_hardware_cli(9,locate)==0 && scans==1 && identifies==1 && closes==1,
           "confirmed physical mapping identifies the correct LED");
    reset(); expected_map_file="/tmp/calibrated-map";
    expect(ugreenctl_hardware_cli(6,disk_list)==0 && scans==1 && detects==0,
           "disk list is read only and passes calibration path");
    reset();
    expect(capture_cli(4,disk_tsv,false,captured,sizeof(captured))==0 && scans==1 && opens==0 && writes==0 &&
           !strcmp(captured,"device\tinterface\tslot_name\tled\n/dev/sda\tsata\tata1\tdisk1\n"),
           "disk map TSV carries only physical mapping fields with no LED access");
    reset(); mapped=false;
    expect(capture_cli(4,disk_tsv,false,captured,sizeof(captured))==0 &&
           strstr(captured,"/dev/sda\tsata\t-\t-\n"),"unmapped disks cannot emit a slot or LED");
    reset();
    expect(ugreenctl_hardware_cli(7,identify)==0 && identifies==0 && opens==0,
           "identify defaults to dry run");
    reset(); no_identify=true;
    expect(ugreenctl_hardware_cli(7,identify)==3 && identifies==0 && opens==0,
           "unsupported identify capability rejects even a dry run before opening hardware");
    reset();
    expect(capture_cli(5,legacy_silent,true,captured,sizeof(captured))==0 && captured[0]==0 && writes==1,
           "successful legacy writes stay silent without status");
    reset();
    expect(capture_cli(7,legacy_status,true,captured,sizeof(captured))==0 &&
           strcmp(captured,"power: status = on, brightness = 100, color = RGB(0, 0, 255)\n"
                           "disk1: status = on, brightness = 100, color = RGB(0, 0, 255)\n")==0,
           "legacy status retains upstream text format and target order");
    reset(); palette=true;
    expect(ugreenctl_hardware_cli(9,mixed_cap)==3 && opens==0 && writes==0,
           "later target palette rejection prevents all earlier target writes");
    reset(); palette=true;
    expect(ugreenctl_hardware_cli(10,palette_set)==0 && writes==1 &&
           seen_fields[0]==(UGREENCTL_LED_PATCH_COLOR|UGREENCTL_LED_PATCH_BRIGHTNESS) &&
           seen_values[0]==UGREENCTL_LED_COLOR_WHITE && seen_brightness[0]==1,
           "new set merges palette and percent into one target intent");
    reset(); palette=true; off_state=true;
    expect(ugreenctl_hardware_cli(10,palette_on)==0 && writes==1 &&
           seen_fields[0]==(UGREENCTL_LED_PATCH_COLOR|UGREENCTL_LED_PATCH_MODE),
           "color and on arrive together for a binary LED initially off");
    reset(); palette=true; off_state=true;
    expect(ugreenctl_hardware_cli(9,palette_only)==3 && opens==1 && closes==1 && writes==0,
           "later target dormant-color limitation preflights before changing earlier RGB targets");
    reset(); palette=true;
    expect(ugreenctl_hardware_cli(8,palette_raw)==3 && opens==0 && writes==0,
           "raw brightness outside target range rejects before opening hardware");
    reset();
    expect(ugreenctl_hardware_cli(8,channel_rgb)==3 && opens==0 && writes==0,
           "hardware channel color never turns silently into RGB black");
    reset(); no_led=true;
    expect(ugreenctl_hardware_cli(9,locate)==3 && opens==0 && detects==0 && identifies==0,
           "mapped physical slot without a verified LED cannot invoke identify");
    reset(); palette=true; confirmed=false;
    unknown_fields=UGREENCTL_LED_PATCH_BRIGHTNESS|UGREENCTL_LED_PATCH_RGB|UGREENCTL_LED_PATCH_COLOR|UGREENCTL_LED_PATCH_TIMING;
    cached_fields=UGREENCTL_LED_PATCH_MODE;
    expect(capture_cli(5,state_json,false,captured,sizeof(captured))==0 &&
           strstr(captured,"\"brightness\":null") && strstr(captured,"\"rgb\":null") &&
           strstr(captured,"\"color\":null") && strstr(captured,"\"cached_fields\":[\"mode\"]") &&
           strstr(captured,"\"write_identity_confirmed\":false"),
           "JSON preserves unknown/cached state and uses backend identity confirmation");
    reset(); palette=true;
    expect(capture_cli(4,list_json,false,captured,sizeof(captured))==0 &&
           strstr(captured,"\"rgb\":false") && strstr(captured,"\"max_brightness\":2") &&
           strstr(captured,"\"min_on_ms\":20") && strstr(captured,"\"colors\":[\"white\"]"), "LED list reports target capabilities and minimum timing");
    reset(); no_led=true; expected_map_file="/tmp/calibrated-map";
    expect(capture_cli(6,disk_list,false,captured,sizeof(captured))==0 &&
           strstr(captured,"\"mapped\":true") && strstr(captured,"\"led\":null") &&
           strstr(captured,"\"slot_name\":\"ata1\"") && strstr(captured,"\"led_supported\":false"),
           "physical mapping JSON remains distinct from LED capability");
    reset(); namespace_fixture=true; expected_map_file="/tmp/calibrated-map";
    expect(capture_cli(6,disk_list,false,captured,sizeof(captured))==0 &&
           strstr(captured,"\"namespaces\":[{\"device\":\"/dev/nvme9n1\",\"wwn\":\"eui.fixture1\"},") &&
           strstr(captured,"{\"device\":\"/dev/nvme9n2\",\"wwn\":\"eui.fixture2\"}]"),
           "disk JSON keeps multiple namespace addresses attached to one physical disk");
    {
        char *text_disks[]={"ugreenctl","disk","list"};
        reset(); namespace_fixture=true;
        expect(capture_cli(3,text_disks,false,captured,sizeof(captured))==0 &&
               strstr(captured," namespaces=nvme9n1,nvme9n2\n"),
               "disk text exposes every namespace alias without inventing another physical slot");
    }
    {
        char *n76_color[]={"ugreenctl","led","set","power","disk2","--color","orange","--apply","--force","--json"};
        char *n76_blink[]={"ugreenctl","led","set","all","--mode","blink","--on-ms","200","--off-ms","200","--apply","--force","--json"};
        char *n76_explicit[]={"ugreenctl","led","set","power","disk2","--color","orange","--mode","on","--apply","--force"};
        char *n76_legacy[]={"ugreen_leds_cli","power","disk2","-brightness","1","-off","--apply","--force","--json"};
        reset(); n76=true; n76_last_effect=true;
        expect(capture_cli(10,n76_color,false,captured,sizeof(captured))==3 &&
               writes==0 && opens==1 && closes==1 && strstr(captured,"\"completed_operations\":0"),
               "later N76 effect rejects incomplete color intent before any earlier target changes");
        reset(); n76=true; n76_last_combined=true;
        expect(capture_cli(13,n76_blink,false,captured,sizeof(captured))==3 && writes==0 && closes==1,
               "later N76 combined palette rejects blink for the whole target set before writing");
        reset(); n76=true; n76_last_unknown_mode=true;
        expect(capture_cli(10,n76_color,false,captured,sizeof(captured))==3 && writes==0 && closes==1,
               "unknown N76 mode is never inferred as the enum's default off state");
        reset(); n76=true; n76_last_unknown_mode=true;
        expect(ugreenctl_hardware_cli(11,n76_explicit)==0 && writes==2 && closes==1,
               "explicit complete N76 mode and palette replace unknown state safely");
        reset(); n76=true; n76_last_unknown_mode=true;
        expect(capture_cli(9,n76_legacy,true,captured,sizeof(captured))==3 && writes==0 && closes==1,
               "legacy N76 state constraints preflight all targets before the first action");
        {
            char *sequence[]={"ugreen_leds_cli","power","disk2","-blink","200","200","-on","--force","--apply","--json"};
            char *blink[]={"ugreenctl","led","set","power","--mode","blink","--apply","--force","--json"};
            char *status[]={"ugreenctl","led","status","power","--json"};
            reset();n76=true;normal_available=false;
            expect(capture_cli(10,sequence,true,captured,sizeof(captured))==3 && writes==0 &&
                   preflights==2 && closes==1 && strstr(captured,"\"completed_operations\":0"),
                   "later legacy ON with an unavailable normal backend preflights before earlier BLINK writes");
            reset();n76=true;timer_available=false;
            expect(capture_cli(9,blink,false,captured,sizeof(captured))==3 && writes==0 && preflights==1 && closes==1,
                   "known unavailable timer rejects a merged blink intent before any fields change");
            reset();n76=true;normal_available=false;timer_available=false;
            expect(capture_cli(5,status,false,captured,sizeof(captured))==0 && writes==0 && preflights==0,
                   "read-only status does not require write-only trigger capabilities");
        }
    }
}

/* Keep these output/rounding regressions separate from stateful preflight
 * fixtures: they check the CLI contract, not a mirror of backend code. */
static void cli_contract_regression_tests(void)
{
    static const unsigned int maxima[]={1,2,254,255};
    static const unsigned int percentages[]={0,1,25,50,75,99,100};
    char percent[8],brightness_token[40],captured[4096];size_t m,p;
    char *preview[]={"ugreenctl","led","set","power","--brightness",percent,"--json"};
    char *apply[]={"ugreenctl","led","set","power","--brightness",percent,"--apply","--force","--json"};
    char *raw[]={"ugreenctl","led","set","power","--brightness","75","--apply","--force","--json"};
    char *legacy_status[]={"ugreen_leds_cli","power","-status"};
    char *legacy_json_status[]={"ugreen_leds_cli","power","-status","--json"};
    char *status_then_failure[]={"ugreen_leds_cli","power","-status","-on","--apply","--force"};
    char *json_status_then_failure[]={"ugreen_leds_cli","power","-status","-on","--apply","--force","--json"};
    char *partial_json[]={"ugreen_leds_cli","power","disk1","-on","-status","--apply","--force","--json"};
    char *partial_text[]={"ugreen_leds_cli","power","disk1","-on","-status","--apply","--force"};
    for (m=0;m<sizeof(maxima)/sizeof(maxima[0]);++m) for (p=0;p<sizeof(percentages)/sizeof(percentages[0]);++p) {
        unsigned int expected=(maxima[m]*percentages[p]+50U)/100U;
        (void)snprintf(percent,sizeof(percent),"%u%%",percentages[p]);
        (void)snprintf(brightness_token,sizeof(brightness_token),"\"brightness\":%u",expected);
        reset();fixture_max_brightness=maxima[m];
        expect(capture_cli(7,preview,false,captured,sizeof(captured))==0 && !opens && !writes &&
               strstr(captured,brightness_token) && strstr(captured,"\"dry_run\":true"),
               "percentage preview rounds directly to the target range without hardware access");
        reset();fixture_max_brightness=maxima[m];
        expect(capture_cli(9,apply,false,captured,sizeof(captured))==0 && writes==1 && seen_brightness[0]==expected &&
               strstr(captured,brightness_token),"percentage apply uses the same directly rounded target value");
    }
    reset();fixture_max_brightness=254;
    expect(capture_cli(9,raw,false,captured,sizeof(captured))==0 && writes==1 && seen_brightness[0]==75,
           "raw brightness retains raw hardware units");
    reset();fixture_status_mode=UGREENCTL_LED_MODE_BREATH;
    expect(capture_cli(3,legacy_status,true,captured,sizeof(captured))==0 &&
           strcmp(captured,"power: status = breath, brightness = 100, color = RGB(0, 0, 255)\n")==0,
           "legacy breath status matches the saved upstream contract without blink-only fields");
    reset();fixture_status_mode=UGREENCTL_LED_MODE_BLINK;
    expect(capture_cli(3,legacy_status,true,captured,sizeof(captured))==0 &&
           strstr(captured,", blink_on = ") && strstr(captured,", blink_off = "),
           "legacy blink status still includes blink timing fields");
    reset();fixture_status_mode=UGREENCTL_LED_MODE_BREATH;
    expect(capture_cli(4,legacy_json_status,true,captured,sizeof(captured))==0 &&
           strstr(captured,"\"mode\":\"breath\"") && strstr(captured,"\"on_ms\":") && strstr(captured,"\"off_ms\":"),
           "structured breath status retains its timing fields");
    reset();failed_write_id=0;
    expect(capture_cli(6,status_then_failure,true,captured,sizeof(captured))==5 &&
           strcmp(captured,"power: status = on, brightness = 100, color = RGB(0, 0, 255)\n")==0,
           "successful legacy status survives a later write failure");
    reset();failed_write_id=0;
    expect(capture_cli(7,json_status_then_failure,true,captured,sizeof(captured))==5 &&
           captured[0]=='{' && strstr(captured,"\"ok\":false") && strstr(captured,"\"completed_operations\":0") &&
           !strstr(captured,"status ="),"JSON failure remains one object after an earlier successful status read");
    reset();failed_write_id=2;
    expect(capture_cli(8,partial_json,true,captured,sizeof(captured))==5 &&
           captured[0]=='{' && strstr(captured,"\"ok\":false") && strstr(captured,"\"completed_operations\":1") &&
           !strstr(captured,"status ="),"partial JSON failure counts only successfully completed write intents");
    reset();failed_write_id=2;
    expect(capture_cli(7,partial_text,true,captured,sizeof(captured))==5 &&
           strcmp(captured,"power: status = on, brightness = 100, color = RGB(0, 0, 255)\n")==0,
           "partial legacy failure prints earlier status once and never fabricates later target status");
    reset();
}

/* Prefix routing must preserve machine-readable failures without discovering
 * a command name inside another command's operands or a global option value. */
static void command_routing_regression_tests(void)
{
    char *prefix[]={"ugreenctl","--unknown","led","list","--json"};
    char *json_first[]={"ugreenctl","--json","--unknown","led","list"};
    char *disk_prefix[]={"ugreenctl","--unknown","disk","list","--json"};
    char *write_prefix[]={"ugreenctl","--force","--unknown","led","on","power","--apply","--json"};
    char *model_prefix[]={"ugreenctl","--model","dxp4800plus","led","list","--json"};
    char *plugin_led[]={"ugreenctl","--plugin-dir","led","--unknown","fan","status","--json"};
    char *plugin_disk[]={"ugreenctl","--plugin-dir","disk","--unknown","widgets","--json"};
    char *model_led[]={"ugreenctl","--model","led","--unknown","fan","status","--json"};
    char *model_disk[]={"ugreenctl","--model","disk","--unknown","widgets","--json"};
    char *map_disk[]={"ugreenctl","--map-file","disk","--unknown","fan","status","--json"};
    char *foreign[]={"ugreenctl","--unknown","widgets","led","list","--json"};
    char *version[]={"ugreenctl","--version","led","list","--json"};
    struct { char **argv; size_t count; int expected; } cases[]={
        {prefix,sizeof(prefix)/sizeof(*prefix),2},
        {json_first,sizeof(json_first)/sizeof(*json_first),2},
        {disk_prefix,sizeof(disk_prefix)/sizeof(*disk_prefix),2},
        {write_prefix,sizeof(write_prefix)/sizeof(*write_prefix),2},
        {model_prefix,sizeof(model_prefix)/sizeof(*model_prefix),2},
        {plugin_led,sizeof(plugin_led)/sizeof(*plugin_led),-1},
        {plugin_disk,sizeof(plugin_disk)/sizeof(*plugin_disk),-1},
        {model_led,sizeof(model_led)/sizeof(*model_led),-1},
        {model_disk,sizeof(model_disk)/sizeof(*model_disk),-1},
        {map_disk,sizeof(map_disk)/sizeof(*map_disk),-1},
        {foreign,sizeof(foreign)/sizeof(*foreign),-1},
        {version,sizeof(version)/sizeof(*version),-1}
    };
    char captured[2048];
    size_t i;
    for (i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        int result;
        reset();
        result=capture_cli((int)cases[i].count,cases[i].argv,false,captured,sizeof(captured));
        expect(result==cases[i].expected,"only the first command token owns LED/disk prefix syntax errors");
        expect(detects==0 && opens==0 && writes==0 && scans==0,"routing failure never accesses hardware");
        if (result==2) {
            const char *schema=strstr(captured,"\"schema_version\":1");
            expect(captured[0]=='{' && schema && !strstr(schema+1,"\"schema_version\":1") &&
                   strstr(captured,"\"ok\":false") && strstr(captured,"\"code\":\"invalid_argument\"") &&
                   strstr(captured,"\"completed_operations\":0") &&
                   strchr(captured,'\n')==captured+strlen(captured)-1,
                   "a prefixed syntax failure emits one JSON object with exit2 and zero completed operations");
        } else expect(captured[0]==0,"foreign command routing produces no premature hardware JSON");
    }
    reset();
}

static void n76_direct_cli_tests(void)
{
    char *preview[]={"ugreenctl","led","set","power","--mode","on","--color","white","--n76-direct","1:0x31","--json"};
    char *apply[]={"ugreenctl","led","set","power","--mode","blink","--color","orange","--n76-direct","1:0x31","--json","--force","--apply"};
    char *bad[]={"ugreenctl","led","set","power","--color","white","--n76-direct","1:0x31","--json","--force","--apply"};
    char *status[]={"ugreenctl","led","status","power","--n76-direct","1:0x31","--json"};
    char captured[4096];
    reset();direct_fixture=n76=true;
    expect(capture_cli(sizeof(preview)/sizeof(*preview),preview,false,captured,sizeof(captured))==0 && !opens && !writes,"N76 preview does not open a bus");
    expect(strstr(captured,"\"transport\":\"n76-direct\"") && strstr(captured,"\"hardware_state_verified\":false"),"N76 preview declares direct unverified output");
    reset();direct_fixture=n76=true;
    expect(capture_cli(sizeof(apply)/sizeof(*apply),apply,false,captured,sizeof(captured))==0 && opens==1 && writes==1,"N76 direct blink accepted with explicit mode and color");
    expect(strstr(captured,"\"write_result\":\"sent-unverified\"") && strstr(captured,"\"state\":{\"mode\":null") && strstr(captured,"\"color\":null") && strstr(captured,"\"brightness\":null"),"N76 sent parameters never become verified output");
    reset();direct_fixture=n76=true;
    expect(capture_cli(sizeof(bad)/sizeof(*bad),bad,false,captured,sizeof(captured))!=0 && !opens && !writes,"incomplete direct request rejected before I2C");
    reset();direct_fixture=n76=true;
    expect(capture_cli(sizeof(status)/sizeof(*status),status,false,captured,sizeof(captured))==0 && !writes && opens==1,"N76 status only reads");
    expect(strstr(captured,"\"n76_mode_registers\":[0,0]") && !strstr(captured,"sent-unverified"),"reading mode zero never claims off or sent");
    reset();
}

int main(void)
{
    n76_direct_cli_tests();
    parse_tests(); execution_tests(); cli_contract_regression_tests(); command_routing_regression_tests();
    (void)puts("Hardware CLI parsing and safe execution tests passed");
    return EXIT_SUCCESS;
}
