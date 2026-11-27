#define _XOPEN_SOURCE 700

#include "disk/disk.h"
#include "disk/firmware_profiles.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct disk_profile {
    const char *model;
    const char *pci;
    unsigned int port;
    unsigned int slot;
};

/* Independently reported PCI paths and verified slot orders, not family aliases.
 * https://github.com/miskcoo/ugreen_leds_controller/issues/9
 * Jun 15-16, 2024: miskcoo (DXP4800), b-reich (DXP6800 Pro),
 * meyergru (DXP8800 Plus). See disk/NOTICE.md for attribution and limits.
 */
static const struct disk_profile profiles[] = {
    {"DXP4800", "0000:00:17.0", 1, 1},
    {"DXP4800", "0000:00:17.0", 2, 2},
    {"DXP4800", "0000:01:00.0", 1, 3},
    {"DXP4800", "0000:01:00.0", 2, 4},
    {"DXP6800 Pro", "0000:59:00.0", 1, 1},
    {"DXP6800 Pro", "0000:59:00.0", 2, 2},
    {"DXP6800 Pro", "0000:59:00.0", 3, 3},
    {"DXP6800 Pro", "0000:59:00.0", 4, 4},
    {"DXP6800 Pro", "0000:00:17.0", 1, 5},
    {"DXP6800 Pro", "0000:00:17.0", 2, 6},
    {"DXP8800 Plus", "0000:59:00.0", 1, 1},
    {"DXP8800 Plus", "0000:59:00.0", 2, 2},
    {"DXP8800 Plus", "0000:59:00.0", 3, 3},
    {"DXP8800 Plus", "0000:59:00.0", 4, 4},
    {"DXP8800 Plus", "0000:5a:00.0", 1, 5},
    {"DXP8800 Plus", "0000:5a:00.0", 2, 6},
    {"DXP8800 Plus", "0000:5a:00.0", 3, 7},
    {"DXP8800 Plus", "0000:5a:00.0", 4, 8}
};

static int fail(char *error, size_t size, const char *format, ...)
{
    va_list args;
    if (error && size) {
        va_start(args, format);
        vsnprintf(error, size, format, args);
        va_end(args);
    }
    return -1;
}

static int parse_number(const char *value, unsigned int *number)
{
    unsigned long n;
    char *end;
    if (!value || !*value || !isdigit((unsigned char)*value))
        return -1;
    errno = 0;
    n = strtoul(value, &end, 10);
    if (errno || *end || n > UINT_MAX)
        return -1;
    *number = (unsigned int)n;
    return 0;
}

static unsigned int selector_slot(const char *selector)
{
    unsigned int number;
    const char *digits;
    if (strncmp(selector, "slot", 4) == 0 || strncmp(selector, "disk", 4) == 0)
        digits = selector + 4;
    else
        return 0;
    if (parse_number(digits, &number) != 0 || !number || number > 8)
        return 0;
    return number;
}

int ugreenctl_disk_resolve(const struct ugreenctl_disk_scan *scan,
                          const char *selector,
                          const struct ugreenctl_disk **out,
                          char *error, size_t error_size)
{
    const struct ugreenctl_disk *found = NULL;
    unsigned int slot;
    size_t i, matches = 0;
    if (out)
        *out = NULL;
    if (!scan || !selector || !*selector || !out || scan->count > UGREENCTL_DISK_MAX ||
        scan->namespace_count > UGREENCTL_DISK_MAX)
        return fail(error, error_size, "Invalid disk selector or scan");
    slot = selector_slot(selector);
    for (i = 0; i < scan->count; ++i) {
        const struct ugreenctl_disk *disk = &scan->disks[i];
        bool match;
        if (slot)
            match = disk->mapped && disk->slot == slot && strcmp(disk->interface_type, "nvme") != 0;
        else if (strncmp(selector, "nvme", 4) == 0 &&
                 disk->slot_name[0] && strcmp(selector, disk->slot_name) == 0)
            match = true;
        else if (strncmp(selector, "serial:", 7) == 0)
            match = disk->serial[0] && strcmp(selector + 7, disk->serial) == 0;
        else if (strncmp(selector, "wwn:", 4) == 0)
            match = disk->wwn[0] && strcmp(selector + 4, disk->wwn) == 0;
        else
            match = strcmp(selector, disk->name) == 0 ||
                    strcmp(selector, disk->device) == 0 ||
                    (disk->serial[0] && strcmp(selector, disk->serial) == 0);
        if (!match && !slot) {
            size_t n;
            for (n = 0; n < scan->namespace_count; ++n) {
                const char *alias = scan->namespace_aliases[n].name;
                if (strcmp(scan->namespace_aliases[n].disk_name, disk->name))
                    continue;
                if ((!strncmp(selector, "wwn:", 4) && scan->namespace_aliases[n].wwn[0] &&
                     !strcmp(selector + 4, scan->namespace_aliases[n].wwn)) ||
                    !strcmp(selector, alias) ||
                    (!strncmp(selector, "/dev/", 5) && !strcmp(selector + 5, alias))) {
                    match = true;
                    break;
                }
            }
        }
        if (match) {
            found = disk;
            ++matches;
        }
    }
    if (!matches)
        return fail(error, error_size, "No currently mapped disk matches '%s'", selector);
    if (matches != 1)
        return fail(error, error_size, "Disk selector '%s' is ambiguous", selector);
    if (!found->mapped)
        return fail(error, error_size, "Disk '%s' has no confirmed LED mapping: %s",
                    selector, found->reason);
    if (!found->led_supported || !found->led_id[0]) {
        (void)fail(error, error_size, "Unsupported: physical slot '%s' has no confirmed independently controllable disk LED", found->slot_name);
        return -ENOTSUP;
    }
    *out = found;
    return 0;
}

#ifdef __linux__

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct calibration {
    char topology[8][128];
    unsigned int slot[8];
    size_t count;
    bool present;
};

static char *trim(char *s)
{
    size_t n;
    while (isspace((unsigned char)*s))
        ++s;
    n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    return s;
}

/* Bay limits are physical layout data only; they do not enable auto-mapping.
 * Keep exact names independent, including the Plus/Pro/S variants. */
static unsigned int known_bay_limit(const char *model)
{
    const struct ugreenctl_disk_firmware_layout *layout = ugreenctl_disk_firmware_layout(model, "rev1");
    unsigned int limit = 0;
    size_t i;
    if (!layout)
        return 8;
    for (i = 0; i < layout->count; ++i) {
        unsigned int slot;
        if (!strncmp(layout->rules[i].slot, "ata", 3) &&
            !parse_number(layout->rules[i].slot + 3, &slot) && slot > limit)
            limit = slot;
    }
    return limit;
}

static int read_text(const char *path, char *out, size_t size)
{
    FILE *file;
    size_t n, i;
    char *start;
    out[0] = '\0';
    file = fopen(path, "r");
    if (!file)
        return -1;
    n = fread(out, 1, size - 1, file);
    if (ferror(file) || (!feof(file) && fgetc(file) != EOF)) {
        fclose(file);
        out[0] = '\0';
        return -1;
    }
    fclose(file);
    out[n] = '\0';
    /* Embedded NULs/control characters cannot identify a disk safely. */
    for (i = 0; i < n; ++i)
        if (!out[i] || (iscntrl((unsigned char)out[i]) &&
                       out[i] != '\n' && out[i] != '\r' && out[i] != '\t')) {
            out[0] = '\0';
            return -1;
        }
    start = trim(out);
    if (strchr(start, '\n') || strchr(start, '\r') || strchr(start, '\t')) {
        out[0] = '\0';
        return -1;
    }
    memmove(out, start, strlen(start) + 1);
    return 0;
}

static bool printable_identity(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    if (!*p)
        return false;
    for (; *p; ++p)
        if (*p < 0x20 || *p > 0x7e)
            return false;
    return true;
}

/* Linux exposes its cached SCSI unit-serial VPD page; reading this sysfs
 * attribute does not issue ATA IDENTIFY, SMART or SG_IO to a parked drive.
 * Layout: peripheral type, page code 0x80, 16-bit big-endian payload length.
 */
static int read_vpd_serial(const char *path, char *out, size_t size)
{
    unsigned char header[4];
    FILE *file;
    size_t length;
    char *start;
    out[0] = '\0';
    file = fopen(path, "rb");
    if (!file)
        return -1;
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        header[0] != 0 || header[1] != 0x80)
        goto invalid;
    length = ((size_t)header[2] << 8) | header[3];
    if (!length || length >= size || fread(out, 1, length, file) != length ||
        fgetc(file) != EOF || ferror(file))
        goto invalid;
    out[length] = '\0';
    /* Validate the complete binary payload, including embedded NUL bytes. */
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e)
            goto invalid;
    fclose(file);
    start = trim(out);
    memmove(out, start, strlen(start) + 1);
    return out[0] ? 0 : -1;
invalid:
    fclose(file);
    out[0] = '\0';
    return -1;
}

static void read_serial(const char *link, char *out, size_t size)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/device/serial", link);
    if (!read_text(path, out, size) && printable_identity(out))
        return;
    snprintf(path, sizeof(path), "%s/device/vpd_pg80", link);
    (void)read_vpd_serial(path, out, size);
}

static void read_wwid(const char *link, char *out, size_t size)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/wwid", link);
    if (read_text(path, out, size)) {
        snprintf(path, sizeof(path), "%s/device/wwid", link);
        (void)read_text(path, out, size);
    }
    if (!printable_identity(out))
        out[0] = '\0';
}

static bool is_pci(const char *s)
{
    size_t i;
    if (strlen(s) != 12 || s[4] != ':' || s[7] != ':' || s[10] != '.')
        return false;
    for (i = 0; i < 12; ++i) {
        if (i == 4 || i == 7 || i == 10)
            continue;
        if (!isxdigit((unsigned char)s[i]))
            return false;
    }
    return s[11] >= '0' && s[11] <= '7';
}

static bool valid_topology(const char *value)
{
    char pci[16];
    unsigned int port;
    if (strlen(value) < 22 || strncmp(value, "pci-", 4) ||
        strncmp(value + 16, "-ata-", 5))
        return false;
    memcpy(pci, value + 4, 12);
    pci[12] = '\0';
    return is_pci(pci) && parse_number(value + 21, &port) == 0 && port && port <= 1024;
}

struct single_sata_profile {
    const char *model;
    const char *product_version;
    unsigned int vendor, device, port_count;
    const char *source;
};

static const struct single_sata_profile *single_sata_profile_find(const char *root,
                                                                 const char *model)
{
    static const struct single_sata_profile single_profiles[] = {
        {"DXP4800 Plus", NULL, 0x1b21, 0x1164, 4, "firmware-asm1164-topology"},
        {"DXP4800 Pro", NULL, 0x1b21, 0x1164, 4, "firmware-asm1164-topology"},
        {"DXP2800", "EM_DXP2800_V1.0.24", 0x8086, 0x54d3, 2, "firmware-intel-ahci-topology"},
        {"DXP2800", "DXP2800 V1.2 251215", 0x8086, 0x54d3, 2, "firmware-intel-ahci-topology"}
    };
    size_t i;
    for (i = 0; i < sizeof(single_profiles) / sizeof(single_profiles[0]); ++i) {
        const struct single_sata_profile *profile = &single_profiles[i];
        char path[PATH_MAX], version[256];
        if (strcmp(model, profile->model))
            continue;
        if (profile->product_version) {
            snprintf(path, sizeof(path), "%s/class/dmi/id/product_version", root);
            if (read_text(path, version, sizeof(version)) || strcmp(version, profile->product_version))
                continue;
        }
        return profile;
    }
    return NULL;
}

static int read_hex(const char *path, unsigned int *value)
{
    char text[32], *end;
    unsigned long number;
    if (read_text(path, text, sizeof(text)) || !*text)
        return -1;
    errno = 0;
    number = strtoul(text, &end, 16);
    if (errno || *end || number > UINT_MAX)
        return -1;
    *value = (unsigned int)number;
    return 0;
}

/* Stock firmware maps ata1..N to disk1..N on these exact model/revisions.
 * Independently reported single AHCI controllers let that order be expressed
 * using controller-local ports instead of Linux's global counters.
 * This conversion is allowed only after validating the entire controller and
 * every port, including empty bays. No ranking of installed disks is involved.
 */
static int single_sata_controller(const char *root, const struct single_sata_profile *profile,
                                   char *pci, size_t pci_size,
                                   char *reason, size_t reason_size)
{
    char directory[PATH_MAX], path[PATH_MAX], candidate[16] = "";
    char controller[PATH_MAX] = "", resolved[PATH_MAX], expected[PATH_MAX];
    DIR *dir;
    struct dirent *entry;
    unsigned int sata_count = 0, port_count = 0;
    bool ports[4] = {false};
    int result = 0;
    pci[0] = '\0';
    if (!profile || !profile->port_count || profile->port_count > sizeof(ports) / sizeof(ports[0]))
        return fail(reason, reason_size, "Invalid single-controller port-count profile");
    snprintf(directory, sizeof(directory), "%s/bus/pci/devices", root);
    dir = opendir(directory);
    if (!dir)
        return fail(reason, reason_size, "PCI controller inventory unavailable; cannot confirm the single-controller layout");
    errno = 0;
    while ((entry = readdir(dir))) {
        unsigned int class_code, vendor, device;
        const char *base;
        if (entry->d_name[0] == '.')
            continue;
        if (!is_pci(entry->d_name)) {
            result = -1;
            break;
        }
        snprintf(path, sizeof(path), "%s/%s/class", directory, entry->d_name);
        if (read_hex(path, &class_code)) {
            result = -1;
            break;
        }
        if ((class_code >> 8) != 0x0106) {
            errno = 0;
            continue;
        }
        ++sata_count;
        if (sata_count != 1 || class_code != 0x010601) {
            result = -1;
            break;
        }
        snprintf(path, sizeof(path), "%s/%s/vendor", directory, entry->d_name);
        if (read_hex(path, &vendor) || vendor != profile->vendor) {
            result = -1;
            break;
        }
        snprintf(path, sizeof(path), "%s/%s/device", directory, entry->d_name);
        if (read_hex(path, &device) || device != profile->device) {
            result = -1;
            break;
        }
        snprintf(path, sizeof(path), "%s/%s/driver", directory, entry->d_name);
        if (!realpath(path, resolved)) {
            result = -1;
            break;
        }
        base = strrchr(resolved, '/');
        if (!base || strcmp(base + 1, "ahci")) {
            result = -1;
            break;
        }
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (!realpath(path, controller) || strncmp(controller, root, strlen(root)) ||
            strncmp(controller + strlen(root), "/devices/", 9)) {
            result = -1;
            break;
        }
        base = strrchr(controller, '/');
        if (!base || strcmp(base + 1, entry->d_name)) {
            result = -1;
            break;
        }
        snprintf(candidate, sizeof(candidate), "%s", entry->d_name);
        errno = 0;
    }
    if (errno)
        result = -1;
    closedir(dir);
    if (result || sata_count != 1)
        return fail(reason, reason_size, "Expected one and only one SATA controller: %04x:%04x class 010601 using ahci",
                    profile->vendor, profile->device);
    snprintf(directory, sizeof(directory), "%s/class/ata_port", root);
    dir = opendir(directory);
    if (!dir)
        return fail(reason, reason_size, "ATA port inventory unavailable; empty chassis ports must also be confirmed");
    errno = 0;
    while ((entry = readdir(dir))) {
        char text[32];
        unsigned int ata, port;
        if (entry->d_name[0] == '.')
            continue;
        if (strncmp(entry->d_name, "ata", 3) || parse_number(entry->d_name + 3, &ata) || !ata) {
            result = -1;
            break;
        }
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        snprintf(expected, sizeof(expected), "%s/ata%u/ata_port/ata%u", controller, ata, ata);
        if (!realpath(path, resolved) || strcmp(resolved, expected)) {
            result = -1;
            break;
        }
        snprintf(path, sizeof(path), "%s/%s/port_no", directory, entry->d_name);
        if (read_text(path, text, sizeof(text)) || parse_number(text, &port) ||
            port < 1 || port > profile->port_count || ports[port - 1]) {
            result = -1;
            break;
        }
        ports[port - 1] = true;
        ++port_count;
        errno = 0;
    }
    if (errno)
        result = -1;
    closedir(dir);
    if (result || port_count != profile->port_count)
        return fail(reason, reason_size, "Controller must expose exactly %u unique local ATA ports 1..%u under the same PCI parent, including empty bays",
                    profile->port_count, profile->port_count);
    snprintf(pci, pci_size, "%s", candidate);
    return 0;
}

static int read_calibration(const char *path, bool optional, const char *model,
                            struct calibration *cal,
                            char *error, size_t error_size)
{
    FILE *file;
    struct stat info;
    char line[512];
    unsigned int line_no = 0;
    bool model_seen = false;
    int fd;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (optional && errno == ENOENT)
            return 0;
        return fail(error, error_size, "Cannot open disk calibration '%s': %s", path, strerror(errno));
    }
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != 0 ||
        (info.st_mode & (S_IWGRP | S_IWOTH))) {
        close(fd);
        return fail(error, error_size, "Disk calibration must be a root-owned regular file without group/other write permission");
    }
    file = fdopen(fd, "r");
    if (!file) {
        close(fd);
        return fail(error, error_size, "Cannot read disk calibration");
    }
    while (fgets(line, sizeof(line), file)) {
        char *text, *space, *topology;
        unsigned int slot;
        size_t i;
        ++line_no;
        if (!strchr(line, '\n') && !feof(file))
            goto invalid;
        text = trim(line);
        if (!*text || *text == '#')
            continue;
        if (strncmp(text, "model=", 6) == 0) {
            if (model_seen || strcmp(text + 6, model))
                goto invalid;
            model_seen = true;
            continue;
        }
        if (!model_seen || cal->count >= 8)
            goto invalid;
        space = strchr(text, ' ');
        if (!space)
            goto invalid;
        *space = '\0';
        slot = selector_slot(text);
        topology = trim(space + 1);
        if (!slot || slot > known_bay_limit(model) || strncmp(topology, "topology=", 9))
            goto invalid;
        topology += 9;
        if (!valid_topology(topology) || strlen(topology) >= sizeof(cal->topology[0]))
            goto invalid;
        for (i = 0; i < cal->count; ++i)
            if (cal->slot[i] == slot || strcmp(cal->topology[i], topology) == 0)
                goto invalid;
        cal->slot[cal->count] = slot;
        snprintf(cal->topology[cal->count], sizeof(cal->topology[0]), "%s", topology);
        ++cal->count;
    }
    if (ferror(file) || !model_seen || !cal->count)
        goto invalid;
    fclose(file);
    cal->present = true;
    return 0;
invalid:
    fclose(file);
    return fail(error, error_size, "Invalid or conflicting disk calibration at line %u (model must match exact DMI)", line_no);
}

static bool disk_name(const char *name)
{
    const char *p;
    if (strncmp(name, "sd", 2) || !name[2])
        return false;
    for (p = name + 2; *p; ++p)
        if (*p < 'a' || *p > 'z')
            return false;
    return true;
}

static bool nvme_name(const char *name)
{
    unsigned int controller, ns;
    int used = 0;
    return sscanf(name, "nvme%un%u%n", &controller, &ns, &used) == 2 &&
           used > 0 && name[used] == '\0' && ns > 0;
}

static bool topology_from_path(const char *resolved, struct ugreenctl_disk *disk)
{
    char copy[PATH_MAX], *part, *next, pci[16] = "";
    bool ata_seen = false, scsi_seen = false;
    snprintf(copy, sizeof(copy), "%s", resolved);
    for (part = strtok_r(copy, "/", &next); part; part = strtok_r(NULL, "/", &next)) {
        unsigned int ata, host, bus, target, lun;
        int used = 0;
        if (strncmp(part, "usb", 3) == 0 || strcmp(part, "virtual") == 0 ||
            strncmp(part, "session", 7) == 0 || strncmp(part, "rport-", 6) == 0)
            return false;
        if (!ata_seen && is_pci(part))
            snprintf(pci, sizeof(pci), "%s", part);
        if (strncmp(part, "ata", 3) == 0 && parse_number(part + 3, &ata) == 0) {
            if (ata_seen || !ata || (!pci[0] && !strstr(resolved, "/devices/platform/")))
                return false;
            disk->ata = ata;
            snprintf(disk->pci, sizeof(disk->pci), "%s", pci);
            ata_seen = true;
        }
        if (sscanf(part, "%u:%u:%u:%u%n", &host, &bus, &target, &lun, &used) == 4 &&
            part[used] == '\0') {
            /* Port multipliers/master-slave are not chassis-bay evidence. */
            if (bus || target || lun)
                return false;
            scsi_seen = true;
        }
    }
    return ata_seen && scsi_seen;
}

static bool platform_sata_controller(const char *resolved, char *out, size_t size)
{
    char controller[PATH_MAX], path[PATH_MAX], driver[PATH_MAX];
    const char *ata = strstr(resolved, "/ata"), *name;
    size_t length;
    if (!ata || !strstr(resolved, "/devices/platform/"))
        return false;
    length = (size_t)(ata - resolved);
    if (length >= sizeof(controller))
        return false;
    memcpy(controller, resolved, length); controller[length] = '\0';
    name = strrchr(controller, '/');
    if (!name || !name[1] || strlen(name + 1) >= size)
        return false;
    snprintf(path, sizeof(path), "%s/driver", controller);
    if (!realpath(path, driver) || !strrchr(driver, '/') ||
        (strcmp(strrchr(driver, '/') + 1, "ahci") && strcmp(strrchr(driver, '/') + 1, "dwc-ahci")))
        return false;
    snprintf(out, size, "%s", name + 1);
    return true;
}

static int read_one(const char *root, const char *name, struct ugreenctl_disk *disk,
                    char controller_path[PATH_MAX])
{
    char link[PATH_MAX], resolved[PATH_MAX], check[PATH_MAX], path[PATH_MAX];
    char value[256], serial_check[256], wwn_check[256], platform[80] = "";
    size_t root_len = strlen(root);
    bool nvme = nvme_name(name);
    controller_path[0] = '\0';
    if ((!disk_name(name) && !nvme) || strlen(name) >= sizeof(disk->name))
        return 0;
    if (snprintf(link, sizeof(link), "%s/class/block/%s", root, name) >= (int)sizeof(link) ||
        !realpath(link, resolved) || strncmp(resolved, root, root_len) ||
        strncmp(resolved + root_len, "/devices/", 9))
        return 0;
    snprintf(path, sizeof(path), "%s/partition", link);
    if (access(path, F_OK) == 0)
        return 0;
    if (nvme) {
        char copy[PATH_MAX], *part, *next;
        char expected[64], *end;
        unsigned int controller, ns;
        if (strstr(resolved, "/virtual/") || strstr(resolved, "/usb") || !strstr(resolved, "/nvme/"))
            return 0;
        snprintf(path, sizeof(path), "%s/device/transport", link);
        if (read_text(path, value, sizeof(value)) || strcmp(value, "pcie"))
            return 0;
        snprintf(copy, sizeof(copy), "%s", resolved);
        for (part = strtok_r(copy, "/", &next); part; part = strtok_r(NULL, "/", &next))
            if (is_pci(part))
                snprintf(disk->pci, sizeof(disk->pci), "%s", part);
        if (!disk->pci[0])
            return 0;
        /* Coalescing is based on the canonical live controller object, never
         * an NVMe enumeration number or a shared serial alone. */
        snprintf(copy, sizeof(copy), "%s", resolved);
        end = strrchr(copy, '/');
        if (!end || strcmp(end + 1, name) ||
            sscanf(name, "nvme%un%u", &controller, &ns) != 2)
            return 0;
        *end = '\0';
        end = strrchr(copy, '/');
        snprintf(expected, sizeof(expected), "nvme%u", controller);
        if (!end || strcmp(end + 1, expected))
            return 0;
        snprintf(controller_path, PATH_MAX, "%s", copy);
        snprintf(disk->interface_type, sizeof(disk->interface_type), "nvme");
        snprintf(disk->topology, sizeof(disk->topology), "pci-%s-nvme", disk->pci);
    } else {
        if (!topology_from_path(resolved, disk))
            return 0;
        if (!disk->pci[0] && !platform_sata_controller(resolved, platform, sizeof(platform)))
            return 0;
        snprintf(path, sizeof(path), "%s/device/type", link);
        if (read_text(path, value, sizeof(value)) || strcmp(value, "0"))
            return 0;
        snprintf(path, sizeof(path), "%s/device/vendor", link);
        if (read_text(path, value, sizeof(value)) || strcmp(value, "ATA"))
            return 0;
        snprintf(disk->interface_type, sizeof(disk->interface_type), "sata");
    }
    snprintf(disk->name, sizeof(disk->name), "%s", name);
    snprintf(disk->device, sizeof(disk->device), "/dev/%s", name);
    read_serial(link, disk->serial, sizeof(disk->serial));
    read_wwid(link, disk->wwn, sizeof(disk->wwn));
    snprintf(path, sizeof(path), "%s/class/ata_port/ata%u/port_no", root, disk->ata);
    if (nvme) {
        /* Physical M.2/U.2 slot is assigned by the firmware PCI rule below. */
    } else if (!read_text(path, value, sizeof(value)) &&
        !parse_number(value, &disk->ata_port) && disk->ata_port && disk->ata_port <= 1024)
        snprintf(disk->topology, sizeof(disk->topology), "%s-%s-ata-%u",
                 platform[0] ? "platform" : "pci", platform[0] ? platform : disk->pci, disk->ata_port);
    else {
        disk->ata_port = 0;
        snprintf(disk->reason, sizeof(disk->reason), "Controller-local ATA port is unavailable; global ATA/HCTL order is unsafe");
    }
    /* A disappearing/replaced symlink during enumeration is never mapped. */
    if (!realpath(link, check) || strcmp(check, resolved))
        return 0;
    /* ATA/SCSI can reuse an identical sysfs path after a replacement. Recheck
     * cached identity too, so a scan never mixes an old serial with a new disk.
     * Physical-topology mapping remains useful when neither identity exists. */
    read_serial(link, serial_check, sizeof(serial_check));
    read_wwid(link, wwn_check, sizeof(wwn_check));
    if (strcmp(serial_check, disk->serial) || strcmp(wwn_check, disk->wwn))
        return 0;
    return 1;
}

static int compare_disks(const void *a, const void *b)
{
    const struct ugreenctl_disk *da = a, *db = b;
    return strcmp(da->name, db->name);
}

static const struct ugreenctl_disk_firmware_layout *firmware_layout(const char *root,
                                                                   const char *model,
                                                                   bool *revision_known)
{
    char path[PATH_MAX], bios[256];
    const char *revision = "rev1";
    *revision_known = true;
    if (!strcmp(model, "DXP2800")) {
        snprintf(path, sizeof(path), "%s/class/dmi/id/product_version", root);
        if (read_text(path, bios, sizeof(bios)) || !bios[0])
            *revision_known = false;
        else if (strncmp(bios, "EM_", 3))
            revision = "rev2";
    } else if (!strcmp(model, "DXP2800 GT")) {
        /* The recovered stock LL_/else selector is not an independently
         * verified pairing of a real BIOS version with physical SATA bays.
         * Keep the revision unknown until such exact evidence exists. */
        *revision_known = false;
    }
    return ugreenctl_disk_firmware_layout(model, revision);
}

static bool gt_rule_revision_independent(const struct ugreenctl_disk_firmware_rule *rule)
{
    static const char *const revisions[] = {"rev1", "rev2"};
    size_t revision, i;
    if (!strcmp(rule->interface_type, "sata"))
        return false;
    /* Retain only connector facts present unchanged in BOTH recovered tables.
     * A shared chip, slot name or an arbitrary choice of rev1 is insufficient. */
    for (revision = 0; revision < sizeof(revisions) / sizeof(revisions[0]); ++revision) {
        const struct ugreenctl_disk_firmware_layout *layout =
            ugreenctl_disk_firmware_layout("DXP2800 GT", revisions[revision]);
        unsigned int matches = 0;
        if (!layout)
            return false;
        for (i = 0; i < layout->count; ++i) {
            const struct ugreenctl_disk_firmware_rule *candidate = &layout->rules[i];
            if (!strcmp(rule->slot, candidate->slot) && !strcmp(rule->kind, candidate->kind) &&
                !strcmp(rule->identity, candidate->identity) &&
                !strcmp(rule->interface_type, candidate->interface_type) &&
                rule->color_led == candidate->color_led)
                ++matches;
        }
        if (matches != 1)
            return false;
    }
    return true;
}

static bool bridge_matches(const char *resolved, const char *identity)
{
    char prefix[PATH_MAX], path[PATH_MAX], part[16];
    unsigned int wanted_vendor, wanted_device;
    int consumed = 0;
    const char *p = resolved;
    if (sscanf(identity, "%x:%x%n", &wanted_vendor, &wanted_device, &consumed) != 2 || identity[consumed])
        return false;
    while ((p = strchr(p, '/'))) {
        const char *end = strchr(++p, '/');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        unsigned int vendor, device, class_code;
        if (length != 12)
            continue;
        memcpy(part, p, length); part[length] = '\0';
        if (!is_pci(part))
            continue;
        length = (size_t)((end ? end : p + length) - resolved);
        if (length >= sizeof(prefix))
            return false;
        memcpy(prefix, resolved, length); prefix[length] = '\0';
        snprintf(path, sizeof(path), "%s/vendor", prefix);
        if (read_hex(path, &vendor) || vendor != wanted_vendor)
            continue;
        snprintf(path, sizeof(path), "%s/device", prefix);
        if (read_hex(path, &device) || device != wanted_device)
            continue;
        snprintf(path, sizeof(path), "%s/class", prefix);
        if (!read_hex(path, &class_code) && (class_code >> 8) == 0x0604)
            return true;
    }
    return false;
}

static bool bridge_identity_unique(const char *root, const char *identity)
{
    DIR *dir;
    struct dirent *entry;
    char directory[PATH_MAX], path[PATH_MAX];
    unsigned int wanted_vendor, wanted_device, count = 0;
    int used = 0;
    bool valid = true;
    if (sscanf(identity, "%x:%x%n", &wanted_vendor, &wanted_device, &used) != 2 || identity[used])
        return false;
    snprintf(directory, sizeof(directory), "%s/bus/pci/devices", root);
    dir = opendir(directory);
    if (!dir) return false;
    while ((entry = readdir(dir))) {
        unsigned int class_code, vendor, device;
        if (entry->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s/class", directory, entry->d_name);
        if (read_hex(path, &class_code)) { valid = false; break; }
        if ((class_code >> 8) != 0x0604) continue;
        snprintf(path, sizeof(path), "%s/%s/vendor", directory, entry->d_name);
        if (read_hex(path, &vendor)) { valid = false; break; }
        if (vendor != wanted_vendor) continue;
        snprintf(path, sizeof(path), "%s/%s/device", directory, entry->d_name);
        if (read_hex(path, &device)) { valid = false; break; }
        if (device == wanted_device && ++count > 1) break;
    }
    closedir(dir);
    return valid && count == 1;
}

static bool controller_ports(const char *root, const char *pci, unsigned int wanted)
{
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX], controller[PATH_MAX], directory[PATH_MAX], resolved[PATH_MAX], expected[PATH_MAX];
    char value[32];
    unsigned int count = 0, mask = 0, class_code;
    bool valid = true;
    if (!wanted || wanted > 8)
        return false;
    snprintf(path, sizeof(path), "%s/bus/pci/devices/%s", root, pci);
    if (!realpath(path, controller))
        return false;
    snprintf(path, sizeof(path), "%s/class", controller);
    if (read_hex(path, &class_code) || class_code != 0x010601)
        return false;
    snprintf(path, sizeof(path), "%s/driver", controller);
    if (!realpath(path, resolved) || !strrchr(resolved, '/') || strcmp(strrchr(resolved, '/') + 1, "ahci"))
        return false;
    snprintf(directory, sizeof(directory), "%s/class/ata_port", root);
    dir = opendir(directory);
    if (!dir)
        return false;
    while ((entry = readdir(dir))) {
        unsigned int ata, port;
        if (entry->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (!realpath(path, resolved)) { valid = false; break; }
        if (strncmp(resolved, controller, strlen(controller)) || resolved[strlen(controller)] != '/')
            continue;
        if (strncmp(entry->d_name, "ata", 3) || parse_number(entry->d_name + 3, &ata)) { valid = false; break; }
        snprintf(expected, sizeof(expected), "%s/ata%u/ata_port/ata%u", controller, ata, ata);
        if (strcmp(expected, resolved)) { valid = false; break; }
        snprintf(path, sizeof(path), "%s/port_no", resolved);
        if (read_text(path, value, sizeof(value)) || parse_number(value, &port) ||
            !port || port > wanted || (mask & (1U << (port - 1)))) { valid = false; break; }
        mask |= 1U << (port - 1); ++count;
    }
    closedir(dir);
    return valid && count == wanted && mask == (1U << wanted) - 1U;
}

static bool property_equals(const char *path, const void *expected, size_t size)
{
    unsigned char data[256];
    FILE *file;
    bool equal;
    if (size > sizeof(data) || !(file = fopen(path, "rb")))
        return false;
    equal = fread(data, 1, size, file) == size && !memcmp(data, expected, size) &&
            fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return equal;
}

static bool controller_of_node(const char *root, const char *controller, const char *node)
{
    char path[PATH_MAX], expected[PATH_MAX], actual[PATH_MAX];
    snprintf(path, sizeof(path), "%s/firmware/devicetree/base/%s", root, node);
    if (!realpath(path, expected))
        return false;
    snprintf(path, sizeof(path), "%s/of_node", controller);
    return realpath(path, actual) && !strcmp(expected, actual);
}

static bool native_ahci_node(const char *root, const char *controller, const char *node,
                              unsigned int address)
{
    static const char compatible[] = "rockchip,rk-ahci\0snps,dwc-ahci";
    static const unsigned char ports[] = {0,0,0,1};
    unsigned char reg[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10,0};
    char path[PATH_MAX], driver[PATH_MAX];
    if (!controller_of_node(root, controller, node))
        return false;
    reg[4] = (unsigned char)(address >> 24); reg[5] = (unsigned char)(address >> 16);
    reg[6] = (unsigned char)(address >> 8); reg[7] = (unsigned char)address;
    snprintf(path, sizeof(path), "%s/of_node/compatible", controller);
    if (!property_equals(path, compatible, sizeof(compatible))) return false;
    snprintf(path, sizeof(path), "%s/of_node/reg", controller);
    if (!property_equals(path, reg, sizeof(reg))) return false;
    snprintf(path, sizeof(path), "%s/of_node/ports-implemented", controller);
    if (!property_equals(path, ports, sizeof(ports))) return false;
    snprintf(path, sizeof(path), "%s/of_node/status", controller);
    if (!property_equals(path, "okay", sizeof("okay"))) return false;
    snprintf(path, sizeof(path), "%s/driver", controller);
    return realpath(path, driver) && strrchr(driver, '/') &&
           (!strcmp(strrchr(driver, '/') + 1, "dwc-ahci") || !strcmp(strrchr(driver, '/') + 1, "ahci"));
}

static bool pci_under_of_node(const char *root, const char *controller, const char *node)
{
    static const char compatible[] = "rockchip,rk3588-pcie\0snps,dw-pcie";
    static const unsigned char first_reg[] = {0,0,0,0,0xfe,0x17,0,0,0,0,0,0,0,1,0,0};
    char parent[PATH_MAX], path[PATH_MAX], *end;
    snprintf(parent, sizeof(parent), "%s", controller);
    while (strlen(parent) > strlen(root) + 9) {
        if (controller_of_node(root, parent, node)) {
            unsigned char reg[64];
            FILE *file;
            size_t count;
            snprintf(path, sizeof(path), "%s/of_node/compatible", parent);
            if (!property_equals(path, compatible, sizeof(compatible))) return false;
            snprintf(path, sizeof(path), "%s/of_node/status", parent);
            if (!property_equals(path, "okay", sizeof("okay"))) return false;
            snprintf(path, sizeof(path), "%s/of_node/reg", parent);
            file = fopen(path, "rb");
            if (!file) return false;
            count = fread(reg, 1, sizeof(reg), file);
            if (ferror(file) || count < sizeof(first_reg) || count % 16 ||
                fgetc(file) != EOF || memcmp(reg, first_reg, sizeof(first_reg))) {
                fclose(file); return false;
            }
            fclose(file);
            return true;
        }
        end = strrchr(parent, '/');
        if (!end) break;
        *end = '\0';
    }
    return false;
}

/* Original ARM Image initcalls, synchronous DWC/PHY probe policy and original
 * FDT child order establish these factory global-ATA roles. This is a static
 * conversion, never the live global ATA numbering. Every native empty port,
 * OF identity and optional ASM1062 PCI controller must match as one layout. */
static unsigned int firmware_arm_ata_index(const char *root, const char *model,
                                           const struct ugreenctl_disk *disk)
{
    const bool four = !strcmp(model, "DH4300 Plus");
    const char *dt_model = four ? "UGREEN RK3588C DH4300Plus Board" : "UGREEN RK3576 DH2300 Board";
    const char *nodes[2] = {four ? "sata@fe230000" : "sata@2a240000",
                            four ? "sata@fe220000" : "sata@2a250000"};
    const unsigned int addresses[2] = {four ? 0xfe230000U : 0x2a240000U,
                                       four ? 0xfe220000U : 0x2a250000U};
    char path[PATH_MAX], directory[PATH_MAX], controller[PATH_MAX], pci_controller[PATH_MAX] = "";
    char resolved[PATH_MAX], expected[PATH_MAX], pci[16] = "", value[32];
    unsigned int native_mask = 0, pci_ports = 0, count = 0, result = 0, sata_controllers = 0;
    DIR *dir;
    struct dirent *entry;
    bool valid = true;
    if (strcmp(model, "DH2300") && !four)
        return 0;
    snprintf(path, sizeof(path), "%s/firmware/devicetree/base/model", root);
    if (!property_equals(path, dt_model, strlen(dt_model) + 1))
        return 0;
    snprintf(directory, sizeof(directory), "%s/bus/pci/devices", root);
    dir = opendir(directory);
    if (!dir && (four || errno != ENOENT)) return 0;
    if (dir) {
        while ((entry = readdir(dir))) {
            unsigned int class_code, vendor, device;
            if (entry->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "%s/%s/class", directory, entry->d_name);
            if (read_hex(path, &class_code)) { valid = false; break; }
            if ((class_code >> 8) != 0x0106) continue;
            ++sata_controllers;
            if (!four || sata_controllers != 1 || class_code != 0x010601 || !is_pci(entry->d_name)) { valid = false; break; }
            snprintf(path, sizeof(path), "%s/%s/vendor", directory, entry->d_name);
            if (read_hex(path, &vendor) || vendor != 0x1b21) { valid = false; break; }
            snprintf(path, sizeof(path), "%s/%s/device", directory, entry->d_name);
            if (read_hex(path, &device) || device != 0x0612) { valid = false; break; }
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
            if (!realpath(path, pci_controller) || !pci_under_of_node(root, pci_controller, "pcie@fe170000") ||
                !controller_ports(root, entry->d_name, 2)) { valid = false; break; }
            snprintf(pci, sizeof(pci), "%s", entry->d_name);
        }
        closedir(dir);
    }
    if (!valid || sata_controllers != (four ? 1U : 0U)) return 0;
    snprintf(directory, sizeof(directory), "%s/class/ata_port", root);
    dir = opendir(directory);
    if (!dir) return 0;
    while ((entry = readdir(dir))) {
        unsigned int ata, port;
        char *ata_component;
        if (entry->d_name[0] == '.') continue;
        ++count;
        if (strncmp(entry->d_name, "ata", 3) || parse_number(entry->d_name + 3, &ata)) { valid = false; break; }
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (!realpath(path, resolved)) { valid = false; break; }
        snprintf(controller, sizeof(controller), "%s", resolved);
        ata_component = strstr(controller + strlen(root), "/ata");
        if (!ata_component) { valid = false; break; }
        *ata_component = '\0';
        snprintf(expected, sizeof(expected), "%s/ata%u/ata_port/ata%u", controller, ata, ata);
        if (strcmp(expected, resolved)) { valid = false; break; }
        snprintf(path, sizeof(path), "%s/port_no", resolved);
        if (read_text(path, value, sizeof(value)) || parse_number(value, &port) || !port) { valid = false; break; }
        if (four && !strcmp(controller, pci_controller)) {
            if (port > 2 || (pci_ports & (1U << (port - 1)))) { valid = false; break; }
            pci_ports |= 1U << (port - 1);
            if (ata == disk->ata && !strcmp(pci, disk->pci)) result = 2 + port;
        } else {
            unsigned int n;
            for (n = 0; n < 2; ++n)
                if (native_ahci_node(root, controller, nodes[n], addresses[n])) break;
            if (n == 2 || port != 1 || (native_mask & (1U << n))) { valid = false; break; }
            native_mask |= 1U << n;
            if (ata == disk->ata && !disk->pci[0]) result = n + 1;
        }
    }
    closedir(dir);
    return valid && count == (four ? 4U : 2U) && native_mask == 3 &&
           pci_ports == (four ? 3U : 0U) ? result : 0;
}

/* Convert the official global-ATA table only after recognizing the complete
 * factory controller roles and all empty ports. Endpoint bus numbers and
 * global ATA/host counters do not participate in this conversion. */
static unsigned int firmware_ata_index(const char *root,
                                       const struct ugreenctl_disk_firmware_layout *layout,
                                       const struct ugreenctl_disk *disk)
{
    struct controller { char pci[16], path[PATH_MAX]; unsigned int vendor, device, ports, bridge; } controllers[4];
    char directory[PATH_MAX], path[PATH_MAX];
    DIR *dir;
    struct dirent *entry;
    unsigned int expected = 0, count = 0, total_ports = 0;
    size_t i;
    bool valid = true;
    if (strcmp(disk->interface_type, "sata") || !disk->ata_port)
        return 0;
    if (!strcmp(layout->model, "DH2300") || !strcmp(layout->model, "DH4300 Plus"))
        return firmware_arm_ata_index(root, layout->model, disk);
    if (!strcmp(layout->model, "DXP2800") || !strcmp(layout->model, "DXP4800 Plus") ||
        !strcmp(layout->model, "DXP4800 Pro")) {
        const struct single_sata_profile *profile = single_sata_profile_find(root, layout->model);
        char pci[16], reason[160];
        if (!profile || single_sata_controller(root, profile, pci, sizeof(pci), reason, sizeof(reason)) ||
            strcmp(pci, disk->pci))
            return 0;
        return disk->ata_port;
    }
    for (i = 0; i < layout->count; ++i)
        if (!strcmp(layout->rules[i].interface_type, "sata") && !strcmp(layout->rules[i].kind, "ata_id"))
            ++expected;
    if (!expected)
        return 0;
    snprintf(directory, sizeof(directory), "%s/bus/pci/devices", root);
    dir = opendir(directory);
    if (!dir)
        return 0;
    memset(controllers, 0, sizeof(controllers));
    while ((entry = readdir(dir))) {
        unsigned int class_code;
        struct controller *controller;
        if (entry->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s/class", directory, entry->d_name);
        if (read_hex(path, &class_code)) { valid = false; break; }
        if ((class_code >> 8) != 0x0106) continue;
        if (class_code != 0x010601 || count == 4 || !is_pci(entry->d_name)) { valid = false; break; }
        controller = &controllers[count++];
        snprintf(controller->pci, sizeof(controller->pci), "%s", entry->d_name);
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (!realpath(path, controller->path)) { valid = false; break; }
        snprintf(path, sizeof(path), "%s/vendor", controller->path);
        if (read_hex(path, &controller->vendor)) { valid = false; break; }
        snprintf(path, sizeof(path), "%s/device", controller->path);
        if (read_hex(path, &controller->device)) { valid = false; break; }
        for (unsigned int n = 1; n <= 8; ++n)
            if (controller_ports(root, controller->pci, n)) { controller->ports = n; break; }
        if (!controller->ports) { valid = false; break; }
        total_ports += controller->ports;
        if (bridge_matches(controller->path, "8086:51bc")) controller->bridge = 1;
        if (bridge_matches(controller->path, "8086:51be")) controller->bridge = 2;
    }
    closedir(dir);
    if (!valid || total_ports != expected)
        return 0;
    /* No unattached/foreign libata port may be hidden outside PCI inventory. */
    snprintf(directory, sizeof(directory), "%s/class/ata_port", root);
    dir = opendir(directory);
    if (!dir) return 0;
    total_ports = 0;
    while ((entry = readdir(dir)))
        if (entry->d_name[0] != '.') ++total_ports;
    closedir(dir);
    if (total_ports != expected)
        return 0;
    for (i = 0; i < count; ++i) {
        const struct controller *c = &controllers[i];
        unsigned int base = 0;
        bool recognized = false;
        if (count == 1 && c->vendor == 0x8086 && c->ports == 2 && expected == 2)
            recognized = true;
        if (count == 1 && c->vendor == 0x1b21 &&
            ((expected == 4 && c->device == 0x1164) || (expected == 6 && c->device == 0x1166)))
            recognized = true;
        if (count == 2 && (expected == 4 || expected == 6)) {
            const struct controller *other = &controllers[1 - i];
            if (c->vendor == 0x8086 && c->ports == 2 && other->vendor == 0x1b21 &&
                other->ports == expected - 2 &&
                ((expected == 4 && (other->device == 0x0612 || other->device == 0x1062)) ||
                 (expected == 6 && other->device == 0x1164)))
                recognized = true;
            if (c->vendor == 0x1b21 && other->vendor == 0x8086 && other->ports == 2 &&
                c->ports == expected - 2 &&
                ((expected == 4 && (c->device == 0x0612 || c->device == 0x1062)) ||
                 (expected == 6 && c->device == 0x1164))) {
                recognized = true; base = 2;
            }
        }
        if (count == 2 && expected == 8) {
            const struct controller *other = &controllers[1 - i];
            if (c->vendor == 0x1b21 && c->device == 0x1164 && c->ports == 4 && c->bridge &&
                other->vendor == 0x1b21 && other->device == 0x1164 && other->ports == 4 &&
                other->bridge && other->bridge != c->bridge) {
                recognized = true; base = c->bridge == 1 ? 0 : 4;
            }
        }
        if (!recognized)
            return 0;
        if (!strcmp(c->pci, disk->pci))
            return base + disk->ata_port;
    }
    return 0;
}

static bool firmware_rule_matches(const char *root, const char *resolved,
                                   const struct ugreenctl_disk_firmware_layout *layout,
                                   const struct ugreenctl_disk_firmware_rule *rule,
                                   const struct ugreenctl_disk *disk, bool revision_known)
{
    bool nvme = !strcmp(disk->interface_type, "nvme");
    if ((!strcmp(rule->interface_type, "sata")) == nvme)
        return false;
    if (!revision_known && !strcmp(layout->model, "DXP2800 GT") &&
        !gt_rule_revision_independent(rule))
        return false;
    if (!strcmp(rule->kind, "ata_id") && !nvme) {
        unsigned int index;
        return !strncmp(rule->identity, "ata", 3) && !parse_number(rule->identity + 3, &index) &&
               firmware_ata_index(root, layout, disk) == index;
    }
    if (!strcmp(rule->kind, "pcie_bridge_id"))
        return nvme && bridge_identity_unique(root, rule->identity) && bridge_matches(resolved, rule->identity);
    if (!strcmp(rule->kind, "pcie_address")) {
        char needle[PATH_MAX], controller_prefix[PATH_MAX];
        const char *relative = resolved + strlen(root);
        char *last;
        snprintf(needle, sizeof(needle), "/devices/%s", rule->identity);
        if (!strncmp(relative, needle, strlen(needle)) && relative[strlen(needle)] == '/')
            return nvme;
        /* Firmware records the direct endpoint BDF too. Its upstream root port
         * is the stable physical connector; require a direct NVMe endpoint
         * under that port, so changed downstream bus numbering is harmless. */
        last = strrchr(needle, '/');
        if (!last)
            return false;
        *last = '\0';
        snprintf(controller_prefix, sizeof(controller_prefix), "%s/%s/nvme/", needle, disk->pci);
        return nvme && !strncmp(relative, controller_prefix, strlen(controller_prefix));
    }
    if (!strcmp(rule->kind, "syspath")) {
        char prefix[PATH_MAX], component[32];
        const char *separator = strchr(rule->identity, '|');
        const char *relative = resolved + strlen(root) + 1;
        size_t prefix_length;
        if (!separator)
            return false;
        prefix_length = (size_t)(separator - rule->identity);
        if (prefix_length >= sizeof(prefix))
            return false;
        memcpy(prefix, rule->identity, prefix_length); prefix[prefix_length] = '\0';
        if (strncmp(relative, prefix, prefix_length) || relative[prefix_length] != '/')
            return false;
        if (nvme)
            return !strcmp(separator + 1, "nvme");
        if (!revision_known || strncmp(separator + 1, "ata", 3))
            return false;
        snprintf(component, sizeof(component), "%s", separator + 4);
        {
            unsigned int port, count = 0;
            size_t i;
            for (i = 0; i < layout->count; ++i)
                if (!strcmp(layout->rules[i].interface_type, "sata"))
                    ++count;
            return !parse_number(component, &port) && disk->ata_port == port &&
                   controller_ports(root, disk->pci, count);
        }
    }
    /* Global ATA / NVMe enumeration names require a separate stable topology
     * conversion, never a fallback to raw Linux numbering. */
    return false;
}

static bool firmware_slot_has_led(const char *model, const struct ugreenctl_disk_firmware_rule *rule)
{
    if (rule->color_led)
        return true;
    /* Independently decoded physical single/dual-colour LED layouts. The
     * storage service's ColorLed flag means RGB, not LED existence. */
    return !strcmp(rule->interface_type, "sata") &&
           (!strcmp(model, "DH2600") || !strcmp(model, "DH2300") || !strcmp(model, "DH4300 Plus"));
}

static void apply_firmware_rule(const char *model, const struct ugreenctl_disk_firmware_rule *rule,
                                 struct ugreenctl_disk *disk)
{
    const char *digits = rule->slot;
    char source[32] = "official-firmware-pci-rule";
    if (!strcmp(rule->kind, "ata_id")) {
        if (!strcmp(model, "DH2300") || !strcmp(model, "DH4300 Plus"))
            snprintf(source, sizeof(source), "firmware-of-topology");
        else if (!strncmp(disk->mapping_source, "firmware-", 9))
            snprintf(source, sizeof(source), "%s", disk->mapping_source);
        else
            snprintf(source, sizeof(source), "firmware-ata-topology");
    }
    while (*digits && !isdigit((unsigned char)*digits))
        ++digits;
    if (parse_number(digits, &disk->slot) || !disk->slot)
        return;
    disk->mapped = true;
    snprintf(disk->slot_name, sizeof(disk->slot_name), "%s", rule->slot);
    if (!strcmp(rule->interface_type, "u2"))
        snprintf(disk->interface_type, sizeof(disk->interface_type), "u2");
    disk->led_supported = firmware_slot_has_led(model, rule);
    if (disk->led_supported)
        snprintf(disk->led_id, sizeof(disk->led_id), "disk%u", disk->slot);
    else
        disk->led_id[0] = '\0';
    snprintf(disk->mapping_source, sizeof(disk->mapping_source), "%s", source);
    snprintf(disk->reason, sizeof(disk->reason), "%s", disk->led_supported ?
             "Official firmware physical-connector rule; corresponding disk LED recorded" :
             "Official firmware physical-connector rule; no independently controllable disk LED confirmed");
}

static int verify_inventory_members(const char *root, const char *directory,
                                    const struct ugreenctl_disk_scan *scan,
                                    char controllers[UGREENCTL_DISK_MAX][PATH_MAX],
                                    char members[UGREENCTL_DISK_MAX][64], size_t member_count,
                                    char *error, size_t error_size)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    bool seen[UGREENCTL_DISK_MAX] = {false};
    size_t count = 0;
    if (!dir)
        return fail(error, error_size, "Cannot recheck block-device inventory; rescan required");
    errno = 0;
    while ((entry = readdir(dir))) {
        struct ugreenctl_disk current = {0};
        char controller[PATH_MAX];
        size_t member, i;
        bool readable, changed;
        if (!disk_name(entry->d_name) && !nvme_name(entry->d_name)) {
            errno = 0;
            continue;
        }
        for (member = 0; member < member_count; ++member)
            if (!strcmp(entry->d_name, members[member]))
                break;
        if (member == member_count || seen[member]) {
            closedir(dir);
            return fail(error, error_size, "Block-device inventory changed during enumeration; rescan required");
        }
        seen[member] = true;
        ++count;
        readable = read_one(root, entry->d_name, &current, controller) != 0;
        for (i = 0; i < scan->count; ++i)
            if (!strcmp(entry->d_name, scan->disks[i].name))
                break;
        changed = readable != (i < scan->count);
        if (readable && !changed)
            changed = strcmp(current.serial, scan->disks[i].serial) ||
                      strcmp(current.wwn, scan->disks[i].wwn) ||
                      strcmp(current.topology, scan->disks[i].topology) ||
                      strcmp(controller, controllers[i]);
        if (changed) {
            closedir(dir);
            return fail(error, error_size, "Block-device inventory changed during enumeration; rescan required");
        }
        errno = 0;
    }
    if (errno || count != member_count) {
        closedir(dir);
        return fail(error, error_size, "Block-device inventory changed during enumeration; rescan required");
    }
    closedir(dir);
    return 0;
}

/* Identity reads above can block while readdir retains an old directory
 * buffer/EOF cookie. Reopen only after all of those reads have finished, and
 * compare the complete candidate-name set without any slow device attributes.
 * This also includes filtered and not-yet-initialized whole-block candidates.
 */
static int verify_inventory_member_set(const char *directory,
                                        char members[UGREENCTL_DISK_MAX][64], size_t member_count,
                                        char *error, size_t error_size)
{
    DIR *dir;
    struct dirent *entry;
    bool seen[UGREENCTL_DISK_MAX] = {false};
    size_t count = 0;
    if (member_count > UGREENCTL_DISK_MAX)
        return fail(error, error_size, "Invalid block-device member count; rescan required");
    dir = opendir(directory);
    if (!dir)
        return fail(error, error_size, "Cannot recheck final block-device member set; rescan required");
    errno = 0;
    while ((entry = readdir(dir))) {
        size_t member;
        if (!disk_name(entry->d_name) && !nvme_name(entry->d_name))
            continue;
        if (count == UGREENCTL_DISK_MAX || strlen(entry->d_name) >= sizeof(members[0])) {
            closedir(dir);
            return fail(error, error_size, "Too many block-device members in final snapshot; rescan required");
        }
        for (member = 0; member < member_count; ++member)
            if (!strcmp(entry->d_name, members[member]))
                break;
        if (member == member_count || seen[member]) {
            closedir(dir);
            return fail(error, error_size, "Block-device inventory changed during enumeration; rescan required");
        }
        seen[member] = true;
        ++count;
    }
    if (errno || count != member_count) {
        closedir(dir);
        return fail(error, error_size, "Block-device inventory changed during enumeration; rescan required");
    }
    closedir(dir);
    return 0;
}

static int coalesce_nvme_namespaces(struct ugreenctl_disk_scan *scan,
                                    char controllers[UGREENCTL_DISK_MAX][PATH_MAX],
                                    char *error, size_t error_size)
{
    size_t i, physical_count = 0;
    size_t namespace_group[UGREENCTL_DISK_MAX];
    scan->namespace_count = 0;
    for (i = 0; i < scan->count; ++i) {
        struct ugreenctl_disk disk = scan->disks[i];
        char controller[PATH_MAX];
        size_t group;
        snprintf(controller, sizeof(controller), "%s", controllers[i]);
        for (group = 0; group < physical_count; ++group)
            if (controller[0] && controllers[group][0] && !strcmp(controller, controllers[group]))
                break;
        if (group < physical_count) {
            const struct ugreenctl_disk *physical = &scan->disks[group];
            if (strcmp(physical->pci, disk.pci) || strcmp(physical->serial, disk.serial) ||
                strcmp(physical->interface_type, disk.interface_type) ||
                physical->mapped != disk.mapped || physical->slot != disk.slot ||
                physical->led_supported != disk.led_supported ||
                strcmp(physical->slot_name, disk.slot_name) || strcmp(physical->led_id, disk.led_id))
                return fail(error, error_size, "NVMe controller identity/mapping changed across namespaces; rescan required");
            /* Stable representative only; every namespace remains selectable.
             * Its numeric name never chooses a physical connector. */
            if (strcmp(disk.name, physical->name) < 0)
                scan->disks[group] = disk;
        } else {
            scan->disks[group] = disk;
            memmove(controllers[group], controller, strlen(controller) + 1);
            ++physical_count;
        }
        if (controller[0]) {
            size_t alias = scan->namespace_count++;
            snprintf(scan->namespace_aliases[alias].name, sizeof(scan->namespace_aliases[alias].name), "%s", disk.name);
            snprintf(scan->namespace_aliases[alias].wwn, sizeof(scan->namespace_aliases[alias].wwn), "%s", disk.wwn);
            namespace_group[alias] = group;
        }
    }
    for (i = 0; i < scan->namespace_count; ++i)
        snprintf(scan->namespace_aliases[i].disk_name, sizeof(scan->namespace_aliases[i].disk_name),
                 "%s", scan->disks[namespace_group[i]].name);
    scan->count = physical_count;
    return 0;
}

int ugreenctl_disk_scan_at(const char *sysfs_root, const char *dmi_product_name,
                          const char *calibration_path,
                          struct ugreenctl_disk_scan *out,
                          char *error, size_t error_size)
{
    struct calibration cal = {0};
    DIR *dir;
    struct dirent *entry;
    char root[PATH_MAX], directory[PATH_MAX], automatic_pci[16] = "";
    char automatic_reason[160] = "";
    char controllers[UGREENCTL_DISK_MAX][PATH_MAX] = {{0}};
    char members[UGREENCTL_DISK_MAX][64];
    size_t member_count = 0;
    const struct single_sata_profile *single_profile = NULL;
    bool conflicts[UGREENCTL_DISK_MAX] = {false};
    const struct ugreenctl_disk_firmware_layout *layout;
    bool revision_known;
    bool gt_sata_ambiguous = false;
    size_t i, j;
    if (!out || !sysfs_root)
        return fail(error, error_size, "Invalid disk scan arguments");
    memset(out, 0, sizeof(*out));
    if (!dmi_product_name)
        dmi_product_name = "";
    if (!*dmi_product_name) {
        if (calibration_path)
            return fail(error, error_size, "Disk calibration requires a readable nonempty exact DMI product name");
        /* The host may not export DMI (containers/unsupported platforms).
         * Still provide the read-only diagnostic inventory without applying
         * a saved layout belonging to another machine. */
    } else if (read_calibration(calibration_path ? calibration_path : "/etc/ugreenctl/disk-map.conf",
                                calibration_path == NULL, dmi_product_name, &cal, error, error_size))
        return -1;
    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i)
        if (!strcmp(dmi_product_name, profiles[i].model)) {
            out->profile_known = true;
            snprintf(out->profile, sizeof(out->profile), "%s", dmi_product_name);
        }
    if (cal.present) {
        out->profile_known = true;
        snprintf(out->profile, sizeof(out->profile), "administrator-calibration");
    }
    if (!realpath(sysfs_root, root) ||
        snprintf(directory, sizeof(directory), "%s/class/block", root) >= (int)sizeof(directory))
        return fail(error, error_size, "Cannot resolve sysfs root '%s'", sysfs_root);
    layout = firmware_layout(root, dmi_product_name, &revision_known);
    if (layout && !cal.present) {
        out->profile_known = true;
        snprintf(out->profile, sizeof(out->profile), "%s/%s", layout->model,
                 !revision_known && !strcmp(layout->model, "DXP2800 GT") ?
                 "revision-unknown" : layout->revision);
    }
    if (!cal.present) {
        single_profile = single_sata_profile_find(root, dmi_product_name);
        if (single_profile) {
            out->profile_known = true;
            snprintf(out->profile, sizeof(out->profile), "%s", dmi_product_name);
            (void)single_sata_controller(root, single_profile, automatic_pci, sizeof(automatic_pci),
                                          automatic_reason, sizeof(automatic_reason));
        }
    }
    dir = opendir(directory);
    if (!dir)
        return fail(error, error_size, "Cannot enumerate Linux block devices: %s", strerror(errno));
    errno = 0;
    while ((entry = readdir(dir))) {
        struct ugreenctl_disk disk = {0};
        char controller[PATH_MAX];
        /* Track even temporarily unreadable/filtered whole-block candidates.
         * A new NVMe member with incomplete attributes must trigger a rescan
         * before an old shared-bay occupant can be located. */
        if (disk_name(entry->d_name) || nvme_name(entry->d_name)) {
            if (member_count == UGREENCTL_DISK_MAX || strlen(entry->d_name) >= sizeof(members[0])) {
                closedir(dir);
                memset(out, 0, sizeof(*out));
                return fail(error, error_size, "Too many block-device members; refusing an incomplete mapping");
            }
            snprintf(members[member_count++], sizeof(members[0]), "%s", entry->d_name);
        }
        if (!read_one(root, entry->d_name, &disk, controller)) {
            errno = 0;
            continue;
        }
        if (out->count == UGREENCTL_DISK_MAX) {
            closedir(dir);
            memset(out, 0, sizeof(*out));
            return fail(error, error_size, "Too many physical disks/NVMe namespaces; refusing an incomplete mapping");
        }
        snprintf(disk.mapping_source, sizeof(disk.mapping_source), "unknown");
        if (disk.topology[0]) {
            if (cal.present) {
                /* Explicit calibration is a complete replacement; never mix it
                 * with a built-in layout for omitted slots. */
                for (i = 0; i < cal.count; ++i)
                    if (!strcmp(disk.topology, cal.topology[i])) {
                        disk.slot = cal.slot[i];
                        snprintf(disk.mapping_source, sizeof(disk.mapping_source), "administrator-calibration");
                    }
            } else {
                /* Historical endpoint BDFs are evidence records only. Runtime
                 * SATA mapping now requires the full factory controller-role
                 * conversion below, so moved buses/add-in cards cannot inherit
                 * another controller's chassis LEDs. */
                if (automatic_pci[0] && !strcmp(disk.pci, automatic_pci) &&
                    disk.ata_port >= 1 && disk.ata_port <= single_profile->port_count) {
                    disk.slot = disk.ata_port;
                    snprintf(disk.mapping_source, sizeof(disk.mapping_source), "%s", single_profile->source);
                }
            }
            if (!disk.slot)
                snprintf(disk.reason, sizeof(disk.reason), "%s",
                         !cal.present && !strcmp(disk.interface_type, "nvme") ?
                         "No verified physical NVMe connector layout for this exact model; run scripts/collect-disk-evidence.sh" :
                         automatic_reason[0] ? automatic_reason :
                         out->profile_known ? "Topology is outside the confirmed chassis layout" :
                         "No confirmed PCI/ATA slot layout for this exact model; calibration required");
        }
        if (disk.slot) {
            disk.mapped = true;
            snprintf(disk.led_id, sizeof(disk.led_id), "disk%u", disk.slot);
            disk.led_supported = true;
            snprintf(disk.slot_name, sizeof(disk.slot_name), "ata%u", disk.slot);
            snprintf(disk.reason, sizeof(disk.reason), "%s", cal.present ?
                     "Administrator-confirmed physical topology" :
                     single_profile && !strcmp(disk.mapping_source, single_profile->source) ?
                     "Stock firmware slot order; local-port conversion checked against the complete single-controller topology" :
                     "Upstream physical slot evidence; verify after hardware changes");
        }
        if (layout) {
            char block_path[PATH_MAX], resolved[PATH_MAX];
            const struct ugreenctl_disk_firmware_rule *matched = NULL;
            unsigned int matches = 0;
            snprintf(block_path, sizeof(block_path), "%s/class/block/%s", root, disk.name);
            if (realpath(block_path, resolved))
                for (i = 0; i < layout->count; ++i)
                    if (firmware_rule_matches(root, resolved, layout, &layout->rules[i], &disk, revision_known)) {
                        matched = &layout->rules[i]; ++matches;
                    }
            if (matches == 1 && !cal.present)
                apply_firmware_rule(dmi_product_name, matched, &disk);
            else if (matches > 1) {
                disk.slot = 0; disk.mapped = false; disk.led_supported = false;
                disk.led_id[0] = '\0'; disk.slot_name[0] = '\0';
                snprintf(disk.reason, sizeof(disk.reason), "Conflicting firmware connector rules; refusing to choose a slot");
            }
            if (!matches && !cal.present && !disk.mapped) {
                if (!strcmp(disk.interface_type, "nvme")) {
                    unsigned int physical_rules = 0, block_name_rules = 0;
                    for (i = 0; i < layout->count; ++i)
                        if (strcmp(layout->rules[i].interface_type, "sata")) {
                            ++physical_rules;
                            if (!strcmp(layout->rules[i].kind, "block_name")) ++block_name_rules;
                        }
                    snprintf(disk.reason, sizeof(disk.reason), "%s",
                             physical_rules && physical_rules == block_name_rules ?
                             "Firmware has nvmeN names only; physical M.2 slot unconfirmed. Run scripts/collect-disk-evidence.sh for sysfs/SMBIOS evidence" :
                             "PCIe connector missing/ambiguous or outside firmware layout; collect topology with scripts/collect-disk-evidence.sh");
                } else if (!revision_known && !strcmp(dmi_product_name, "DXP2800 GT"))
                    snprintf(disk.reason, sizeof(disk.reason), "BIOS/SATA routing is unverified for DXP2800 GT; automatic SATA mapping disabled; explicit administrator calibration required");
                else if (!strcmp(dmi_product_name, "DH2300") || !strcmp(dmi_product_name, "DH4300 Plus"))
                    snprintf(disk.reason, sizeof(disk.reason), "Live DT/OF/ASIC/empty-port inventory does not match the complete factory ARM layout");
            }
            /* For old stable SATA converters, use the firmware's per-slot LED
             * capability record too. Never fabricate RGB for a plain GPIO bay. */
            if (disk.mapped && !strcmp(disk.interface_type, "sata"))
                for (i = 0; i < layout->count; ++i)
                    if (!strcmp(layout->rules[i].interface_type, "sata") &&
                        !strcmp(layout->rules[i].slot, disk.slot_name)) {
                        disk.led_supported = firmware_slot_has_led(dmi_product_name, &layout->rules[i]);
                        if (!disk.led_supported) disk.led_id[0] = '\0';
                    }
        }
        snprintf(controllers[out->count], sizeof(controllers[0]), "%s", controller);
        out->disks[out->count++] = disk;
        errno = 0;
    }
    if (errno) {
        closedir(dir);
        memset(out, 0, sizeof(*out));
        return fail(error, error_size, "Block-device enumeration failed");
    }
    closedir(dir);
    /* Recheck every identity after the whole inventory is collected. A disk
     * replaced while a later entry is scanned must not retain an old serial.
     * Re-evaluate firmware topology too, including empty multi-controller
     * ports and bridge roles, before exposing a locatable result. */
    for (i = 0; i < out->count; ++i) {
        struct ugreenctl_disk current = {0};
        struct ugreenctl_disk *disk = &out->disks[i];
        char controller[PATH_MAX];
        if (!read_one(root, disk->name, &current, controller) ||
            strcmp(current.serial, disk->serial) || strcmp(current.wwn, disk->wwn) ||
            strcmp(current.topology, disk->topology) || strcmp(controller, controllers[i])) {
            memset(out, 0, sizeof(*out));
            return fail(error, error_size, "Disk identity/topology changed during enumeration; rescan required");
        }
        if (layout && !cal.present && disk->mapped) {
            char block_path[PATH_MAX], resolved[PATH_MAX];
            const struct ugreenctl_disk_firmware_rule *matched = NULL;
            unsigned int matches = 0;
            snprintf(block_path, sizeof(block_path), "%s/class/block/%s", root, disk->name);
            if (realpath(block_path, resolved))
                for (j = 0; j < layout->count; ++j)
                    if (firmware_rule_matches(root, resolved, layout, &layout->rules[j], &current, revision_known)) {
                        matched = &layout->rules[j]; ++matches;
                    }
            if (matches == 1)
                apply_firmware_rule(dmi_product_name, matched, &current);
            if (matches != 1 || strcmp(current.slot_name, disk->slot_name) ||
                strcmp(current.led_id, disk->led_id)) {
                disk->mapped = false; disk->led_supported = false; disk->slot = 0;
                disk->led_id[0] = '\0'; disk->slot_name[0] = '\0';
                snprintf(disk->mapping_source, sizeof(disk->mapping_source), "unknown");
                snprintf(disk->reason, sizeof(disk->reason), "Controller topology changed during enumeration; rescan required");
            }
        }
    }
    if (automatic_pci[0]) {
        char current_pci[16], current_reason[160];
        if (single_sata_controller(root, single_profile, current_pci, sizeof(current_pci), current_reason, sizeof(current_reason)) ||
            strcmp(current_pci, automatic_pci)) {
            for (i = 0; i < out->count; ++i)
                if (!strcmp(out->disks[i].mapping_source, single_profile->source)) {
                    out->disks[i].mapped = false;
                    out->disks[i].led_supported = false;
                    out->disks[i].slot = 0;
                    out->disks[i].led_id[0] = '\0';
                    out->disks[i].slot_name[0] = '\0';
                    snprintf(out->disks[i].mapping_source, sizeof(out->disks[i].mapping_source), "unknown");
                    snprintf(out->disks[i].reason, sizeof(out->disks[i].reason), "Controller topology changed during enumeration; rescan required");
                }
        }
    }
    /* Existing identities alone cannot detect a newly arrived U.2/SATA peer or
     * namespace. Refuse any changed physical-block membership before exposing
     * slot/LED claims; a caller may request a fresh bounded scan. */
    if (verify_inventory_members(root, directory, out, controllers, members, member_count, error, error_size) ||
        verify_inventory_member_set(directory, members, member_count, error, error_size) ||
        coalesce_nvme_namespaces(out, controllers, error, error_size)) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    if (!cal.present && !revision_known && !strcmp(dmi_product_name, "DXP2800 GT")) {
        for (i = 0; i < out->count; ++i)
            if (!out->disks[i].mapped && !strcmp(out->disks[i].interface_type, "sata"))
                gt_sata_ambiguous = true;
        /* The two SATA revisions exchange the shared disk1/disk2 bays. With
         * either possible route still unknown, an installed SATA disk may
         * share either U.2 LED; never guess from its ATA enumeration/port. */
        if (gt_sata_ambiguous)
            for (i = 0; i < out->count; ++i)
                if (!strcmp(out->disks[i].interface_type, "u2"))
                    conflicts[i] = true;
    }
    /* Compare all candidates before invalidating any, so a three-way conflict
     * cannot leave the third device mapped after clearing the first pair. */
    for (i = 0; i < out->count; ++i)
        for (j = i + 1; j < out->count; ++j)
            if ((out->disks[i].topology[0] &&
                 !strcmp(out->disks[i].topology, out->disks[j].topology)) ||
                (out->disks[i].slot_name[0] && !strcmp(out->disks[i].slot_name, out->disks[j].slot_name)) ||
                (out->disks[i].led_id[0] && !strcmp(out->disks[i].led_id, out->disks[j].led_id)))
                conflicts[i] = conflicts[j] = true;
    for (i = 0; i < out->count; ++i)
        if (conflicts[i]) {
            struct ugreenctl_disk *disk = &out->disks[i];
            disk->mapped = false;
            disk->led_supported = false;
            disk->slot = 0;
            disk->led_id[0] = '\0';
            disk->slot_name[0] = '\0';
            snprintf(disk->mapping_source, sizeof(disk->mapping_source), "unknown");
            snprintf(disk->reason, sizeof(disk->reason), "%s",
                     gt_sata_ambiguous && !strcmp(disk->interface_type, "u2") ?
                     "Unverified BIOS/SATA routing may share this U.2 bay; refusing to choose a disk LED" :
                     "Conflicting or duplicate topology/slot; refusing to choose an LED");
        }
    qsort(out->disks, out->count, sizeof(out->disks[0]), compare_disks);
    return 0;
}

#else

int ugreenctl_disk_scan_at(const char *sysfs_root, const char *dmi_product_name,
                          const char *calibration_path,
                          struct ugreenctl_disk_scan *out,
                          char *error, size_t error_size)
{
    (void)sysfs_root;
    (void)dmi_product_name;
    (void)calibration_path;
    (void)profiles;
    if (out)
        memset(out, 0, sizeof(*out));
    return fail(error, error_size, "Physical ATA disk scanning requires Linux sysfs");
}

#endif

int ugreenctl_disk_scan(const char *dmi_product_name, const char *calibration_path,
                       struct ugreenctl_disk_scan *out,
                       char *error, size_t error_size)
{
    return ugreenctl_disk_scan_at("/sys", dmi_product_name, calibration_path,
                                  out, error, error_size);
}
