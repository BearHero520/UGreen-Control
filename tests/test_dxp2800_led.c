/* Compile with the pinned compatibility CLI sources, never against hardware. */
#define main baseline_led_tests
#include "tests/test_led.c"
#undef main

int main(void)
{
    struct fixture f;
    struct ugreenctl_led_identity id = {0};
    struct ugreenctl_led_access access = {.force=true, .apply=true};
    struct ugreenctl_led_session *session;
    struct ugreenctl_led_value value;
    struct ugreenctl_led_patch patch = rgb_patch();
    const struct ugreenctl_led_profile *profile;
    char error[256];
    CHECK(baseline_led_tests() == 0);
    profile = ugreenctl_led_profile_find("DXP2800", "EM_DXP2800_V1.0.26");
    CHECK(profile && !strcmp(profile->protocol, "legacy-i2c-block") && profile->disk_count == 2);
    CHECK(!ugreenctl_led_profile_find("DXP2800", "EM_DXP2800_V1.0.26-extra"));
    CHECK(!ugreenctl_led_profile_find("DXP2800 Engineering", "EM_DXP2800_V1.0.26"));
    strcpy(id.product_name, "DXP2800"); strcpy(id.product_version, "EM_DXP2800_V1.0.26");
    initialize(&f);
    CHECK(!ugreenctl_led_test_open(&id, &access, &io, &f, &session, error, sizeof(error)));
    for (unsigned int led = 0; led < 4; ++led) {
        CHECK(!ugreenctl_led_read(session, led, &value, error, sizeof(error)));
        patch = rgb_patch();
        CHECK(!ugreenctl_led_apply(session, led, &patch, &value, error, sizeof(error)));
        CHECK(value.red == 10 && value.green == 20 && value.blue == 30);
        patch.fields = UGREENCTL_LED_PATCH_BRIGHTNESS; patch.value.brightness = 64;
        CHECK(!ugreenctl_led_apply(session, led, &patch, &value, error, sizeof(error)) && value.brightness == 64);
        patch.fields = UGREENCTL_LED_PATCH_MODE;
        patch.value.mode = UGREENCTL_LED_MODE_OFF;
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
    CHECK(ugreenctl_led_apply(session, 4, &patch, &value, error, sizeof(error)) == -EINVAL);
    f.ignore_writes = true; patch = rgb_patch(); patch.value.red = 99;
    CHECK(ugreenctl_led_apply(session, 0, &patch, &value, error, sizeof(error)) == -EIO);
    ugreenctl_led_close(session);
    initialize(&f); access.force = false;
    CHECK(!ugreenctl_led_test_open(&id, &access, &io, &f, &session, error, sizeof(error)));
    CHECK(ugreenctl_led_apply(session, 0, &patch, &value, error, sizeof(error)) == -EPERM && !f.writes);
    ugreenctl_led_close(session);
    initialize(&f); access.force = true; access.apply = false;
    CHECK(!ugreenctl_led_test_open(&id, &access, &io, &f, &session, error, sizeof(error)));
    CHECK(ugreenctl_led_apply(session, 0, &patch, &value, error, sizeof(error)) == -EPERM && !f.writes);
    ugreenctl_led_close(session);
    initialize(&f); access.apply = true; f.chip_id = 0xffff;
    CHECK(!ugreenctl_led_test_open(&id, &access, &io, &f, &session, error, sizeof(error)));
    CHECK(ugreenctl_led_apply(session, 0, &patch, &value, error, sizeof(error)) == -ENOTSUP && !f.writes);
    ugreenctl_led_close(session);
    initialize(&f); f.owner_error = -EBUSY;
    CHECK(ugreenctl_led_test_open(&id, &access, &io, &f, &session, error, sizeof(error)) == -EBUSY && !f.writes);
    puts("DXP2800 EM26 four-LED effects, readback and access guards passed");
    return 0;
}
