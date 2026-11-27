#define _POSIX_C_SOURCE 200809L
#include "led/led.h"
#include "led/led_protocol.h"
#include "led/led_n76_protocol.h"
#include "led/led_sysfs.h"
#include "led/led_gpio.h"
#include "led/led_service.h"
#include "led/led_cs201x.h"
#include "core/identity.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#ifdef UGREENCTL_LED_TESTING
#include "led/led_test.h"
#endif

struct led_io {
    int (*owners)(void *opaque);
    int (*lock)(void *opaque);
    void (*unlock)(void *opaque);
    int (*read_chip_id)(void *opaque, uint16_t *chip_id);
    int (*read_status)(void *opaque, uint8_t id, uint8_t frame[11]);
    int (*write_command)(void *opaque, uint8_t id, const uint8_t frame[12]);
    int (*read_ack)(void *opaque);
    void (*pause_ms)(void *opaque, unsigned int milliseconds);
    int (*read_identity)(void *opaque,struct ugreenctl_led_identity *identity,
                         char *error,size_t size);
    int (*read_byte)(void *, uint8_t, uint8_t *);
    int (*write_byte)(void *, uint8_t, uint8_t);
};

struct ugreenctl_led_session {
    struct ugreenctl_led_identity identity;
    struct ugreenctl_led_access access;
    struct led_io io;
    void *opaque;
    int fd, lock_fd;
    bool locked;
    uint16_t chip_id;
    uint8_t n76_version;
    bool n76_failed;
    char bus[64];
    unsigned int bus_number;
    struct ugreenctl_led_sysfs *sysfs;
    struct ugreenctl_led_service_client *service;
    struct ugreenctl_led_cs201x *cs201x;
};

static int identify_authorize(void *opaque,char *error,size_t size);

static int fail(char *error, size_t error_size, int result, const char *format, ...)
{
    va_list arguments;
    if (error != NULL && error_size != 0) {
        va_start(arguments, format);
        (void)vsnprintf(error, error_size, format, arguments);
        va_end(arguments);
    }
    return result;
}

int ugreenctl_led_not_implemented(char *error, size_t error_size)
{
    return fail(error, error_size, -ENOTSUP,
                "no verified LED register map is available for this model");
}

static int read_text(const char *path, char *buffer, size_t capacity)
{
    FILE *stream;
    size_t length;
    if (capacity < 2) return -EINVAL;
    buffer[0] = '\0';
    stream = fopen(path, "r");
    if (stream == NULL) return -errno;
    if (fgets(buffer, (int)capacity, stream) == NULL) {
        int result = ferror(stream) ? -EIO : 0;
        (void)fclose(stream);
        return result;
    }
    if (ferror(stream)) { (void)fclose(stream); return -EIO; }
    (void)fclose(stream);
    length = strlen(buffer);
    while (length > 0 && isspace((unsigned char)buffer[length - 1])) buffer[--length] = '\0';
    return 0;
}

int ugreenctl_led_detect(struct ugreenctl_led_identity *identity,
                        char *error, size_t error_size)
{
    struct ugreenctl_machine_identity machine;
    int result;
    if (identity == NULL) return fail(error, error_size, -EINVAL, "missing LED identity output");
    memset(identity, 0, sizeof(*identity));
    result=ugreenctl_identity_read(&machine,error,error_size);
    if (result<0) return result;
    (void)snprintf(identity->product_name,sizeof(identity->product_name),"%s",machine.product_name);
    (void)snprintf(identity->product_version,sizeof(identity->product_version),"%s",machine.product_version);
    (void)snprintf(identity->manufacturer,sizeof(identity->manufacturer),"%s",machine.manufacturer);
    (void)snprintf(identity->board_version,sizeof(identity->board_version),"%s",machine.board_version);
    (void)snprintf(identity->source,sizeof(identity->source),"%s",machine.source);
    (void)snprintf(identity->compatible,sizeof(identity->compatible),"%s",machine.compatible);
    (void)snprintf(identity->bios_vendor,sizeof(identity->bios_vendor),"%s",machine.bios_vendor);
    (void)snprintf(identity->bios_version,sizeof(identity->bios_version),"%s",machine.bios_version);
    (void)snprintf(identity->bios_date,sizeof(identity->bios_date),"%s",machine.bios_date);
    identity->profile=ugreenctl_led_profile_find(ugreenctl_identity_model(&machine),machine.product_version);
    if (identity->profile && identity->profile->dt_compatible &&
        !ugreenctl_identity_has_compatible(&machine,identity->profile->dt_compatible)) identity->profile=NULL;
    if (identity->profile == NULL)
        return fail(error, error_size, -ENOTSUP,
                    "no LED protocol profile for exact DMI '%s' / system-version '%s'",
                    identity->product_name, identity->product_version);
    return 0;
}

static bool process_name_is_owner(const char *name, bool truncated_comm)
{
    static const char *const names[] = {
        "ugreen_leds_cli", "ugreen-diskiomon", "ugreen-netdevmon",
        "ugreen-netdevmon-multi", "ugreen-power-led", "ugreen-blink-disk",
        "ugreen_led", "ugreen-led", "ledmon", "ledctl", "nas_led", "led_ctrl",
        "hwmonitor", "hwmonitor-amd", "hwmonitor-480t"
    };
    const char *base;
    size_t i, length;
    if (name == NULL) return false;
    base = strrchr(name, '/');
    base = base == NULL ? name : base + 1;
    length = strlen(base);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(base, names[i]) == 0) return true;
        /* Linux comm truncates at 15 bytes. */
        if (truncated_comm && length == 15 && strncmp(base, names[i], length) == 0) return true;
    }
    return false;
}

static const char *process_basename(const char *name)
{
    const char *slash = strrchr(name, '/');
    return slash == NULL ? name : slash + 1;
}

/* Identify the program being executed, not arbitrary command arguments. A
 * parent `sudo ugreen_leds_cli ...` is a wrapper, not another MCU owner. */
static bool command_is_owner(const char *comm, const char *executable,
                             const char *const *arguments, size_t count)
{
    const char *program;
    size_t i;
    bool shell, python;
    if (process_name_is_owner(comm, true) || process_name_is_owner(executable, false)) return true;
    if (arguments == NULL || count == 0 || arguments[0] == NULL) return false;
    if (process_name_is_owner(arguments[0], false)) return true;
    program = process_basename(arguments[0]);
    shell = strcmp(program, "sh") == 0 || strcmp(program, "bash") == 0 ||
        strcmp(program, "dash") == 0 || strcmp(program, "ash") == 0 ||
        strcmp(program, "ksh") == 0 || strcmp(program, "zsh") == 0;
    python = strcmp(program, "python") == 0 || strcmp(program, "python3") == 0 ||
        (strncmp(program, "python3.", 8) == 0 && isdigit((unsigned char)program[8]));
    if (!shell && !python) return false;
    for (i = 1; i < count; ++i) {
        const char *argument = arguments[i];
        if (argument == NULL) return false;
        if (strcmp(argument, "--") == 0)
            return i + 1 < count && process_name_is_owner(arguments[i + 1], false);
        if (argument[0] != '-' && !(shell && argument[0] == '+'))
            return process_name_is_owner(argument, false);
        if (shell) {
            /* -c executes text and -s reads stdin: following words are not a
             * script path. Actual child controllers are inspected separately. */
            if (strcmp(argument, "--command") == 0 ||
                (argument[0] == '-' && argument[1] != '-' &&
                 (strchr(argument + 1, 'c') != NULL || strchr(argument + 1, 's') != NULL))) return false;
            if (strcmp(argument, "-o") == 0 || strcmp(argument, "+o") == 0 ||
                strcmp(argument, "--rcfile") == 0 || strcmp(argument, "--init-file") == 0) {
                if (++i >= count) return false;
            }
        } else {
            if (strcmp(argument, "-c") == 0 || strcmp(argument, "-m") == 0 ||
                strncmp(argument, "-c", 2) == 0 || strncmp(argument, "-m", 2) == 0) return false;
            if (strcmp(argument, "-W") == 0 || strcmp(argument, "-X") == 0 ||
                strcmp(argument, "--check-hash-based-pycs") == 0) {
                if (++i >= count) return false;
            }
        }
    }
    return false;
}

static int process_owners_except(pid_t authenticated_pid)
{
    DIR *directory = opendir("/proc");
    struct dirent *entry;
    int result = 0;
    if (directory == NULL) return -errno;
    while ((entry = readdir(directory)) != NULL) {
        char path[256], name[256], executable[4096] = "", *end;
        const char *arguments[128];
        long pid;
        FILE *stream;
        unsigned char command[4097];
        size_t length, position, argument_count = 0;
        ssize_t executable_length;
        if (!isdigit((unsigned char)entry->d_name[0])) continue;
        errno = 0;
        pid = strtol(entry->d_name, &end, 10);
        if (errno != 0 || *end != '\0' || pid <= 0 || pid == (long)getpid() || pid==(long)authenticated_pid) continue;
        (void)snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
        result = read_text(path, name, sizeof(name));
        if (result == -ENOENT || result == -ESRCH) { result = 0; continue; }
        if (result < 0) break;
        if (process_name_is_owner(name, true)) { result = -EBUSY; break; }
        (void)snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
        executable_length = readlink(path, executable, sizeof(executable) - 1);
        if (executable_length >= 0) executable[executable_length] = '\0';
        else if (errno != ENOENT && errno != ESRCH) { result = -errno; break; }
        (void)snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        stream = fopen(path, "rb");
        if (stream == NULL) {
            if (errno == ENOENT || errno == ESRCH) continue;
            result = -errno; break;
        }
        length = fread(command, 1, sizeof(command) - 1, stream);
        if (ferror(stream)) result = -EIO;
        (void)fclose(stream);
        if (result < 0) break;
        command[length] = '\0';
        for (position = 0; position < length && argument_count < sizeof(arguments) / sizeof(arguments[0]);) {
            arguments[argument_count++] = (const char *)command + position;
            position += strlen(arguments[argument_count - 1]) + 1;
        }
        if (command_is_owner(name, executable, arguments, argument_count)) result = -EBUSY;
        if (result < 0) break;
    }
    (void)closedir(directory);
    return result;
}

static int process_owners(void) { return process_owners_except(0); }
int ugreenctl_led_process_owners(void) { return process_owners(); }
int ugreenctl_led_process_owners_except(pid_t authenticated_pid) { return process_owners_except(authenticated_pid); }

static int production_owners(void *opaque)
{
    static const char *const modules[] = {
        "led_ugreen", "leds_mcu_28a48", "leds_mcu_68a88", "leds_mcu", "leds_mcu_n76e003",
        "leds_ugreen", "ugreen_led", "ugreen_leds", "ug_mcu", "leds_mcu_n76e003_dh2600"
    };
    struct ugreenctl_led_session *session = opaque;
    struct stat metadata;
    char path[256];
    size_t i;
    int result;
    if (!session->access.n76_direct && session->identity.profile && strncmp(session->identity.profile->protocol,"sysfs-",6)==0)
        return process_owners();
    for (i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
        (void)snprintf(path, sizeof(path), "/sys/module/%s", modules[i]);
        if (lstat(path, &metadata) == 0) return -EBUSY;
        if (errno != ENOENT) return -errno;
    }
    if (session->access.n76_direct) {
        const unsigned int addresses[] = {0x31, 0x26};
        for (i = 0; i < 2; ++i) {
            (void)snprintf(path, sizeof(path), "/sys/bus/i2c/devices/%u-%04x",
                           session->access.n76_bus, addresses[i]);
            if (lstat(path, &metadata) == 0) return -EBUSY;
            if (errno != ENOENT) return -errno;
        }
    } else if (session->bus[0] != '\0') {
        (void)snprintf(path, sizeof(path), "/sys/bus/i2c/devices/%u-003a/driver", session->bus_number);
        if (lstat(path, &metadata) == 0) return -EBUSY;
        if (errno != ENOENT) return -errno;
    }
    result = process_owners();
    return result;
}

static int production_lock(void *opaque)
{
    struct ugreenctl_led_session *session = opaque;
    struct stat metadata;
    int result;
    session->lock_fd = open("/run/ugreenctl-led-mcu.lock",
                            O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (session->lock_fd < 0) return -errno;
    if (fstat(session->lock_fd, &metadata) < 0) return -errno;
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != 0 || metadata.st_nlink != 1)
        return -EPERM;
    result = flock(session->lock_fd, LOCK_EX | LOCK_NB);
    if (result < 0) return errno == EWOULDBLOCK ? -EBUSY : -errno;
    return 0;
}

static void production_unlock(void *opaque)
{
    struct ugreenctl_led_session *session = opaque;
    if (session->lock_fd >= 0) {
        (void)flock(session->lock_fd, LOCK_UN);
        (void)close(session->lock_fd);
        session->lock_fd = -1;
    }
}

static int smbus(struct ugreenctl_led_session *session, uint8_t direction,
                 uint8_t command, int size, union i2c_smbus_data *data)
{
    struct i2c_smbus_ioctl_data transaction;
    transaction.read_write = direction;
    transaction.command = command;
    transaction.size = (uint32_t)size;
    transaction.data = data;
    if (ioctl(session->fd, I2C_SMBUS, &transaction) < 0) return -errno;
    return 0;
}

static int production_read_byte(void *opaque, uint8_t reg, uint8_t *value)
{
    struct ugreenctl_led_session *session = opaque;
    union i2c_smbus_data data = {0};
    struct timespec delay = {0, 1000000L};
    int result;
    /* Recheck kernel ownership, never use I2C_SLAVE_FORCE. */
    if (ioctl(session->fd, I2C_SLAVE, session->access.n76_address) < 0) return -errno;
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) { }
    result = smbus(session, I2C_SMBUS_READ, reg, I2C_SMBUS_BYTE_DATA, &data);
    if (!result) *value = data.byte;
    return result;
}

static int production_write_byte(void *opaque, uint8_t reg, uint8_t value)
{
    struct ugreenctl_led_session *session = opaque;
    union i2c_smbus_data data = {0};
    if (ioctl(session->fd, I2C_SLAVE, session->access.n76_address) < 0) return -errno;
    data.byte = value;
    return smbus(session, I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE_DATA, &data);
}

static int production_chip_id(void *opaque, uint16_t *chip_id)
{
    struct ugreenctl_led_session *session = opaque;
    union i2c_smbus_data data;
    int result;
    if (session->access.n76_direct) {
        uint8_t high, low;
        result = session->io.read_byte(session->opaque, 0x5a, &high);
        if (result < 0) return result;
        result = session->io.read_byte(session->opaque, 0x5b, &low);
        if (!result) *chip_id = (uint16_t)((unsigned int)high << 8 | low);
        return result;
    }
    memset(&data, 0, sizeof(data));
    result = smbus(session, I2C_SMBUS_READ, session->identity.profile->chip_register,
                    I2C_SMBUS_WORD_DATA, &data);
    if (result == 0) *chip_id = data.word;
    return result;
}

static int production_read(void *opaque, uint8_t id, uint8_t frame[11])
{
    struct ugreenctl_led_session *session = opaque;
    union i2c_smbus_data data;
    int result;
    memset(&data, 0, sizeof(data));
    data.block[0] = UGREENCTL_LED_FRAME_SIZE;
    result = smbus(session, I2C_SMBUS_READ, (uint8_t)(0x81 + id),
                    I2C_SMBUS_I2C_BLOCK_DATA, &data);
    if (result < 0) return result;
    if (data.block[0] != UGREENCTL_LED_FRAME_SIZE) return -EBADMSG;
    memcpy(frame, data.block + 1, UGREENCTL_LED_FRAME_SIZE);
    return 0;
}

static int production_write(void *opaque, uint8_t id, const uint8_t frame[12])
{
    struct ugreenctl_led_session *session = opaque;
    union i2c_smbus_data data;
    memset(&data, 0, sizeof(data));
    if (strncmp(session->identity.profile->protocol, "smbus-block-", 12) == 0) {
        data.block[0] = 11;
        memcpy(data.block + 1, frame, 11);
        return smbus(session, I2C_SMBUS_WRITE, id, I2C_SMBUS_BLOCK_DATA, &data);
    }
    data.block[0] = UGREENCTL_LED_COMMAND_SIZE;
    memcpy(data.block + 1, frame, UGREENCTL_LED_COMMAND_SIZE);
    return smbus(session, I2C_SMBUS_WRITE, id, I2C_SMBUS_I2C_BLOCK_DATA, &data);
}

static int production_ack(void *opaque)
{
    struct ugreenctl_led_session *session = opaque;
    union i2c_smbus_data data;
    int result;
    memset(&data, 0, sizeof(data));
    result = smbus(session, I2C_SMBUS_READ, 0x80, I2C_SMBUS_BYTE_DATA, &data);
    if (result < 0) return result;
    return data.byte == 1 ? 0 : -EIO;
}

static void production_pause(void *opaque, unsigned int milliseconds)
{
    struct timespec delay;
    (void)opaque;
    delay.tv_sec = (time_t)(milliseconds / 1000);
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) { }
}

static int production_identity(void *opaque,struct ugreenctl_led_identity *identity,char *error,size_t size)
{
    (void)opaque;return ugreenctl_led_detect(identity,error,size);
}

static const struct led_io production_io = {
    production_owners, production_lock, production_unlock,
    production_chip_id, production_read, production_write,
    production_ack, production_pause, production_identity, production_read_byte, production_write_byte
};

static int verify_session_identity(struct ugreenctl_led_session *session,char *error,size_t size)
{
    struct ugreenctl_led_identity current;
    int result;
    if (!session->io.read_identity) {
#ifdef UGREENCTL_LED_TESTING
        return 0; /* Explicit fixtures may isolate the transport under test. */
#else
        return fail(error,size,-ENOTSUP,"LED machine identity reader is unavailable");
#endif
    }
    result=session->io.read_identity(session->opaque,&current,error,size);
    if (result<0) return result;
    if (!ugreenctl_led_same_identity(&session->identity,&current))
        return fail(error,size,-ESTALE,"LED machine, board or BIOS identity changed; reopen and revalidate");
    return 0;
}

/* Only enumerate the adapter family documented by each current profile.
 * Do not probe arbitrary devices, issue SMBus quick commands or guess bus 1. */
static int find_bus(struct ugreenctl_led_session *session, char *error, size_t error_size)
{
    DIR *directory = opendir("/sys/class/i2c-dev");
    struct dirent *entry;
    unsigned int found = 0;
    int result = 0;
    if (directory == NULL)
        return fail(error, error_size, -errno,
                    "i2c-dev is unavailable; load/configure it on the host before using LEDs");
    while ((entry = readdir(directory)) != NULL) {
        char path[512], name[256], tail;
        unsigned int number;
        if (sscanf(entry->d_name, "i2c-%u%c", &number, &tail) != 1) continue;
        (void)snprintf(path, sizeof(path), "/sys/class/i2c-dev/%s/name", entry->d_name);
        result = read_text(path, name, sizeof(name));
        if (result == -ENOENT) {
            (void)snprintf(path, sizeof(path), "/sys/class/i2c-dev/%s/device/name", entry->d_name);
            result = read_text(path, name, sizeof(name));
        }
        if (result < 0) break;
        if (session->identity.profile->bus_prefix == NULL ||
            strncmp(name, session->identity.profile->bus_prefix,
                    strlen(session->identity.profile->bus_prefix)) != 0) continue;
        ++found;
        session->bus_number = number;
        (void)snprintf(session->bus, sizeof(session->bus), "/dev/i2c-%u", number);
    }
    (void)closedir(directory);
    if (result < 0) return fail(error, error_size, result, "cannot inspect I2C adapters: %s", strerror(-result));
    if (found != 1) return fail(error, error_size, -ENOTSUP,
                               "expected exactly one documented '%s' adapter, found %u",
                               session->identity.profile->bus_prefix, found);
    result = session->io.owners(session->opaque);
    if (result < 0) return fail(error, error_size, result, "LED controller has a driver/process owner or ownership is unreadable");
    session->fd = open(session->bus, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (session->fd < 0) return fail(error, error_size, -errno, "cannot open %s: %s", session->bus, strerror(errno));
    {
        unsigned long functions = 0;
        const unsigned long required = I2C_FUNC_SMBUS_READ_I2C_BLOCK |
            (strncmp(session->identity.profile->protocol,"smbus-block-",12)==0 ?
             I2C_FUNC_SMBUS_WRITE_BLOCK_DATA : I2C_FUNC_SMBUS_WRITE_I2C_BLOCK) |
            I2C_FUNC_SMBUS_READ_BYTE_DATA |
            I2C_FUNC_SMBUS_READ_WORD_DATA;
        if (ioctl(session->fd, I2C_FUNCS, &functions) < 0)
            return fail(error, error_size, -errno, "cannot query SMBus capabilities: %s", strerror(errno));
        if ((functions & required) != required)
            return fail(error, error_size, -ENOTSUP, "I2C adapter lacks the documented SMBus operations");
    }
    /* I2C_SLAVE deliberately refuses a kernel-owned client. Never FORCE. */
    if (ioctl(session->fd, I2C_SLAVE, session->identity.profile->i2c_address) < 0)
        return fail(error, error_size, -errno, "cannot claim LED address 0x3a: %s", strerror(errno));
    return 0;
}

static int acquire(struct ugreenctl_led_session *session, char *error, size_t error_size)
{
    int result = session->io.owners(session->opaque);
    if (result < 0) return fail(error, error_size, result, "LED controller has another driver/process owner or ownership is unreadable");
    result = session->io.lock(session->opaque);
    if (result < 0) return fail(error, error_size, result, "cannot acquire fixed LED controller lock: %s", strerror(-result));
    session->locked = true;
    return 0;
}

static bool n76_profile(const struct ugreenctl_led_profile *profile)
{
    return profile && !strcmp(profile->protocol, "sysfs-n76") &&
        ((!strcmp(profile->id, "dh2600-n76") && !strcmp(profile->product_name, "DH2600")) ||
         (!strcmp(profile->id, "dxp480t-n76") && !strcmp(profile->product_name, "DXP480T Plus")));
}

static int n76_probe(struct ugreenctl_led_session *session, char *error, size_t size)
{
    int result = verify_session_identity(session, error, size);
    if (!result) result = session->io.owners(session->opaque);
    if (!result) result = session->io.read_chip_id(session->opaque, &session->chip_id);
    if (!result && session->chip_id != 0xa5b5) result = -ENODEV;
    if (!result) result = session->io.read_byte(session->opaque, 0x5d, &session->n76_version);
    return result < 0 ? fail(error, size, result, "N76 identity/version probe failed; no writes performed") : 0;
}

static int n76_open_bus(struct ugreenctl_led_session *session, char *error, size_t size)
{
    unsigned long functions = 0;
    struct stat st;
    const unsigned long required = I2C_FUNC_SMBUS_READ_BYTE_DATA | I2C_FUNC_SMBUS_WRITE_BYTE_DATA;
    session->bus_number = session->access.n76_bus;
    (void)snprintf(session->bus, sizeof(session->bus), "/dev/i2c-%u", session->bus_number);
    session->fd = open(session->bus, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (session->fd < 0) return fail(error, size, -errno, "cannot open explicit N76 I2C bus");
    if (fstat(session->fd, &st) < 0) return -errno;
    if (!S_ISCHR(st.st_mode) || major(st.st_rdev) != 89 || minor(st.st_rdev) != session->bus_number)
        return fail(error, size, -ENODEV, "N76 bus must be the requested Linux i2c-dev character device");
    if (ioctl(session->fd, I2C_FUNCS, &functions) < 0) return -errno;
    if ((functions & required) != required) return fail(error, size, -ENOTSUP, "N76 requires SMBus byte-data operations");
    return n76_probe(session, error, size);
}

static int probe(struct ugreenctl_led_session *session, char *error, size_t error_size)
{
    struct ugreenctl_led_value value;
    int result;
    /* An absent ID is allowed for read-only diagnosis; it never permits writes. */
    session->chip_id = 0;
    result = session->io.read_chip_id(session->opaque, &session->chip_id);
    if (result < 0) session->chip_id = 0;
    result = ugreenctl_led_read(session, 0, &value, error, error_size);
    if (result < 0) return result;
    return ugreenctl_led_read(session, 1, &value, error, error_size);
}

int ugreenctl_led_open(const struct ugreenctl_led_access *access,
                      struct ugreenctl_led_session **output,
                      char *error, size_t error_size)
{
    struct ugreenctl_led_session *session;
    int result;
    if (access == NULL || output == NULL) return fail(error, error_size, -EINVAL, "missing LED access arguments");
    *output = NULL;
    session = calloc(1, sizeof(*session));
    if (session == NULL) return -ENOMEM;
    session->fd = -1; session->lock_fd = -1;
    session->access = *access; session->io = production_io; session->opaque = session;
    result = ugreenctl_led_detect(&session->identity, error, error_size);
    if (result == 0 && access->n76_direct &&
        (!n76_profile(session->identity.profile) || access->n76_bus > 65535 ||
         (access->n76_address != 0x31 && access->n76_address != 0x26)))
        result = fail(error, error_size, -ENOTSUP, "direct N76 requires exact DH2600/DXP480T Plus and explicit BUS:0x31 or BUS:0x26");
    if (result==0 && (!strcmp(session->identity.profile->protocol,"cs201x-sio") ||
                       !strcmp(session->identity.profile->protocol,"sysfs-gpio"))) {
        result=ugreenctl_led_service_connect(&session->identity,access,&session->service,error,error_size);
        if (result==0) {
            (void)snprintf(session->bus,sizeof(session->bus),"%s:ugreenctl-ledd",
                !strcmp(session->identity.profile->protocol,"cs201x-sio") ? "cs201x" : "gpio");
            if (!strcmp(session->identity.profile->protocol,"cs201x-sio")) session->chip_id=session->identity.profile->expected_chip_id;
            *output=session;return 0;
        }
        /* A present but unavailable/rejected holder must never be bypassed. */
        if (result==-ENOENT) { result=0;if (error && error_size) error[0]=0; }
    }
    if (result==0 && !strcmp(session->identity.profile->protocol,"cs201x-sio")) {
        result=ugreenctl_led_cs201x_open(access,&session->cs201x,error,error_size);
        if (result<0) { ugreenctl_led_close(session);return result; }
        session->identity=*ugreenctl_led_cs201x_identity(session->cs201x);
        session->chip_id=ugreenctl_led_cs201x_chip_id(session->cs201x);
        (void)snprintf(session->bus,sizeof(session->bus),"%s",ugreenctl_led_cs201x_bus(session->cs201x));
        *output=session;return 0;
    }
    if (result == 0) result = acquire(session, error, error_size);
    if (result == 0 && session->access.n76_direct) {
        result = n76_open_bus(session, error, error_size);
    } else if (result == 0 && strncmp(session->identity.profile->protocol,"sysfs-",6)==0) {
        result=ugreenctl_led_sysfs_open(session->identity.profile,"/sys",identify_authorize,session,&session->sysfs,error,error_size);
        if (result==0) (void)snprintf(session->bus,sizeof(session->bus),"sysfs:%s",session->identity.profile->sysfs_driver);
        else if (result==-ENOTSUP && !strcmp(session->identity.profile->protocol,"sysfs-gpio"))
            (void)fail(error,error_size,result,"verified GPIO LED driver is unavailable; run the explicit ugreenctl-ledd holder for protected GPIO character-device support");
    } else {
        if (result == 0) result = find_bus(session, error, error_size);
        if (result == 0) result = probe(session, error, error_size);
    }
    if (result < 0) { ugreenctl_led_close(session); return result; }
    *output = session;
    return 0;
}

const struct ugreenctl_led_identity *ugreenctl_led_identity(const struct ugreenctl_led_session *session)
{
    return session == NULL ? NULL : &session->identity;
}

uint16_t ugreenctl_led_chip_id(const struct ugreenctl_led_session *session)
{
    return session == NULL ? 0 : session->chip_id;
}

bool ugreenctl_led_identity_confirmed(const struct ugreenctl_led_session *session)
{
    if (session && session->access.n76_direct)
        return session->locked && !session->n76_failed && n76_profile(session->identity.profile) && session->chip_id == 0xa5b5;
    if (session && session->cs201x) return ugreenctl_led_cs201x_identity_confirmed(session->cs201x);
    if (session && session->service) return ugreenctl_led_service_identity_confirmed(session->service);
    if (session && session->sysfs) return ugreenctl_led_sysfs_identity_confirmed(session->sysfs);
    return session != NULL && session->locked && session->identity.profile != NULL &&
        session->chip_id != 0 && session->chip_id == session->identity.profile->expected_chip_id &&
        (strcmp(session->identity.profile->protocol,"legacy-i2c-block")==0 ||
         strcmp(session->identity.profile->protocol,"smbus-block-be")==0 ||
         strcmp(session->identity.profile->protocol,"smbus-block-le")==0);
}

static const struct ugreenctl_led_descriptor *find_target(const struct ugreenctl_led_session *session,
                                                         uint8_t id)
{
    size_t i;
    if (session == NULL || session->identity.profile == NULL) return NULL;
    for (i = 0; i < session->identity.profile->led_count; ++i)
        if (session->identity.profile->leds[i].id == id) return &session->identity.profile->leds[i];
    return NULL;
}

const char *ugreenctl_led_bus(const struct ugreenctl_led_session *session)
{
    return session == NULL ? "" : session->bus;
}

int ugreenctl_led_read(struct ugreenctl_led_session *session, uint8_t id,
                      struct ugreenctl_led_value *value,
                      char *error, size_t error_size)
{
    uint8_t frame[UGREENCTL_LED_FRAME_SIZE];
    int result = -EBADMSG;
    unsigned int attempt;
    if (session == NULL || (!session->locked && !session->service && !session->cs201x) || value == NULL ||
        find_target(session,id) == NULL)
        return fail(error, error_size, -EINVAL, "LED target is absent from the exact model profile");
    result=verify_session_identity(session,error,error_size);
    if (result<0) return result;
    if (session->cs201x) return ugreenctl_led_cs201x_read(session->cs201x,id,value,error,error_size);
    if (session->service) return ugreenctl_led_service_read(session->service,id,value,error,error_size);
    result = session->io.owners(session->opaque);
    if (result < 0) return fail(error, error_size, result, "LED controller ownership changed or is unreadable");
    if (session->sysfs) return ugreenctl_led_sysfs_read(session->sysfs,id,value,error,error_size);
    if (session->access.n76_direct) {
        uint16_t chip = 0;
        uint8_t version = 0;
        unsigned int bit = id == 1 ? 4 : id == 2 ? 2 : 0;
        memset(value, 0, sizeof(*value));
        value->unknown_fields = UGREENCTL_LED_PATCH_MODE | UGREENCTL_LED_PATCH_RGB |
            UGREENCTL_LED_PATCH_COLOR | UGREENCTL_LED_PATCH_BRIGHTNESS | UGREENCTL_LED_PATCH_TIMING;
        result = session->io.read_chip_id(session->opaque, &chip);
        if (!result) result = session->io.read_byte(session->opaque, 0x5d, &version);
        if (!result && (chip != session->chip_id || chip != 0xa5b5 || version != session->n76_version)) result = -ESTALE;
        if (!result) result = session->io.read_byte(session->opaque, (uint8_t)(0x50 + bit), &value->n76_modes[0]);
        if (!result) result = session->io.read_byte(session->opaque, (uint8_t)(0x51 + bit), &value->n76_modes[1]);
        if (!result && (value->n76_modes[0] > 3 || value->n76_modes[1] > 3)) result = -EBADMSG;
        if (result < 0) return fail(error, error_size, result, "N76 identity/version/mode read failed; actual outputs remain unknown");
        value->n76_mode_valid = true;
        value->n76_version = version;
        return 0;
    }
    memset(value, 0, sizeof(*value));
    for (attempt = 0; attempt < 3; ++attempt) {
        session->io.pause_ms(session->opaque, attempt == 0 ? 8 : 3);
        result = session->io.read_status(session->opaque, id, frame);
        if (result == 0) result = ugreenctl_led_decode(frame, sizeof(frame), value);
        if (result == 0) return 0;
        if (result == -EBUSY || result == -EACCES || result == -EPERM) break;
    }
    return fail(error, error_size, result, "LED %u status read/validation failed: %s", id, strerror(-result));
}

static int authorize_write(struct ugreenctl_led_session *session, char *error, size_t error_size)
{
    uint16_t current = 0;
    int result;
    if (session == NULL || (!session->locked && !session->service && !session->cs201x)) return fail(error, error_size, -EINVAL, "LED session is not locked");
    if (!session->access.apply || (session->identity.profile->experimental && !session->access.force))
        return fail(error, error_size, -EPERM, "experimental LED writes require --force --apply");
    result=verify_session_identity(session,error,error_size);
    if (result<0) return result;
    if (session->cs201x)
        return ugreenctl_led_cs201x_identity_confirmed(session->cs201x) ? 0 :
            fail(error,error_size,-ENOTSUP,"CS201X identity is unavailable");
    if (session->service)
        return ugreenctl_led_service_identity_confirmed(session->service) ? 0 :
            fail(error,error_size,-ENOTSUP,"LED holder identity is unavailable");
    result = session->io.owners(session->opaque);
    if (result < 0) return fail(error, error_size, result, "LED controller ownership changed or is unreadable; write refused");
    if (session->sysfs)
        return ugreenctl_led_sysfs_identity_confirmed(session->sysfs) ? 0 :
            fail(error,error_size,-ENOTSUP,"LED driver binding identity changed; write refused");
    result = session->io.read_chip_id(session->opaque, &current);
    if (result < 0 || current == 0 || session->chip_id == 0)
        return fail(error, error_size, -ENOTSUP, "LED chip identity unavailable; writes refused even with --force");
    if (current != session->identity.profile->expected_chip_id || current != session->chip_id)
        return fail(error, error_size, -ENOTSUP, "LED chip identity 0x%04x does not match the model's expected MCU family", current);
    if (session->access.n76_direct) {
        uint8_t version = 0;
        if (session->n76_failed) return fail(error, error_size, -EIO, "N76 session failed; reopen after inspecting unknown output state");
        result = session->io.read_byte(session->opaque, 0x5d, &version);
        if (result < 0 || version != session->n76_version)
            return fail(error, error_size, result < 0 ? result : -ESTALE, "N76 MCU version changed or is unreadable");
        return 0;
    }
    if (strcmp(session->identity.profile->protocol, "legacy-i2c-block") != 0 &&
        strcmp(session->identity.profile->protocol, "smbus-block-be") != 0 &&
        strcmp(session->identity.profile->protocol, "smbus-block-le") != 0)
        return fail(error, error_size, -ENOTSUP, "LED write protocol has not been implemented for this exact profile");
    return 0;
}

static int change(struct ugreenctl_led_session *session, uint8_t id,
                  uint8_t command, const uint8_t parameters[4],
                  const struct ugreenctl_led_patch *expected,
                  struct ugreenctl_led_value *readback,
                  char *error, size_t error_size)
{
    uint8_t frame[UGREENCTL_LED_COMMAND_SIZE];
    const struct ugreenctl_led_value *wanted = &expected->value;
    int result = authorize_write(session, error, error_size);
    if (result < 0) return result;
    if (strncmp(session->identity.profile->protocol,"smbus-block-",12)==0)
        result = ugreenctl_led_encode_smbus(command,parameters,
            strcmp(session->identity.profile->protocol,"smbus-block-le")==0,frame);
    else result = ugreenctl_led_encode(id, command, parameters, frame);
    if (result < 0) return result;
    session->io.pause_ms(session->opaque, 1);
    result = session->io.write_command(session->opaque, id, frame);
    if (result < 0) return fail(error, error_size, result, "LED command write failed; state may have changed: %s", strerror(-result));
    session->io.pause_ms(session->opaque, 2);
    result = session->io.read_ack(session->opaque);
    if (result < 0) return fail(error, error_size, result, "LED MCU did not acknowledge command; state may have changed");
    result = ugreenctl_led_read(session, id, readback, error, error_size);
    if (result < 0) return result;
    if (((expected->fields & UGREENCTL_LED_PATCH_RGB) &&
         (readback->red != wanted->red || readback->green != wanted->green || readback->blue != wanted->blue)) ||
        ((expected->fields & UGREENCTL_LED_PATCH_BRIGHTNESS) && readback->brightness != wanted->brightness) ||
        ((expected->fields & UGREENCTL_LED_PATCH_MODE) && readback->mode != wanted->mode) ||
        ((expected->fields & UGREENCTL_LED_PATCH_TIMING) &&
         (readback->on_ms != wanted->on_ms || readback->off_ms != wanted->off_ms)))
        return fail(error, error_size, -EIO, "LED %u target fields did not match after writing; controller may use another protocol", id);
    return 0;
}

int ugreenctl_led_preflight(struct ugreenctl_led_session *session, uint8_t id,
                           const struct ugreenctl_led_value *before,
                           const struct ugreenctl_led_patch *patch,
                           char *error, size_t error_size)
{
    const struct ugreenctl_led_descriptor *target=find_target(session,id);
    if (!target || !before)
        return fail(error,error_size,-EINVAL,"LED preflight requires a modeled target and prior state");
    if (session->access.n76_direct) {
        struct ugreenctl_n76_plan plan;
        return ugreenctl_n76_plan(session->identity.profile, id, patch, &plan, error, error_size);
    }
    if (session->sysfs)
        return ugreenctl_led_sysfs_preflight(session->sysfs,id,before,patch,error,error_size);
    return ugreenctl_led_validate_state_patch(session->identity.profile,target,before,patch,error,error_size);
}

int ugreenctl_led_apply(struct ugreenctl_led_session *session, uint8_t id,
                       const struct ugreenctl_led_patch *patch,
                       struct ugreenctl_led_value *readback,
                       char *error, size_t error_size)
{
    struct ugreenctl_led_value local;
    struct ugreenctl_led_patch expected;
    uint8_t parameters[4] = {0};
    int result;
    if (find_target(session,id) == NULL)
        return fail(error,error_size,-EINVAL,"LED target is absent from the exact model profile");
    result = ugreenctl_led_validate_target_patch(find_target(session,id), patch, error, error_size);
    if (result < 0) return result;
    result = authorize_write(session, error, error_size);
    if (result < 0) return result;
    if (session->access.n76_direct) {
        struct ugreenctl_n76_plan plan;
        size_t i;
        result = ugreenctl_n76_plan(session->identity.profile, id, patch, &plan, error, error_size);
        if (result < 0) return result;
        result = ugreenctl_led_read(session, id, &local, error, error_size);
        if (result < 0) return result;
        for (i = 0; i < plan.count; ++i) {
            uint8_t actual;
            result = authorize_write(session, error, error_size);
            if (result < 0) break;
            result = session->io.write_byte(session->opaque, plan.commands[i].reg, plan.commands[i].value);
            if (result < 0) break;
            session->io.pause_ms(session->opaque, plan.commands[i].settle_ms);
            if (plan.commands[i].reg == plan.mode_registers[0] || plan.commands[i].reg == plan.mode_registers[1]) {
                result = session->io.read_byte(session->opaque, plan.commands[i].reg, &actual);
                if (!result && actual != plan.commands[i].value) result = -EIO;
                if (result < 0) break;
            }
        }
        if (!result) result = ugreenctl_led_read(session, id, &local, error, error_size);
        if (!result && (local.n76_modes[0] != plan.expected_modes[0] || local.n76_modes[1] != plan.expected_modes[1])) result = -EIO;
        if (result < 0) {
            session->n76_failed = true;
            return fail(error, error_size, result, "N76 transaction failed; outputs may have changed and cannot be restored from unknown state");
        }
        local.unverified_write = true;
        if (readback) *readback = local;
        return 0;
    }
    if (session->cs201x) return ugreenctl_led_cs201x_apply(session->cs201x,id,patch,readback,error,error_size);
    if (session->service) return ugreenctl_led_service_apply(session->service,id,patch,readback,error,error_size);
    if (session->sysfs) return ugreenctl_led_sysfs_apply(session->sysfs,id,patch,readback,error,error_size);
    /* Fail on unreadable target before issuing the first write. */
    result = ugreenctl_led_read(session, id, &local, error, error_size);
    if (result < 0) return result;
    if (readback == NULL) readback = &local;
    memset(&expected, 0, sizeof(expected));
    expected.value = patch->value;
    if (patch->fields & UGREENCTL_LED_PATCH_RGB) {
        parameters[0] = (uint8_t)patch->value.red;
        parameters[1] = (uint8_t)patch->value.green;
        parameters[2] = (uint8_t)patch->value.blue;
        expected.fields = UGREENCTL_LED_PATCH_RGB;
        result = change(session, id, 2, parameters, &expected, readback, error, error_size);
        if (result < 0) return result;
    }
    if (patch->fields & UGREENCTL_LED_PATCH_BRIGHTNESS) {
        memset(parameters, 0, sizeof(parameters));
        parameters[0] = (uint8_t)patch->value.brightness;
        expected.fields = UGREENCTL_LED_PATCH_BRIGHTNESS;
        result = change(session, id, 1, parameters, &expected, readback, error, error_size);
        if (result < 0) return result;
    }
    if (patch->fields & UGREENCTL_LED_PATCH_MODE) {
        uint8_t command = 3;
        memset(parameters, 0, sizeof(parameters));
        expected.fields = UGREENCTL_LED_PATCH_MODE;
        if (patch->value.mode <= UGREENCTL_LED_MODE_ON) {
            parameters[0] = (uint8_t)patch->value.mode;
        } else {
            unsigned int cycle = patch->value.on_ms + patch->value.off_ms;
            command = patch->value.mode == UGREENCTL_LED_MODE_BLINK ? 4 : 5;
            parameters[0] = (uint8_t)(cycle >> 8); parameters[1] = (uint8_t)cycle;
            parameters[2] = (uint8_t)(patch->value.on_ms >> 8);
            parameters[3] = (uint8_t)patch->value.on_ms;
            expected.fields |= UGREENCTL_LED_PATCH_TIMING;
        }
        result = change(session, id, command, parameters, &expected, readback, error, error_size);
        if (result < 0) return result;
    }
    /* A later command must not silently undo an earlier requested field. */
    if (((patch->fields & UGREENCTL_LED_PATCH_RGB) &&
         (readback->red != patch->value.red || readback->green != patch->value.green || readback->blue != patch->value.blue)) ||
        ((patch->fields & UGREENCTL_LED_PATCH_BRIGHTNESS) && readback->brightness != patch->value.brightness) ||
        ((patch->fields & UGREENCTL_LED_PATCH_MODE) && readback->mode != patch->value.mode) ||
        ((patch->fields & UGREENCTL_LED_PATCH_TIMING) &&
         (readback->on_ms != patch->value.on_ms || readback->off_ms != patch->value.off_ms)))
        return fail(error, error_size, -EIO, "LED %u final state did not retain all requested fields", id);
    return 0;
}

static int identify_authorize(void *opaque,char *error,size_t size)
{
    return authorize_write(opaque,error,size);
}
static void identify_pause(void *opaque,unsigned int milliseconds)
{
    struct ugreenctl_led_session *session=opaque;
    session->io.pause_ms(session->opaque,milliseconds);
}
static int identify_binary(struct ugreenctl_led_session *session,uint8_t id,unsigned int seconds,
    const volatile sig_atomic_t *cancelled,char *error,size_t size)
{
    struct ugreenctl_led_value saved,current;
    struct ugreenctl_led_patch patch={0};
    char restore_error[256]="";unsigned int tick;int result,restored;
    result=ugreenctl_led_read(session,id,&saved,error,size);if (result<0) return result;
    if ((saved.unknown_fields&(UGREENCTL_LED_PATCH_MODE|UGREENCTL_LED_PATCH_BRIGHTNESS)) ||
        (saved.mode==UGREENCTL_LED_MODE_ON && (saved.unknown_fields&UGREENCTL_LED_PATCH_COLOR)))
        return fail(error,size,-ENOTSUP,"identify cannot restore unknown original binary LED outputs");
    if (saved.mode!=UGREENCTL_LED_MODE_ON && saved.mode!=UGREENCTL_LED_MODE_OFF)
        return fail(error,size,-ENOTSUP,"binary identify requires a restorable steady LED state");
    for (tick=0;tick<seconds*4U;++tick) {
        patch.fields=UGREENCTL_LED_PATCH_MODE;patch.value.mode=tick&1U ? UGREENCTL_LED_MODE_OFF : UGREENCTL_LED_MODE_ON;
        if (!(tick&1U)) { patch.fields|=UGREENCTL_LED_PATCH_COLOR;patch.value.color=saved.mode==UGREENCTL_LED_MODE_ON ? saved.color : UGREENCTL_LED_COLOR_WHITE; }
        result=ugreenctl_led_apply(session,id,&patch,&current,error,size);if (result<0) break;
        if (cancelled && *cancelled) break;
        identify_pause(session,250);
    }
    patch.fields=UGREENCTL_LED_PATCH_MODE;patch.value=saved;
    if (saved.mode==UGREENCTL_LED_MODE_ON) patch.fields|=UGREENCTL_LED_PATCH_COLOR;
    restored=ugreenctl_led_apply(session,id,&patch,&current,restore_error,sizeof(restore_error));
    if (restored<0) return fail(error,size,restored,"identify restoration failed: %s",restore_error);
    if (current.mode!=saved.mode || current.brightness!=saved.brightness ||
        (saved.mode==UGREENCTL_LED_MODE_ON && current.color!=saved.color))
        return fail(error,size,-EIO,"identify restoration did not match original binary outputs");
    return result;
}

int ugreenctl_led_identify(struct ugreenctl_led_session *session, uint8_t id,
                          unsigned int seconds,
                          const volatile sig_atomic_t *cancelled,
                          char *error, size_t error_size)
{
    struct ugreenctl_led_value original, current;
    struct ugreenctl_led_patch patch, restore;
    char restore_error[256] = "";
    unsigned int tick;
    int result, restored;
    if (seconds < 1 || seconds > 60) return fail(error, error_size, -EINVAL, "identify duration must be 1..60 seconds");
    result = authorize_write(session, error, error_size);
    if (result < 0) return result;
    if (find_target(session,id)==NULL) return fail(error,error_size,-EINVAL,"LED target is absent from the exact model profile");
    if (session->cs201x) return ugreenctl_led_cs201x_identify(session->cs201x,id,seconds,cancelled,error,error_size);
    if (session->service && !strcmp(session->identity.profile->protocol,"cs201x-sio"))
        return ugreenctl_led_service_identify(session->service,id,seconds,cancelled,error,error_size);
    if (session->sysfs) return ugreenctl_led_sysfs_identify(session->sysfs,id,seconds,cancelled,
        identify_authorize,identify_pause,session,error,error_size);
    if (!find_target(session,id)->rgb) return identify_binary(session,id,seconds,cancelled,error,error_size);
    result = ugreenctl_led_read(session, id, &original, error, error_size);
    if (result < 0) return result;
    memset(&patch, 0, sizeof(patch));
    patch.fields = UGREENCTL_LED_PATCH_RGB | UGREENCTL_LED_PATCH_BRIGHTNESS |
        UGREENCTL_LED_PATCH_MODE | UGREENCTL_LED_PATCH_TIMING;
    patch.value.mode = UGREENCTL_LED_MODE_BLINK;
    patch.value.red = 255; patch.value.green = 100;
    patch.value.brightness = 255; patch.value.on_ms = 250; patch.value.off_ms = 250;
    result = ugreenctl_led_apply(session, id, &patch, &current, error, error_size);
    if (result == 0) {
        for (tick = 0; tick < seconds * 10U; ++tick) {
            if (cancelled != NULL && *cancelled) break;
            session->io.pause_ms(session->opaque, 100);
        }
    }
    /* Restore even after a partial/uncertain identify write. Never bypass a
     * new owner or a changed identity to perform that restoration. */
    memset(&restore, 0, sizeof(restore));
    restore.fields = UGREENCTL_LED_PATCH_RGB | UGREENCTL_LED_PATCH_BRIGHTNESS |
        UGREENCTL_LED_PATCH_MODE | UGREENCTL_LED_PATCH_TIMING;
    restore.value = original;
    /* Timing remains visible in status even in steady modes. Restore those
     * dormant fields before switching back to the original steady mode. */
    if (original.mode == UGREENCTL_LED_MODE_OFF || original.mode == UGREENCTL_LED_MODE_ON)
        restore.value.mode = UGREENCTL_LED_MODE_BLINK;
    restored = ugreenctl_led_apply(session, id, &restore, &current, restore_error, sizeof(restore_error));
    if (restored < 0) return fail(error, error_size, restored, "identify restoration failed: %s", restore_error);
    if (restore.value.mode != original.mode) {
        restore.fields = UGREENCTL_LED_PATCH_MODE;
        restore.value.mode = original.mode;
        restored = ugreenctl_led_apply(session, id, &restore, &current, restore_error, sizeof(restore_error));
        if (restored < 0) return fail(error, error_size, restored, "identify restoration failed: %s", restore_error);
    }
    if (current.mode != original.mode || current.brightness != original.brightness ||
        current.red != original.red || current.green != original.green || current.blue != original.blue ||
        current.on_ms != original.on_ms || current.off_ms != original.off_ms)
        return fail(error, error_size, -EIO, "identify restoration did not match the saved full state");
    return result;
}

void ugreenctl_led_close(struct ugreenctl_led_session *session)
{
    if (session == NULL) return;
    ugreenctl_led_cs201x_close(session->cs201x);
    ugreenctl_led_service_close(session->service);
    ugreenctl_led_sysfs_close(session->sysfs);
    if (session->fd >= 0) (void)close(session->fd);
    if (session->locked || session->lock_fd >= 0) session->io.unlock(session->opaque);
    free(session);
}

#ifdef UGREENCTL_LED_TESTING
void ugreenctl_led_test_sysfs_observer(struct ugreenctl_led_session *session,
    void (*observer)(void *,const char *,const char *),void *opaque)
{
    ugreenctl_led_sysfs_test_observer(session->sysfs,observer,opaque);
}

bool ugreenctl_led_test_process_is_owner(const char *comm, const char *executable,
                                         const char *const *arguments, size_t count)
{
    return command_is_owner(comm, executable, arguments, count);
}

static int test_open_impl(const struct ugreenctl_led_identity *identity,
                           const struct ugreenctl_led_access *access,
                           const struct ugreenctl_led_test_io *io, void *opaque,
                           struct ugreenctl_led_session **output,
                           char *error, size_t error_size,const char *sysfs_root)
{
    struct ugreenctl_led_session *session;
    int result;
    if (identity == NULL || access == NULL || io == NULL || output == NULL ||
        io->owners == NULL || io->lock == NULL || io->unlock == NULL ||
        io->read_chip_id == NULL || io->read_status == NULL || io->write_command == NULL ||
        io->read_ack == NULL || io->pause_ms == NULL) return -EINVAL;
    *output = NULL;
    session = calloc(1, sizeof(*session));
    if (session == NULL) return -ENOMEM;
    session->fd = -1; session->lock_fd = -1;
    session->identity = *identity;
    session->identity.profile = ugreenctl_led_profile_find(identity->product_name, identity->product_version);
    session->access = *access;
    session->opaque = opaque;
    session->io.owners = io->owners; session->io.lock = io->lock; session->io.unlock = io->unlock;
    session->io.read_chip_id = io->read_chip_id; session->io.read_status = io->read_status;
    session->io.write_command = io->write_command; session->io.read_ack = io->read_ack;
    session->io.pause_ms = io->pause_ms;
    session->io.read_identity = io->read_identity;
    session->io.read_byte = io->read_byte; session->io.write_byte = io->write_byte;
    if (session->identity.profile == NULL) result = -ENOTSUP;
    else result = acquire(session, error, error_size);
    if (result==0 && sysfs_root!=NULL)
        result=ugreenctl_led_sysfs_open(session->identity.profile,sysfs_root,identify_authorize,session,&session->sysfs,error,error_size);
    else if (result == 0 && access->n76_direct) {
        if (!n76_profile(session->identity.profile) || !io->read_byte || !io->write_byte ||
            (access->n76_address != 0x31 && access->n76_address != 0x26) || access->n76_bus > 65535) result = -ENOTSUP;
        else result = n76_probe(session, error, error_size);
    } else if (result == 0) result = probe(session, error, error_size);
    if (result < 0) { ugreenctl_led_close(session); return result; }
    *output = session;
    return 0;
}

int ugreenctl_led_test_open(const struct ugreenctl_led_identity *identity,
                           const struct ugreenctl_led_access *access,
                           const struct ugreenctl_led_test_io *io,void *opaque,
                           struct ugreenctl_led_session **session,char *error,size_t size)
{
    return test_open_impl(identity,access,io,opaque,session,error,size,NULL);
}

int ugreenctl_led_test_open_sysfs(const struct ugreenctl_led_identity *identity,
                                 const struct ugreenctl_led_access *access,
                                 const struct ugreenctl_led_test_io *io,void *opaque,
                                 const char *sysfs_root,struct ugreenctl_led_session **session,
                                 char *error,size_t size)
{
    return test_open_impl(identity,access,io,opaque,session,error,size,sysfs_root);
}
#endif
