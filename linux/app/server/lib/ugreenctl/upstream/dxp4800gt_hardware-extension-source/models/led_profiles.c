#include "led/led.h"

#include <string.h>

#define RGB_LED(n, i, b, sys) { .name=(n), .id=(i), .bay=(b), .rgb=true, \
    .mode_mask=UGREENCTL_LED_MODES_ALL, .max_brightness=255, .sysfs_name=(sys) }
#define COMMON_RGB RGB_LED("power",0,0,"power"), RGB_LED("netdev",1,0,"network_stat")
#define DISK_RGB(n, i) RGB_LED("disk" #n,(i),(n),"disk" #n)
static const struct ugreenctl_led_descriptor rgb2[] = {COMMON_RGB, DISK_RGB(1,2),DISK_RGB(2,3)};
static const struct ugreenctl_led_descriptor rgb4[] = {COMMON_RGB, DISK_RGB(1,2),DISK_RGB(2,3),DISK_RGB(3,4),DISK_RGB(4,5)};
static const struct ugreenctl_led_descriptor rgb6[] = {COMMON_RGB, DISK_RGB(1,2),DISK_RGB(2,3),DISK_RGB(3,4),DISK_RGB(4,5),DISK_RGB(5,6),DISK_RGB(6,7)};
static const struct ugreenctl_led_descriptor rgb8[] = {COMMON_RGB, DISK_RGB(1,2),DISK_RGB(2,3),DISK_RGB(3,4),DISK_RGB(4,5),DISK_RGB(5,6),DISK_RGB(6,7),DISK_RGB(7,8),DISK_RGB(8,9)};
/* The official iDX6011 Pro name table puts the second network LED at ID 2.
 * Disk IDs therefore start at 3, unlike the classic single-network layout. */
static const struct ugreenctl_led_descriptor idx6[] = {COMMON_RGB, RGB_LED("netdev2",2,0,"network_stat2"),DISK_RGB(1,3),DISK_RGB(2,4),DISK_RGB(3,5),DISK_RGB(4,6),DISK_RGB(5,7),DISK_RGB(6,8)};
#define N76_COLORS (UGREENCTL_LED_COLOR_BIT(UGREENCTL_LED_COLOR_WHITE) | \
    UGREENCTL_LED_COLOR_BIT(UGREENCTL_LED_COLOR_ORANGE) | \
    UGREENCTL_LED_COLOR_BIT(UGREENCTL_LED_COLOR_WHITE_ORANGE))
#define N76_LED(n,i,b) { .name=(n),.id=(i),.bay=(b),.rgb=false, \
    .mode_mask=7,.max_brightness=254,.color_mask=N76_COLORS,.sysfs_name=(n),.min_on_ms=20,.min_off_ms=20,.min_brightness=2 }
static const struct ugreenctl_led_descriptor n76_power[] = {N76_LED("power",0,0)};
#define DH2600_LED(n,i,b) { .name=(n),.id=(i),.bay=(b),.rgb=false, \
    .mode_mask=7,.max_brightness=1,.color_mask=N76_COLORS,.sysfs_name=(n),.min_on_ms=20,.min_off_ms=20 }
static const struct ugreenctl_led_descriptor n76_dh2600[] = {DH2600_LED("power",0,0),DH2600_LED("disk1",1,1),DH2600_LED("disk2",2,2)};
#define GPIO_COLORS(second) (UGREENCTL_LED_COLOR_BIT(UGREENCTL_LED_COLOR_WHITE) | \
    UGREENCTL_LED_COLOR_BIT(second) | UGREENCTL_LED_COLOR_BIT(UGREENCTL_LED_COLOR_CHANNEL1) | \
    UGREENCTL_LED_COLOR_BIT(UGREENCTL_LED_COLOR_CHANNEL2))
/* The stock GPIO driver switches two mutually exclusive channels and treats
 * every nonzero brightness as ON. Neither RGB nor a fade is physical PWM. */
#define GPIO_LED(n,i,b,s,c) { .name=(n),.id=(i),.bay=(b),.rgb=false,.mode_mask=3, \
    .max_brightness=1,.color_mask=GPIO_COLORS(c),.sysfs_name=(s) }
static const struct ugreenctl_led_descriptor gpio2[] = {GPIO_LED("power",0,0,"power",UGREENCTL_LED_COLOR_ORANGE),GPIO_LED("netdev",1,0,"network_stat",UGREENCTL_LED_COLOR_ORANGE),GPIO_LED("disk1",2,1,"disk1",UGREENCTL_LED_COLOR_ORANGE),GPIO_LED("disk2",3,2,"disk2",UGREENCTL_LED_COLOR_ORANGE)};
static const struct ugreenctl_led_descriptor gpio4[] = {GPIO_LED("power",0,0,"power",UGREENCTL_LED_COLOR_RED),GPIO_LED("netdev",1,0,"network_stat",UGREENCTL_LED_COLOR_RED),GPIO_LED("disk1",2,1,"disk1",UGREENCTL_LED_COLOR_RED),GPIO_LED("disk2",3,2,"disk2",UGREENCTL_LED_COLOR_RED),GPIO_LED("disk3",4,3,"disk3",UGREENCTL_LED_COLOR_RED),GPIO_LED("disk4",5,4,"disk4",UGREENCTL_LED_COLOR_RED)};
#define CS201X_LED(n,i,b,s) { .name=(n),.id=(i),.bay=(b),.rgb=true,.mode_mask=UGREENCTL_LED_MODES_ALL, \
    .max_brightness=255,.sysfs_name=(s),.min_on_ms=100,.min_off_ms=100, \
    .min_breath_on_ms=1000,.min_breath_off_ms=500,.max_cycle_ms=12750,.timing_quantum_ms=50 }
static const struct ugreenctl_led_descriptor cs201x2[] = {CS201X_LED("power",0,0,"power"),CS201X_LED("netdev",1,0,"network_stat"),CS201X_LED("disk1",2,1,"disk1"),CS201X_LED("disk2",3,2,"disk2")};

#define ARRAY_COUNT(a) (sizeof(a)/sizeof((a)[0]))
#define MCU_PROFILE(key, model, revision, proto, bays, layout, bus, proof) \
    { .id=(key),.product_name=(model),.product_version=(revision),.protocol=(proto), \
      .disk_count=(bays),.experimental=true,.evidence=(proof),.leds=(layout),.led_count=ARRAY_COUNT(layout), \
      .bus_prefix=(bus),.i2c_address=0x3a,.expected_chip_id=0xc5b2,.chip_register=0x5a,.chip_read=UGREENCTL_LED_CHIP_WORD_LE }
#define LEGACY(key,model,bays,layout) MCU_PROFILE(key,model,NULL,"legacy-i2c-block",bays,layout,"SMBus I801 adapter","docs/ALL_MODELS_LED_EVIDENCE.zh-CN.md")
#define N76_PROFILE(key,model,bays,layout,module) \
    { .id=(key),.product_name=(model),.protocol="sysfs-n76",.disk_count=(bays),.experimental=true, \
      .evidence="docs/ALL_MODELS_LED_EVIDENCE.zh-CN.md",.leds=(layout),.led_count=ARRAY_COUNT(layout), \
      .expected_chip_id=0xa5b5,.chip_register=0x5a,.chip_read=UGREENCTL_LED_CHIP_DRIVER_PROBE,.sysfs_driver=(module) }
#define GPIO_PROFILE(key,model,bays,layout,compat) \
    { .id=(key),.product_name=(model),.protocol="sysfs-gpio",.disk_count=(bays),.experimental=true, \
      .evidence="docs/ALL_MODELS_LED_EVIDENCE.zh-CN.md",.leds=(layout),.led_count=ARRAY_COUNT(layout), \
      .chip_read=UGREENCTL_LED_CHIP_DRIVER_PROBE,.sysfs_driver="ug_gpio_leds",.dt_compatible=(compat) }

/* Per-model routing comes from the official firmware. MCU family ID alone
 * NEVER selects legacy/block framing or expands a model/revision allowlist. */
static const struct ugreenctl_led_profile profiles[] = {
    LEGACY("dx4600-legacy","DX4600",4,rgb4),
    LEGACY("dx4600plus-legacy","DX4600+",4,rgb4),
    LEGACY("dx4600pro-legacy","DX4600 Pro",4,rgb4),
    LEGACY("dx4700-legacy","DX4700",4,rgb4),
    LEGACY("dx4700plus-legacy","DX4700+",4,rgb4),
    MCU_PROFILE("dxp2800-em-legacy","DXP2800","EM_DXP2800_V1.0.24","legacy-i2c-block",2,rgb2,"SMBus I801 adapter","https://github.com/miskcoo/ugreen_leds_controller/issues/128"),
    MCU_PROFILE("dxp2800-v12-block","DXP2800","DXP2800 V1.2 251215","smbus-block-le",2,rgb2,"SMBus I801 adapter","https://github.com/miskcoo/ugreen_leds_controller/issues/128"),
    { .id="dxp2800gt-cs201x",.product_name="DXP2800 GT",.protocol="cs201x-sio",.disk_count=2,.experimental=true,
      .evidence="docs/DXP2800_GT_LED_REVERSE.zh-CN.md",.leds=cs201x2,.led_count=ARRAY_COUNT(cs201x2),.expected_chip_id=0x2011 },
    LEGACY("dxp4800-legacy","DXP4800",4,rgb4),
    LEGACY("dxp4800s-legacy","DXP4800S",4,rgb4),
    LEGACY("dxp4800plus-legacy","DXP4800 Plus",4,rgb4),
    LEGACY("dxp4800pro-legacy","DXP4800 Pro",4,rgb4),
    MCU_PROFILE("dxp4800gt-block","DXP4800 GT",NULL,"smbus-block-be",4,rgb4,"SMBus PIIX4 adapter","docs/ALL_MODELS_LED_EVIDENCE.zh-CN.md"),
    MCU_PROFILE("dxp6800-block","DXP6800",NULL,"smbus-block-be",6,rgb6,"SMBus I801 adapter","docs/ALL_MODELS_LED_EVIDENCE.zh-CN.md"),
    LEGACY("dxp6800plus-legacy","DXP6800 Plus",6,rgb6),
    LEGACY("dxp6800pro-legacy","DXP6800 Pro",6,rgb6),
    LEGACY("dxp6800ultra-legacy","DXP6800 Ultra",6,rgb6),
    LEGACY("dxp8800-legacy","DXP8800",8,rgb8),
    LEGACY("dxp8800plus-legacy","DXP8800 Plus",8,rgb8),
    LEGACY("dxp8800pro-legacy","DXP8800 Pro",8,rgb8),
    LEGACY("dxp8800ultra-legacy","DXP8800 Ultra",8,rgb8),
    MCU_PROFILE("idx6011pro-block","iDX6011 Pro",NULL,"smbus-block-be",6,idx6,"SMBus I801 adapter","docs/ALL_MODELS_LED_EVIDENCE.zh-CN.md"),
    N76_PROFILE("dxp480t-n76","DXP480T Plus",0,n76_power,"leds_mcu_n76e003"),
    N76_PROFILE("dh2600-n76","DH2600",2,n76_dh2600,"leds_mcu_n76e003_dh2600"),
    GPIO_PROFILE("dh2300-gpio","DH2300",2,gpio2,"rockchip,rk3576"),
    GPIO_PROFILE("dh4300plus-gpio","DH4300 Plus",4,gpio4,"rockchip,rk3588")
};

const struct ugreenctl_led_profile *ugreenctl_led_profiles(size_t *count)
{
    if (count != NULL) *count = sizeof(profiles) / sizeof(profiles[0]);
    return profiles;
}

const struct ugreenctl_led_profile *ugreenctl_led_profile_find(
    const char *product_name, const char *product_version)
{
    size_t i;
    if (product_name == NULL) return NULL;
    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        if (strcmp(product_name, profiles[i].product_name) != 0) continue;
        if (profiles[i].product_version != NULL &&
            (product_version == NULL ||
             strcmp(product_version, profiles[i].product_version) != 0)) continue;
        return &profiles[i];
    }
    return NULL;
}

size_t ugreenctl_led_descriptors(const struct ugreenctl_led_profile *profile,
                               struct ugreenctl_led_descriptor *out,
                               size_t capacity)
{
    size_t i, count;
    if (profile == NULL || profile->leds == NULL || profile->led_count > UGREENCTL_MAX_LEDS) return 0;
    count = profile->led_count;
    if (out != NULL) {
        for (i = 0; i < count && i < capacity; ++i) out[i] = profile->leds[i];
    }
    return count;
}
