#define _DEFAULT_SOURCE

#include "network/wol.h"

#include <dirent.h>
#include <errno.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define UGREENCTL_WOL_MAX_RUNTIME_INTERFACES 8U

struct runtime_interface {
    char name[IFNAMSIZ];
    char pci_slot[32];
};

static int validate_interfaces(const char * const *interfaces, size_t interface_count,
                               char *error, size_t error_size);

static int read_pci_ethernet_interface(const char *interface,
                                       struct runtime_interface *candidate)
{
    char path[512];
    char line[256];
    FILE *stream;
    int length;
    int is_pci_ethernet = 0;
    int has_slot = 0;

    length = snprintf(path, sizeof(path), "/sys/class/net/%s/device/uevent", interface);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        return 0;
    }
    stream = fopen(path, "r");
    if (stream == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), stream) != NULL) {
        if (strncmp(line, "PCI_CLASS=", 10) == 0) {
            char *end;
            unsigned long pci_class;

            errno = 0;
            pci_class = strtoul(line + 10, &end, 16);
            if (errno == 0 && end != line + 10 && (pci_class >> 8U) == 0x0200U) {
                is_pci_ethernet = 1;
            }
        } else if (strncmp(line, "PCI_SLOT_NAME=", 14) == 0) {
            size_t slot_length = strcspn(line + 14, "\r\n");

            if (slot_length > 0 && slot_length < sizeof(candidate->pci_slot)) {
                memcpy(candidate->pci_slot, line + 14, slot_length);
                candidate->pci_slot[slot_length] = '\0';
                has_slot = 1;
            }
        }
    }
    (void)fclose(stream);
    if (!is_pci_ethernet || !has_slot) {
        return 0;
    }
    (void)snprintf(candidate->name, sizeof(candidate->name), "%s", interface);
    return 1;
}

static int compare_runtime_interfaces(const void *left, const void *right)
{
    const struct runtime_interface *first = left;
    const struct runtime_interface *second = right;
    int result = strcmp(first->pci_slot, second->pci_slot);

    return result != 0 ? result : strcmp(first->name, second->name);
}

static int mapped_interfaces_are_present_and_physical(const char * const *interfaces,
                                                       size_t interface_count)
{
    size_t index;

    for (index = 0; index < interface_count; ++index) {
        struct runtime_interface candidate = {{0}, {0}};

        if (if_nametoindex(interfaces[index]) == 0U ||
            !read_pci_ethernet_interface(interfaces[index], &candidate)) {
            return 0;
        }
    }
    return 1;
}

static int discover_renamed_interfaces(size_t expected_count,
                                       char names[][IFNAMSIZ],
                                       const char **interfaces,
                                       size_t *interface_count,
                                       char *error, size_t error_size)
{
    struct runtime_interface candidates[UGREENCTL_WOL_MAX_RUNTIME_INTERFACES];
    DIR *directory;
    struct dirent *entry;
    size_t count = 0;
    size_t index;

    directory = opendir("/sys/class/net");
    if (directory == NULL) {
        (void)snprintf(error, error_size,
                       "cannot inspect physical Ethernet interfaces: %s", strerror(errno));
        return -errno;
    }
    while ((entry = readdir(directory)) != NULL) {
        struct runtime_interface candidate = {{0}, {0}};

        if (entry->d_name[0] == '.' || strlen(entry->d_name) >= IFNAMSIZ ||
            !read_pci_ethernet_interface(entry->d_name, &candidate)) {
            continue;
        }
        if (count >= UGREENCTL_WOL_MAX_RUNTIME_INTERFACES) {
            (void)closedir(directory);
            (void)snprintf(error, error_size,
                           "too many physical PCI Ethernet interfaces to map Wake-on-LAN safely");
            return -E2BIG;
        }
        candidates[count++] = candidate;
    }
    (void)closedir(directory);
    if (count != expected_count) {
        (void)snprintf(error, error_size,
                       "firmware Wake-on-LAN names are absent and found %zu physical PCI Ethernet "
                       "interfaces (expected %zu)", count, expected_count);
        return -ENODEV;
    }
    qsort(candidates, count, sizeof(candidates[0]), compare_runtime_interfaces);
    for (index = 0; index < count; ++index) {
        memcpy(names[index], candidates[index].name, IFNAMSIZ);
        names[index][IFNAMSIZ - 1U] = '\0';
        interfaces[index] = names[index];
    }
    *interface_count = count;
    return 0;
}

static int select_interfaces(const char * const *firmware_interfaces,
                             size_t firmware_interface_count,
                             size_t renamed_interface_count,
                             char names[][IFNAMSIZ], const char **interfaces,
                             size_t *interface_count,
                             char *error, size_t error_size)
{
    int result = validate_interfaces(firmware_interfaces, firmware_interface_count,
                                     error, error_size);

    if (result != 0) {
        return result;
    }
    if (renamed_interface_count == 0 ||
        renamed_interface_count > UGREENCTL_WOL_MAX_RUNTIME_INTERFACES) {
        (void)snprintf(error, error_size,
                       "invalid renamed Wake-on-LAN interface count for this model");
        return -EINVAL;
    }
    if (mapped_interfaces_are_present_and_physical(firmware_interfaces,
                                                    firmware_interface_count)) {
        memcpy(interfaces, firmware_interfaces,
               firmware_interface_count * sizeof(interfaces[0]));
        *interface_count = firmware_interface_count;
        return 0;
    }
    return discover_renamed_interfaces(renamed_interface_count, names, interfaces,
                                       interface_count, error, error_size);
}

static int validate_interfaces(const char * const *interfaces, size_t interface_count,
                               char *error, size_t error_size)
{
    size_t index;

    if (interfaces == NULL || interface_count == 0) {
        (void)snprintf(error, error_size, "no Wake-on-LAN interfaces are mapped for this model");
        return -ENODEV;
    }
    for (index = 0; index < interface_count; ++index) {
        if (interfaces[index] == NULL || interfaces[index][0] == '\0' ||
            strlen(interfaces[index]) >= IFNAMSIZ) {
            (void)snprintf(error, error_size, "invalid Wake-on-LAN interface mapping");
            return -EINVAL;
        }
    }
    return 0;
}

static int query_interface(int fd, const char *interface, struct ethtool_wolinfo *wol,
                           char *error, size_t error_size)
{
    struct ifreq request;

    memset(&request, 0, sizeof(request));
    (void)snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
    memset(wol, 0, sizeof(*wol));
    wol->cmd = ETHTOOL_GWOL;
    request.ifr_data = (char *)wol;
    if (ioctl(fd, SIOCETHTOOL, &request) != 0) {
        (void)snprintf(error, error_size, "cannot read Wake-on-LAN on %s: %s",
                       interface, strerror(errno));
        return -errno;
    }
    if ((wol->supported & WAKE_MAGIC) == 0U) {
        (void)snprintf(error, error_size,
                       "interface %s does not support magic-packet Wake-on-LAN", interface);
        return -EOPNOTSUPP;
    }
    return 0;
}

int ugreenctl_wol_read_policy_with_renamed_count(const char * const *interfaces,
                                                 size_t interface_count,
                                                 size_t renamed_interface_count,
                                                 enum ugreenctl_wol_policy *policy,
                                                 char *error, size_t error_size)
{
    char runtime_names[UGREENCTL_WOL_MAX_RUNTIME_INTERFACES][IFNAMSIZ] = {{0}};
    const char *mapped_interfaces[UGREENCTL_WOL_MAX_RUNTIME_INTERFACES] = {NULL};
    enum ugreenctl_wol_policy observed = UGREENCTL_WOL_UNKNOWN;
    int fd;
    int result;
    size_t index;

    if (policy == NULL) {
        (void)snprintf(error, error_size, "Wake-on-LAN policy output is required");
        return -EINVAL;
    }
    if (interface_count > UGREENCTL_WOL_MAX_RUNTIME_INTERFACES ||
        renamed_interface_count > UGREENCTL_WOL_MAX_RUNTIME_INTERFACES) {
        (void)snprintf(error, error_size, "too many Wake-on-LAN interfaces are mapped for this model");
        return -E2BIG;
    }
    result = select_interfaces(interfaces, interface_count, renamed_interface_count,
                               runtime_names, mapped_interfaces,
                               &interface_count, error, error_size);
    if (result != 0) {
        return result;
    }
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        (void)snprintf(error, error_size, "cannot open Wake-on-LAN control socket: %s",
                       strerror(errno));
        return -errno;
    }
    for (index = 0; index < interface_count; ++index) {
        struct ethtool_wolinfo wol;
        enum ugreenctl_wol_policy current;

        result = query_interface(fd, mapped_interfaces[index], &wol, error, error_size);
        if (result != 0) {
            (void)close(fd);
            return result;
        }
        current = (wol.wolopts & WAKE_MAGIC) != 0U ? UGREENCTL_WOL_ON : UGREENCTL_WOL_OFF;
        if (observed == UGREENCTL_WOL_UNKNOWN) {
            observed = current;
        } else if (observed != current) {
            (void)close(fd);
            (void)snprintf(error, error_size,
                           "Wake-on-LAN state is mixed across the model's mapped interfaces");
            return -EUCLEAN;
        }
    }
    (void)close(fd);
    *policy = observed;
    return 0;
}

int ugreenctl_wol_read_policy(const char * const *interfaces, size_t interface_count,
                              enum ugreenctl_wol_policy *policy,
                              char *error, size_t error_size)
{
    return ugreenctl_wol_read_policy_with_renamed_count(interfaces, interface_count,
                                                         interface_count, policy,
                                                         error, error_size);
}

int ugreenctl_wol_set_policy_with_renamed_count(const char * const *interfaces,
                                                size_t interface_count,
                                                size_t renamed_interface_count,
                                                enum ugreenctl_wol_policy policy,
                                                char *error, size_t error_size)
{
    char runtime_names[UGREENCTL_WOL_MAX_RUNTIME_INTERFACES][IFNAMSIZ] = {{0}};
    const char *mapped_interfaces[UGREENCTL_WOL_MAX_RUNTIME_INTERFACES] = {NULL};
    unsigned int desired;
    int fd;
    int result;
    size_t index;

    if (policy == UGREENCTL_WOL_ON) {
        desired = WAKE_MAGIC;
    } else if (policy == UGREENCTL_WOL_OFF) {
        desired = 0U;
    } else {
        (void)snprintf(error, error_size, "Wake-on-LAN policy must be on or off");
        return -EINVAL;
    }
    if (interface_count > UGREENCTL_WOL_MAX_RUNTIME_INTERFACES ||
        renamed_interface_count > UGREENCTL_WOL_MAX_RUNTIME_INTERFACES) {
        (void)snprintf(error, error_size, "too many Wake-on-LAN interfaces are mapped for this model");
        return -E2BIG;
    }
    result = select_interfaces(interfaces, interface_count, renamed_interface_count,
                               runtime_names, mapped_interfaces,
                               &interface_count, error, error_size);
    if (result != 0) {
        return result;
    }
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        (void)snprintf(error, error_size, "cannot open Wake-on-LAN control socket: %s",
                       strerror(errno));
        return -errno;
    }
    for (index = 0; index < interface_count; ++index) {
        struct ethtool_wolinfo wol;

        result = query_interface(fd, mapped_interfaces[index], &wol, error, error_size);
        if (result != 0) {
            (void)close(fd);
            return result;
        }
    }
    for (index = 0; index < interface_count; ++index) {
        struct ethtool_wolinfo wol;
        struct ifreq request;

        memset(&request, 0, sizeof(request));
        (void)snprintf(request.ifr_name, sizeof(request.ifr_name), "%s",
                       mapped_interfaces[index]);
        memset(&wol, 0, sizeof(wol));
        wol.cmd = ETHTOOL_SWOL;
        wol.wolopts = desired;
        request.ifr_data = (char *)&wol;
        if (ioctl(fd, SIOCETHTOOL, &request) != 0) {
            (void)snprintf(error, error_size, "cannot set Wake-on-LAN on %s: %s",
                           mapped_interfaces[index], strerror(errno));
            (void)close(fd);
            return -errno;
        }
    }
    (void)close(fd);
    result = ugreenctl_wol_read_policy(mapped_interfaces, interface_count, &policy,
                                       error, error_size);
    if (result != 0) {
        return result;
    }
    if ((policy == UGREENCTL_WOL_ON ? WAKE_MAGIC : 0U) != desired) {
        (void)snprintf(error, error_size, "Wake-on-LAN readback did not match requested policy");
        return -EIO;
    }
    return 0;
}

int ugreenctl_wol_set_policy(const char * const *interfaces, size_t interface_count,
                             enum ugreenctl_wol_policy policy,
                             char *error, size_t error_size)
{
    return ugreenctl_wol_set_policy_with_renamed_count(interfaces, interface_count,
                                                        interface_count, policy,
                                                        error, error_size);
}
