/* Run with simulated transport only, including the existing LED regression suite. */
#define main baseline_led_tests
#include "tests/test_led.c"
#undef main

static struct ugreenctl_led_identity idx_identity(void)
{
    struct ugreenctl_led_identity id = {0};
    strcpy(id.product_name, "Default string");
    strcpy(id.product_version, "iDX6011_iDX6012 V3.0 241112");
    strcpy(id.manufacturer, "UGREEN");
    return id;
}

static int init_write(void *opaque, uint8_t id, const uint8_t frame[12])
{
    static const uint8_t sequence[5][5] = {
        {4,0,0,0,0}, {1,255,0,0,0}, {2,255,255,255,0},
        {3,255,0,0,0}, {1,255,0,0,0}
    };
    struct fixture *f = opaque;
    unsigned int sum = 0;
    CHECK(id == 0 && frame[0] == 0xa0);
    if (f->writes >= 5) return write_command(opaque,id,frame);
    CHECK(!memcmp(frame + 4, sequence[f->writes], 5));
    for (unsigned int i=0;i<9;++i) sum += frame[i];
    CHECK(frame[9] == (sum >> 8) && frame[10] == (sum & 255));
    int result = write_command(opaque, id, frame);
    /* The official ON parameter 255 is represented as mode 1 in status. */
    if (frame[4] == 3 && !f->ignore_writes) f->state[0].mode = UGREENCTL_LED_MODE_ON;
    return result;
}

int main(void)
{
    struct fixture f;
    struct ugreenctl_led_identity id = idx_identity();
    struct ugreenctl_led_access access = {.force=true, .apply=true};
    struct ugreenctl_led_session *session;
    struct ugreenctl_led_value value;
    struct ugreenctl_led_patch patch;
    struct ugreenctl_led_descriptor descriptors[10];
    struct ugreenctl_led_test_io changing_io = io;
    const struct ugreenctl_led_profile *profile;
    const uint8_t expected_rgb[11] = {0xa0,1,0,0,2,10,20,30,0,0,0xdf};
    char error[256];
    CHECK(baseline_led_tests() == 0);
    profile = ugreenctl_led_profile_find(id.product_name, id.product_version);
    CHECK(profile && profile->experimental && !strcmp(profile->protocol, "smbus-block-be"));
    CHECK(profile->i2c_address == 0x3a && profile->expected_chip_id == 0xc5b2);
    CHECK(!strcmp(profile->bus_prefix, "SMBus I801 adapter"));
    CHECK(ugreenctl_led_descriptors(profile, descriptors, 10) == 8);
    CHECK(!strcmp(descriptors[1].name, "netdev") && descriptors[1].id == 1);
    for (unsigned int i = 2; i < 8; ++i)
        CHECK(descriptors[i].id == i && descriptors[i].bay == i - 1);
    CHECK(!ugreenctl_led_profile_find("Default string", "iDX6011_iDX6012 V3.0 241112-extra"));
    CHECK(!ugreenctl_led_profile_find("Default string", "iDX6011_iDX6012 V3.1 241112"));
    CHECK(!ugreenctl_led_profile_find("Default string", ""));
    CHECK(!ugreenctl_led_profile_find("iDX6012-other", id.product_version));

    initialize(&f);
    CHECK(!ugreenctl_led_test_open(&id, &access, &io, &f, &session, error, sizeof(error)));
    CHECK(!f.writes); /* Status/open must not run the fork's write-on-start init. */
    for (unsigned int led = 0; led < 8; ++led) {
        CHECK(!ugreenctl_led_read(session, led, &value, error, sizeof(error)));
        CHECK(!f.writes || led > 0);
        patch = rgb_patch();
        CHECK(!ugreenctl_led_apply(session, led, &patch, &value, error, sizeof(error)));
        CHECK(!memcmp(f.last_frame, expected_rgb, sizeof(expected_rgb)));
        CHECK(value.red == 10 && value.green == 20 && value.blue == 30);
        patch.fields = UGREENCTL_LED_PATCH_BRIGHTNESS; patch.value.brightness = 64;
        CHECK(!ugreenctl_led_apply(session, led, &patch, &value, error, sizeof(error)) && value.brightness == 64);
        patch.fields = UGREENCTL_LED_PATCH_MODE; patch.value.mode = UGREENCTL_LED_MODE_OFF;
        CHECK(!ugreenctl_led_apply(session, led, &patch, &value, error, sizeof(error)) && value.mode == UGREENCTL_LED_MODE_OFF);
        patch.value.mode = UGREENCTL_LED_MODE_ON;
        CHECK(!ugreenctl_led_apply(session, led, &patch, &value, error, sizeof(error)) && value.mode == UGREENCTL_LED_MODE_ON);
        patch.fields |= UGREENCTL_LED_PATCH_TIMING;
        patch.value.mode = UGREENCTL_LED_MODE_BLINK; patch.value.on_ms = 400; patch.value.off_ms = 600;
        CHECK(!ugreenctl_led_apply(session, led, &patch, &value, error, sizeof(error)));
        CHECK(value.mode == UGREENCTL_LED_MODE_BLINK && value.on_ms == 400 && value.off_ms == 600);
        patch.value.mode = UGREENCTL_LED_MODE_BREATH;
        CHECK(!ugreenctl_led_apply(session, led, &patch, &value, error, sizeof(error)) && value.mode == UGREENCTL_LED_MODE_BREATH);
    }
    CHECK(ugreenctl_led_read(session, 8, &value, error, sizeof(error)) == -EINVAL);
    CHECK(ugreenctl_led_apply(session, 8, &patch, &value, error, sizeof(error)) == -EINVAL);
    f.ignore_writes = true; patch = rgb_patch(); patch.value.red = 99;
    CHECK(ugreenctl_led_apply(session, 0, &patch, &value, error, sizeof(error)) == -EIO);
    ugreenctl_led_close(session);

    for (unsigned int guard = 0; guard < 5; ++guard) {
        initialize(&f); id = idx_identity(); access.force = access.apply = true;
        if (guard == 0) access.force = false;
        if (guard == 1) access.apply = false;
        if (guard == 2) f.chip_id = 0xffff;
        if (guard == 3) f.owner_error = -EBUSY;
        if (guard == 4) f.lock_error = -EBUSY;
        int rc = ugreenctl_led_test_open(&id, &access, &io, &f, &session, error, sizeof(error));
        if (guard >= 3) CHECK(rc == -EBUSY);
        else {
            CHECK(!rc);
            CHECK(ugreenctl_led_apply(session, 0, &patch, &value, error, sizeof(error)) < 0);
            CHECK(ugreenctl_led_initialize_idx6012(session, error, sizeof(error)) < 0);
            ugreenctl_led_close(session);
        }
        CHECK(!f.writes);
    }
    access.force = access.apply = true;
    for (unsigned int named = 0; named < 2; ++named) {
        id = idx_identity(); initialize(&f);
        if (named) strcpy(id.product_name, "iDX6012");
        CHECK(!ugreenctl_led_test_open(&id, &access, &io, &f, &session, error, sizeof(error)));
        ugreenctl_led_close(session);
        strcpy(id.manufacturer, "Other vendor");
        CHECK(ugreenctl_led_test_open(&id, &access, &io, &f, &session, error, sizeof(error)) == -ENOTSUP);
        id.manufacturer[0] = '\0';
        CHECK(ugreenctl_led_test_open(&id, &access, &io, &f, &session, error, sizeof(error)) == -ENOTSUP);
        CHECK(!f.writes);
    }
    initialize(&f); id = idx_identity(); f.machine = id;
    f.machine.profile = ugreenctl_led_profile_find(id.product_name, id.product_version);
    changing_io.read_identity = read_machine;
    CHECK(!ugreenctl_led_test_open(&id, &access, &changing_io, &f, &session, error, sizeof(error)));
    strcpy(f.machine.manufacturer, "Other vendor");
    CHECK(ugreenctl_led_apply(session, 0, &patch, &value, error, sizeof(error)) == -ESTALE);
    CHECK(ugreenctl_led_initialize_idx6012(session, error, sizeof(error)) == -ESTALE);
    CHECK(!f.writes);
    ugreenctl_led_close(session);
    changing_io = io; changing_io.write_command = init_write;
    for (unsigned int scenario=0; scenario<4; ++scenario) {
        initialize(&f); id = idx_identity();
        if (scenario==1) f.ignore_writes = true;
        if (scenario==2) f.ack_failures = 1;
        if (scenario==3) f.corrupt = true;
        int rc=ugreenctl_led_test_open(&id,&access,&changing_io,&f,&session,error,sizeof(error));
        if (scenario==3) { CHECK(rc<0 && !f.writes); continue; }
        CHECK(!rc && !f.writes);
        rc=ugreenctl_led_initialize_idx6012(session,error,sizeof(error));
        CHECK(scenario==0 ? rc==0 : rc<0);
        CHECK(f.writes == (scenario==2 ? 1 : scenario==0 ? 8 : 5));
        if (scenario==0) CHECK(f.state[0].brightness==180 && f.state[0].mode==UGREENCTL_LED_MODE_ON);
        ugreenctl_led_close(session);
    }
    initialize(&f); memset(&id,0,sizeof(id)); strcpy(id.product_name,"DXP4800 Plus");
    CHECK(!ugreenctl_led_test_open(&id,&access,&io,&f,&session,error,sizeof(error)));
    CHECK(ugreenctl_led_initialize_idx6012(session,error,sizeof(error)) == -ENOTSUP);
    CHECK(!f.writes); ugreenctl_led_close(session);
    puts("iDX6012: eight LEDs, explicit initialization, BE frames, readback and access guards passed");
    return 0;
}
