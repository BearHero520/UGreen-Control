/* Verify MCU I2C retries, ACK polling, proc owner races, and DX4600/DXP4800S bus discovery. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

int fake_open(const char *path, int flags, ...);
int fake_close(int fd);
int fake_fstat(int fd, struct stat *st);
int fake_flock(int fd, int operation);
int fake_ioctl(int fd, unsigned long request, ...);
DIR *fake_opendir(const char *path);
struct dirent *fake_readdir(DIR *d);
int fake_closedir(DIR *d);
ssize_t fake_readlink(const char *path, char *buf, size_t size);
FILE *fake_fopen(const char *path, const char *mode);
char *fake_fgets(char *s, int size, FILE *stream);
size_t fake_fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
int fake_ferror(FILE *stream);
int fake_fclose(FILE *stream);

#undef _POSIX_C_SOURCE
#define open fake_open
#define close fake_close
#define fstat fake_fstat
#define flock fake_flock
#define ioctl fake_ioctl
#define opendir fake_opendir
#define readdir fake_readdir
#define closedir fake_closedir
#define readlink fake_readlink
#define fopen fake_fopen
#define fgets fake_fgets
#define fread fake_fread
#define ferror fake_ferror
#define fclose fake_fclose
#include "led/led.c"
#undef open
#undef close
#undef fstat
#undef flock
#undef ioctl
#undef opendir
#undef readdir
#undef closedir
#undef readlink
#undef fopen
#undef fgets
#undef fread
#undef ferror
#undef fclose

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

enum fake_dir_kind { FAKE_DIR_I2C = 1, FAKE_DIR_PROC = 2 };
enum fake_file_kind {
    FAKE_FILE_NONE = 0,
    FAKE_FILE_VANISH_COMM,
    FAKE_FILE_VANISH_CMDLINE
};

static const char *bus_names[4] = {
    "Synopsys DesignWare I2C adapter",
    "Synopsys DesignWare I2C adapter",
    "SMBus I801 adapter at efa0",
    "i915 gmbus dpb"
};
static int bus_chip_ids[4];
static unsigned int i2c_cursor, proc_cursor;
static bool include_foreign_owner;
static int lock_contend_attempts;
static int chip_fail_attempts;
static int chip_zero_attempts;
static int ack_busy_attempts;
static int ack_eio_attempts;
static int drop_writes;
static int lag_status_reads;
static unsigned int total_writes, total_ack_reads, total_chip_reads, total_pauses, opened_mask;
static struct ugreenctl_led_value mcu_state[10], prev_state[10];
static enum fake_file_kind stream_kind;
static FILE *tracked_stream;

DIR *fake_opendir(const char *path)
{
    if (!strcmp(path, "/sys/class/i2c-dev")) {
        i2c_cursor = 0;
        return (DIR *)(uintptr_t)FAKE_DIR_I2C;
    }
    if (!strcmp(path, "/proc")) {
        proc_cursor = 0;
        return (DIR *)(uintptr_t)FAKE_DIR_PROC;
    }
    errno = ENOENT;
    return NULL;
}

struct dirent *fake_readdir(DIR *d)
{
    static struct dirent entry;
    uintptr_t kind = (uintptr_t)d;
    if (kind == FAKE_DIR_I2C) {
        if (i2c_cursor >= 4) return NULL;
        snprintf(entry.d_name, sizeof(entry.d_name), "i2c-%u", i2c_cursor++);
        return &entry;
    }
    if (kind == FAKE_DIR_PROC) {
        static const char *const pids_clean[] = {"201", "202", "203", "204", "206"};
        static const char *const pids_foreign[] = {"201", "202", "203", "204", "205", "206"};
        const char *const *list = include_foreign_owner ? pids_foreign : pids_clean;
        unsigned int count = include_foreign_owner ? 6U : 5U;
        if (proc_cursor >= count) return NULL;
        snprintf(entry.d_name, sizeof(entry.d_name), "%s", list[proc_cursor++]);
        return &entry;
    }
    return NULL;
}

int fake_closedir(DIR *d)
{
    CHECK((uintptr_t)d == FAKE_DIR_I2C || (uintptr_t)d == FAKE_DIR_PROC);
    return 0;
}

ssize_t fake_readlink(const char *path, char *buf, size_t size)
{
    const char *target = NULL;
    size_t len;
    if (!strcmp(path, "/proc/self/exe") || !strcmp(path, "/proc/203/exe"))
        target = "/vol1/@appcenter/App.Native.UGreenLED/server/bin/ugreen_leds_cli.compat";
    else if (!strcmp(path, "/proc/204/exe"))
        target = "/usr/bin/bash";
    else if (!strcmp(path, "/proc/205/exe"))
        target = "/usr/local/bin/ugreen_leds_cli";
    else if (!strcmp(path, "/proc/206/exe"))
        target = "/usr/bin/sleep";
    else {
        errno = ESRCH;
        return -1;
    }
    len = strlen(target);
    if (len > size) len = size;
    memcpy(buf, target, len);
    return (ssize_t)len;
}

static FILE *make_mem_file(const void *data, size_t len)
{
    FILE *f = tmpfile();
    CHECK(f != NULL);
    if (len > 0) CHECK(fwrite(data, 1, len, f) == len);
    rewind(f);
    return f;
}

FILE *fake_fopen(const char *path, const char *mode)
{
    unsigned int bus;
    (void)mode;
    if (sscanf(path, "/sys/class/i2c-dev/i2c-%u/name", &bus) == 1 && bus < 4)
        return make_mem_file(bus_names[bus], strlen(bus_names[bus]));
    if (!strcmp(path, "/proc/201/comm")) {
        tracked_stream = make_mem_file("", 0);
        stream_kind = FAKE_FILE_VANISH_COMM;
        return tracked_stream;
    }
    if (!strcmp(path, "/proc/202/comm"))
        return make_mem_file("short-task\n", 11);
    if (!strcmp(path, "/proc/202/cmdline")) {
        tracked_stream = make_mem_file("", 0);
        stream_kind = FAKE_FILE_VANISH_CMDLINE;
        return tracked_stream;
    }
    if (!strcmp(path, "/proc/203/comm") || !strcmp(path, "/proc/204/comm") || !strcmp(path, "/proc/205/comm"))
        return make_mem_file("ugreen_leds_cli\n", 16);
    if (!strcmp(path, "/proc/204/cmdline")) {
        static const char cmd[] = "/usr/bin/bash\0/vol1/@appcenter/App.Native.UGreenLED/server/bin/ugreen_leds_cli\0all\0-status\0";
        return make_mem_file(cmd, sizeof(cmd) - 1);
    }
    if (!strcmp(path, "/proc/205/cmdline")) {
        static const char cmd[] = "/usr/local/bin/ugreen_leds_cli\0all\0-status\0";
        return make_mem_file(cmd, sizeof(cmd) - 1);
    }
    if (!strcmp(path, "/proc/206/comm"))
        return make_mem_file("sleep\n", 6);
    if (!strcmp(path, "/proc/206/cmdline")) {
        static const char cmd[] = "sleep\0001\0";
        return make_mem_file(cmd, sizeof(cmd) - 1);
    }
    errno = ENOENT;
    return NULL;
}

char *fake_fgets(char *s, int size, FILE *stream)
{
    if (stream == tracked_stream && stream_kind == FAKE_FILE_VANISH_COMM) {
        errno = ESRCH;
        return NULL;
    }
    return fgets(s, size, stream);
}

size_t fake_fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (stream == tracked_stream && stream_kind == FAKE_FILE_VANISH_CMDLINE) {
        errno = ESRCH;
        return 0;
    }
    return fread(ptr, size, nmemb, stream);
}

int fake_ferror(FILE *stream)
{
    if (stream == tracked_stream && stream_kind != FAKE_FILE_NONE) return 1;
    return ferror(stream);
}

int fake_fclose(FILE *stream)
{
    if (stream == tracked_stream) {
        tracked_stream = NULL;
        stream_kind = FAKE_FILE_NONE;
    }
    return fclose(stream);
}

int fake_open(const char *path, int flags, ...)
{
    unsigned int bus;
    CHECK((flags & O_NOFOLLOW) && (flags & O_CLOEXEC));
    if (!strcmp(path, "/run/ugreenctl-led-mcu.lock")) return 200;
    if (sscanf(path, "/dev/i2c-%u", &bus) == 1 && bus < 4) {
        opened_mask |= 1U << bus;
        return 100 + (int)bus;
    }
    errno = ENOENT;
    return -1;
}

int fake_close(int fd)
{
    CHECK(fd == 200 || (fd >= 100 && fd < 104));
    return 0;
}

int fake_fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    if (fd == 200) {
        st->st_mode = S_IFREG | 0600;
        st->st_uid = 0;
        st->st_nlink = 1;
        return 0;
    }
    CHECK(fd >= 100 && fd < 104);
    st->st_mode = S_IFCHR | 0600;
    st->st_rdev = makedev(89, (unsigned int)(fd - 100));
    return 0;
}

int fake_flock(int fd, int operation)
{
    CHECK(fd == 200);
    if (operation == LOCK_UN) return 0;
    CHECK(operation == (LOCK_EX | LOCK_NB));
    if (lock_contend_attempts > 0) {
        --lock_contend_attempts;
        errno = EWOULDBLOCK;
        return -1;
    }
    return 0;
}

static void encode_frame(const struct ugreenctl_led_value *v, uint8_t out[11])
{
    unsigned int sum = 0, cycle = v->on_ms + v->off_ms;
    out[0] = (uint8_t)v->mode;
    out[1] = (uint8_t)v->brightness;
    out[2] = (uint8_t)v->red;
    out[3] = (uint8_t)v->green;
    out[4] = (uint8_t)v->blue;
    out[5] = (uint8_t)(cycle >> 8);
    out[6] = (uint8_t)cycle;
    out[7] = (uint8_t)(v->on_ms >> 8);
    out[8] = (uint8_t)v->on_ms;
    for (unsigned int i = 0; i < 9; ++i) sum += out[i];
    out[9] = (uint8_t)(sum >> 8);
    out[10] = (uint8_t)sum;
}

int fake_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    int bus = fd - 100;
    CHECK(bus >= 0 && bus < 4);
    va_start(ap, request);
    if (request == I2C_FUNCS) {
        unsigned long *f = va_arg(ap, unsigned long *);
        *f = I2C_FUNC_SMBUS_READ_WORD_DATA | I2C_FUNC_SMBUS_READ_BYTE_DATA |
             I2C_FUNC_SMBUS_READ_I2C_BLOCK | I2C_FUNC_SMBUS_WRITE_I2C_BLOCK |
             I2C_FUNC_SMBUS_WRITE_BLOCK_DATA;
        va_end(ap);
        return 0;
    }
    if (request == I2C_SLAVE) {
        CHECK(va_arg(ap, int) == 0x3a);
        va_end(ap);
        return 0;
    }
    CHECK(request == I2C_SMBUS);
    {
        struct i2c_smbus_ioctl_data *t = va_arg(ap, struct i2c_smbus_ioctl_data *);
        va_end(ap);
        if (t->read_write == I2C_SMBUS_READ && t->command == 0x5a) {
            ++total_chip_reads;
            if (!bus_chip_ids[bus]) { errno = ENXIO; return -1; }
            if (chip_fail_attempts > 0) { --chip_fail_attempts; errno = ENXIO; return -1; }
            if (chip_zero_attempts > 0) { --chip_zero_attempts; t->data->word = 0; return 0; }
            t->data->word = (uint16_t)bus_chip_ids[bus];
            return 0;
        }
        if (t->read_write == I2C_SMBUS_READ && t->command == 0x80) {
            ++total_ack_reads;
            if (ack_eio_attempts > 0) { --ack_eio_attempts; errno = EIO; return -1; }
            t->data->byte = ack_busy_attempts > 0 ? (--ack_busy_attempts, 0) : 1;
            return 0;
        }
        if (t->read_write == I2C_SMBUS_READ && t->command >= 0x81 && t->command <= 0x8a) {
            uint8_t id = (uint8_t)(t->command - 0x81);
            const struct ugreenctl_led_value *src = &mcu_state[id];
            if (lag_status_reads > 0) { --lag_status_reads; src = &prev_state[id]; }
            t->data->block[0] = 11;
            encode_frame(src, t->data->block + 1);
            return 0;
        }
        if (t->read_write == I2C_SMBUS_WRITE) {
            uint8_t id = t->command;
            const uint8_t *raw = t->data->block + 1;
            uint8_t cmd = raw[0] == 0xa0 ? raw[4] : raw[5];
            const uint8_t *p = raw[0] == 0xa0 ? raw + 5 : raw + 6;
            CHECK(id < 10);
            ++total_writes;
            if (drop_writes > 0) { --drop_writes; return 0; }
            prev_state[id] = mcu_state[id];
            if (cmd == 1) mcu_state[id].brightness = p[0];
            else if (cmd == 2) { mcu_state[id].red = p[0]; mcu_state[id].green = p[1]; mcu_state[id].blue = p[2]; }
            else if (cmd == 3) mcu_state[id].mode = (enum ugreenctl_led_mode)p[0];
            else if (cmd == 4 || cmd == 5) {
                unsigned int cycle = ((unsigned int)p[0] << 8) | p[1];
                mcu_state[id].mode = cmd == 4 ? UGREENCTL_LED_MODE_BLINK : UGREENCTL_LED_MODE_BREATH;
                mcu_state[id].on_ms = ((unsigned int)p[2] << 8) | p[3];
                mcu_state[id].off_ms = cycle - mcu_state[id].on_ms;
            }
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

static void fast_pause(void *opaque, unsigned int ms)
{
    (void)opaque;
    (void)ms;
    ++total_pauses;
}

static int no_owners(void *opaque)
{
    (void)opaque;
    return 0;
}

static int same_identity(void *opaque, struct ugreenctl_led_identity *out, char *error, size_t size)
{
    struct ugreenctl_led_session *s = opaque;
    (void)error;
    (void)size;
    *out = s->identity;
    return 0;
}

static void reset_env(void)
{
    memset(bus_chip_ids, 0, sizeof(bus_chip_ids));
    bus_chip_ids[2] = 0xc5b2;
    include_foreign_owner = false;
    lock_contend_attempts = chip_fail_attempts = chip_zero_attempts = 0;
    ack_busy_attempts = ack_eio_attempts = drop_writes = lag_status_reads = 0;
    total_writes = total_ack_reads = total_chip_reads = total_pauses = opened_mask = 0;
    for (unsigned int i = 0; i < 10; ++i) {
        mcu_state[i] = (struct ugreenctl_led_value){
            .mode = UGREENCTL_LED_MODE_ON, .brightness = 180, .red = 255, .green = 255, .blue = 255
        };
        prev_state[i] = mcu_state[i];
    }
}

static void init_production_session(struct ugreenctl_led_session *s, const char *product)
{
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    s->lock_fd = -1;
    s->access.force = true;
    s->access.apply = true;
    strcpy(s->identity.product_name, product);
    s->identity.profile = ugreenctl_led_profile_find(product, "");
    CHECK(s->identity.profile != NULL);
    s->io = production_io;
    s->io.owners = no_owners;
    s->io.pause_ms = fast_pause;
    s->io.read_identity = same_identity;
    s->opaque = s;
}

int main(void)
{
    struct ugreenctl_led_session s;
    struct ugreenctl_led_value value;
    struct ugreenctl_led_patch patch = {0};
    char error[448];

    /* 1. /proc vanished processes & peer/wrapper instances do not trigger -EIO/-EBUSY. */
    reset_env();
    CHECK(ugreenctl_led_process_owners() == 0);
    include_foreign_owner = true;
    CHECK(ugreenctl_led_process_owners() == -EBUSY);

    /* 2. Lock contention retries cleanly before checking owners, and transient 0x5a recovers. */
    reset_env();
    init_production_session(&s, "DXP8800 Pro");
    lock_contend_attempts = 4;
    chip_fail_attempts = 1;
    chip_zero_attempts = 1;
    CHECK(acquire(&s, error, sizeof(error)) == 0 && s.locked);
    CHECK(find_bus(&s, error, sizeof(error)) == 0 && s.bus_number == 2);
    CHECK(probe(&s, error, sizeof(error)) == 0 && s.chip_id == 0xc5b2);

    /* 3. Slow MCU ACK polling recovers without re-issuing the write. */
    ack_eio_attempts = 1;
    ack_busy_attempts = 3;
    patch.fields = UGREENCTL_LED_PATCH_RGB;
    patch.value.red = 0; patch.value.green = 199; patch.value.blue = 255;
    CHECK(ugreenctl_led_apply(&s, 2, &patch, &value, error, sizeof(error)) == 0);
    CHECK(total_writes == 1 && total_ack_reads == 5);
    CHECK(value.red == 0 && value.green == 199 && value.blue == 255);

    /* 4. Status shadow lag settles via 10ms re-read without duplicating writes. */
    total_writes = 0;
    lag_status_reads = 1;
    patch.value.red = 255; patch.value.green = 64; patch.value.blue = 0;
    CHECK(ugreenctl_led_apply(&s, 2, &patch, &value, error, sizeof(error)) == 0);
    CHECK(total_writes == 1 && value.red == 255 && value.green == 64 && value.blue == 0);

    /* 5. Dropped writes with stale 0x80==1 ACK retry up to 4 times and succeed. */
    total_writes = 0;
    drop_writes = 2;
    patch.fields = UGREENCTL_LED_PATCH_RGB | UGREENCTL_LED_PATCH_BRIGHTNESS |
                   UGREENCTL_LED_PATCH_MODE | UGREENCTL_LED_PATCH_TIMING;
    patch.value.red = 12; patch.value.green = 34; patch.value.blue = 56;
    patch.value.brightness = 90;
    patch.value.mode = UGREENCTL_LED_MODE_BLINK;
    patch.value.on_ms = 200; patch.value.off_ms = 400;
    CHECK(ugreenctl_led_apply(&s, 3, &patch, &value, error, sizeof(error)) == 0);
    CHECK(total_writes == 5);
    CHECK(value.red == 12 && value.brightness == 90 && value.mode == UGREENCTL_LED_MODE_BLINK);

    /* Persistent write failure still fails after 4 attempts. */
    total_writes = 0;
    drop_writes = 10;
    patch.fields = UGREENCTL_LED_PATCH_BRIGHTNESS;
    patch.value.brightness = 11;
    CHECK(ugreenctl_led_apply(&s, 3, &patch, &value, error, sizeof(error)) == -EIO);
    CHECK(total_writes == 4);
    if (s.fd >= 0) fake_close(s.fd);
    production_unlock(&s);

    /* 6. DX4600 / DX4700 / DXP4800S try I801 first and fall back to verified DesignWare. */
    static const char *const multibus_models[] = {
        "DX4600", "DX4600+", "DX4600 Pro", "DX4700", "DX4700+", "DXP4800S"
    };
    for (size_t i = 0; i < sizeof(multibus_models) / sizeof(multibus_models[0]); ++i) {
        reset_env();
        init_production_session(&s, multibus_models[i]);
        s.locked = true;
        CHECK(find_bus(&s, error, sizeof(error)) == 0 && s.bus_number == 2);
        CHECK(opened_mask == (1U << 2));
        fake_close(s.fd);

        reset_env();
        bus_chip_ids[2] = 0;
        bus_chip_ids[1] = 0xc5b2;
        init_production_session(&s, multibus_models[i]);
        s.locked = true;
        CHECK(find_bus(&s, error, sizeof(error)) == 0 && s.bus_number == 1);
        fake_close(s.fd);

        reset_env();
        bus_chip_ids[2] = 0;
        bus_chip_ids[0] = bus_chip_ids[1] = 0xc5b2;
        init_production_session(&s, multibus_models[i]);
        s.locked = true;
        CHECK(find_bus(&s, error, sizeof(error)) == -ENOTUNIQ);
    }

    puts("MCU I2C resilience: proc owner races, lock wait, chip-id/ACK/write retries, and DX4600/DXP4800S bus discovery passed");
    return 0;
}
