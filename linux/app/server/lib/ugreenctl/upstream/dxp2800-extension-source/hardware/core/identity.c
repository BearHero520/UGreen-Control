#define _POSIX_C_SOURCE 200809L
#include "core/identity.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int failure(char *error, size_t size, int result, const char *field)
{
    if (error && size) (void)snprintf(error, size, "cannot read machine identity %s: %s", field, strerror(-result));
    return result;
}

static int raw_read(const char *root, const char *field, unsigned char *out,
                    size_t capacity, size_t *length)
{
    char path[4096];
    FILE *stream;
    int extra, n;
    *length = 0;
    n = snprintf(path, sizeof(path), "%s/%s", root, field);
    if (n < 0 || (size_t)n >= sizeof(path)) return -ENAMETOOLONG;
    stream = fopen(path, "rb");
    if (!stream) return -errno;
    *length = fread(out, 1, capacity, stream);
    extra = fgetc(stream);
    if (ferror(stream)) { (void)fclose(stream); return -EIO; }
    (void)fclose(stream);
    return extra == EOF ? 0 : -EOVERFLOW;
}

static int text_read(const char *root, const char *field, char *out,
                     size_t capacity, bool dt)
{
    size_t length, i;
    int result = raw_read(root, field, (unsigned char *)out, capacity - 1, &length);
    if (result) { out[0] = 0; return result; }
    if (dt && length && out[length - 1] == 0) --length;
    for (i = 0; i < length; ++i)
        if (out[i] == 0 || ((unsigned char)out[i] < 0x20 && out[i] != '\n' && out[i] != '\r' && out[i] != '\t')) {
            out[0] = 0; return -EBADMSG;
        }
    while (length && isspace((unsigned char)out[length - 1])) --length;
    out[length] = 0;
    return 0;
}

static int compatible_read(const char *root, char *out, size_t capacity)
{
    unsigned char raw[512];
    size_t length, used = 0, begin = 0, i;
    int result = raw_read(root, "firmware/devicetree/base/compatible", raw, sizeof(raw), &length);
    out[0] = 0;
    if (result) return result;
    if (!length || raw[length - 1] != 0) return -EBADMSG;
    for (i = 0; i < length; ++i) {
        if (raw[i]) {
            if (raw[i] < 0x21 || raw[i] > 0x7e || raw[i] == '|') return -EBADMSG;
            continue;
        }
        if (i == begin || used + (used ? 1 : 0) + i - begin >= capacity) return -EBADMSG;
        if (used) out[used++] = '|';
        memcpy(out + used, raw + begin, i - begin);
        used += i - begin;
        begin = i + 1;
    }
    out[used] = 0;
    return 0;
}

int ugreenctl_identity_read_at(const char *root,
                              struct ugreenctl_machine_identity *identity,
                              char *error, size_t size)
{
    struct ugreenctl_machine_identity found = {0};
    int result;
    if (error && size) error[0] = 0;
    if (!root || !identity) return failure(error, size, -EINVAL, "arguments");
    memset(identity, 0, sizeof(*identity));
    result = text_read(root, "class/dmi/id/product_name", found.product_name, sizeof(found.product_name), false);
    if (result && result != -ENOENT && result != -ENOTDIR) return failure(error, size, result, "DMI product_name");
    if (found.product_name[0]) {
        strcpy(found.source, "dmi");
        result = text_read(root, "class/dmi/id/product_version", found.product_version, sizeof(found.product_version), false);
        if (result && result != -ENOENT) return failure(error, size, result, "DMI product_version");
        result = text_read(root, "class/dmi/id/sys_vendor", found.manufacturer, sizeof(found.manufacturer), false);
        if (result && result != -ENOENT) return failure(error, size, result, "DMI sys_vendor");
        result = text_read(root, "class/dmi/id/board_version", found.board_version, sizeof(found.board_version), false);
        if (result && result != -ENOENT) return failure(error, size, result, "DMI board_version");
        result = text_read(root, "class/dmi/id/bios_vendor", found.bios_vendor, sizeof(found.bios_vendor), false);
        if (result && result != -ENOENT) return failure(error, size, result, "DMI bios_vendor");
        result = text_read(root, "class/dmi/id/bios_version", found.bios_version, sizeof(found.bios_version), false);
        if (result && result != -ENOENT) return failure(error, size, result, "DMI bios_version");
        result = text_read(root, "class/dmi/id/bios_date", found.bios_date, sizeof(found.bios_date), false);
        if (result && result != -ENOENT) return failure(error, size, result, "DMI bios_date");
    } else {
        result = text_read(root, "firmware/devicetree/base/model", found.product_name, sizeof(found.product_name), true);
        if (result) return failure(error, size, result, "device-tree model");
        if (!found.product_name[0]) return failure(error, size, -ENODEV, "empty device-tree model");
        result = compatible_read(root, found.compatible, sizeof(found.compatible));
        if (result) return failure(error, size, result, "device-tree compatible");
        strcpy(found.source, "devicetree");
    }
    *identity = found;
    return 0;
}

int ugreenctl_identity_read(struct ugreenctl_machine_identity *identity,
                           char *error, size_t size)
{
    return ugreenctl_identity_read_at("/sys", identity, error, size);
}

bool ugreenctl_identity_has_compatible(const struct ugreenctl_machine_identity *identity,
                                      const char *token)
{
    const char *begin, *end;
    size_t length;
    if (!identity || !token || !token[0] || strchr(token, '|')) return false;
    length = strlen(token);
    for (begin = identity->compatible; *begin; begin = end + 1) {
        end = strchr(begin, '|');
        if (!end) return strlen(begin) == length && memcmp(begin, token, length) == 0;
        if ((size_t)(end - begin) == length && memcmp(begin, token, length) == 0) return true;
    }
    return false;
}

const char *ugreenctl_identity_model(const struct ugreenctl_machine_identity *identity)
{
    if (!identity) return "";
    if (!strcmp(identity->source, "devicetree")) {
        if (!strcmp(identity->product_name, "UGREEN RK3576 DH2300 Board") &&
            ugreenctl_identity_has_compatible(identity, "rockchip,rk3576")) return "DH2300";
        if (!strcmp(identity->product_name, "UGREEN RK3588C DH4300Plus Board") &&
            ugreenctl_identity_has_compatible(identity, "rockchip,rk3588")) return "DH4300 Plus";
    }
    return identity->product_name;
}
