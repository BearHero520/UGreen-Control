#define _XOPEN_SOURCE 700
#include "disk/disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef __linux__
#include <ftw.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void test_selectors(void)
{
    struct ugreenctl_disk_scan scan = {0};
    const struct ugreenctl_disk *disk;
    char error[256];
    scan.count = 2;
    snprintf(scan.disks[0].name, sizeof(scan.disks[0].name), "sdz");
    snprintf(scan.disks[0].device, sizeof(scan.disks[0].device), "/dev/sdz");
    snprintf(scan.disks[0].serial, sizeof(scan.disks[0].serial), "SN 123");
    snprintf(scan.disks[0].wwn, sizeof(scan.disks[0].wwn), "naa.500123");
    scan.disks[0].mapped = true;
    scan.disks[0].slot = 3;
    scan.disks[0].led_supported = true;
    snprintf(scan.disks[0].led_id, sizeof(scan.disks[0].led_id), "disk3");
    CHECK(ugreenctl_disk_resolve(&scan, "/dev/sdz", &disk, error, sizeof(error)) == 0);
    CHECK(disk == &scan.disks[0]);
    CHECK(ugreenctl_disk_resolve(&scan, "sdz", &disk, error, sizeof(error)) == 0);
    CHECK(ugreenctl_disk_resolve(&scan, "serial:SN 123", &disk, error, sizeof(error)) == 0);
    CHECK(ugreenctl_disk_resolve(&scan, "wwn:naa.500123", &disk, error, sizeof(error)) == 0);
    CHECK(ugreenctl_disk_resolve(&scan, "slot3", &disk, error, sizeof(error)) == 0);
    CHECK(ugreenctl_disk_resolve(&scan, "disk3", &disk, error, sizeof(error)) == 0);
    CHECK(ugreenctl_disk_resolve(&scan, "slot1", &disk, error, sizeof(error)) != 0);
    CHECK(disk == NULL);
    snprintf(scan.disks[1].serial, sizeof(scan.disks[1].serial), "SN 123");
    CHECK(ugreenctl_disk_resolve(&scan, "serial:SN 123", &disk, error, sizeof(error)) != 0);
    CHECK(strstr(error, "ambiguous") != NULL);
    scan.disks[0].mapped = false;
    CHECK(ugreenctl_disk_resolve(&scan, "sdz", &disk, error, sizeof(error)) != 0);
    CHECK(disk == NULL);
    CHECK(ugreenctl_disk_resolve(NULL, "sdz", &disk, error, sizeof(error)) != 0);
}

#ifdef __linux__

static char fixture[256];
static const struct ugreenctl_disk *find_disk(const struct ugreenctl_disk_scan *scan, const char *name);

static void mkdirs(const char *path)
{
    char temp[4096];
    char *p;
    snprintf(temp, sizeof(temp), "%s", path);
    for (p = temp + 1; *p; ++p)
        if (*p == '/') {
            *p = '\0';
            CHECK(mkdir(temp, 0700) == 0 || errno == EEXIST);
            *p = '/';
        }
    CHECK(mkdir(temp, 0700) == 0 || errno == EEXIST);
}

static void write_file(const char *path, const char *text)
{
    FILE *file;
    char parent[4096], *slash;
    snprintf(parent, sizeof(parent), "%s", path);
    slash = strrchr(parent, '/');
    CHECK(slash != NULL);
    *slash = '\0';
    mkdirs(parent);
    file = fopen(path, "w");
    CHECK(file != NULL);
    CHECK(fputs(text, file) >= 0);
    CHECK(fclose(file) == 0);
}

static void write_vpd(const char *name, const unsigned char *bytes, size_t length)
{
    char path[4096];
    FILE *file;
    snprintf(path, sizeof(path), "%s/class/block/%s/device/vpd_pg80", fixture, name);
    file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(bytes, 1, length, file) == length);
    CHECK(fclose(file) == 0);
}

static void write_binary(const char *path, const void *bytes, size_t length)
{
    FILE *file;
    write_file(path, "");
    file = fopen(path, "wb");
    CHECK(file != NULL && fwrite(bytes, 1, length, file) == length && fclose(file) == 0);
}

static void remove_serial_attribute(const char *name)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/class/block/%s/device/serial", fixture, name);
    CHECK(unlink(path) == 0);
}

static void setup(void)
{
    char path[4096];
    snprintf(fixture, sizeof(fixture), "/tmp/ugreenctl-disk-test-XXXXXX");
    CHECK(mkdtemp(fixture) != NULL);
    snprintf(path, sizeof(path), "%s/class/block", fixture);
    mkdirs(path);
}

static int remove_entry(const char *path, const struct stat *s, int type, struct FTW *f)
{
    (void)s; (void)type; (void)f;
    return remove(path);
}

static void teardown(void)
{
    CHECK(strncmp(fixture, "/tmp/ugreenctl-disk-test-", 24) == 0);
    CHECK(nftw(fixture, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
}

static void add_disk(const char *name, const char *pci, unsigned int ata,
                     unsigned int port, unsigned int host, const char *serial)
{
    char base[4096], path[4096], link[4096], value[32], parent[4096];
    snprintf(path, sizeof(path), "%s/bus/pci/devices/%s", fixture, pci);
    if (!realpath(path, parent))
        snprintf(parent, sizeof(parent), "%s/devices/pci0000:00/%s", fixture, pci);
    snprintf(base, sizeof(base), "%s/ata%u/host%u/target%u:0:0/%u:0:0:0/block/%s",
             parent, ata, host, host, host, name);
    mkdirs(base);
    snprintf(path, sizeof(path), "%s/device/type", base);
    write_file(path, "0\n");
    snprintf(path, sizeof(path), "%s/device/vendor", base);
    write_file(path, "ATA     \n");
    snprintf(path, sizeof(path), "%s/device/serial", base);
    write_file(path, serial);
    snprintf(path, sizeof(path), "%s/wwid", base);
    write_file(path, "naa.5001234\n");
    snprintf(link, sizeof(link), "%s/class/block/%s", fixture, name);
    CHECK(symlink(base, link) == 0);
    snprintf(path, sizeof(path), "%s/class/ata_port/ata%u/port_no", fixture, ata);
    snprintf(value, sizeof(value), "%u\n", port);
    write_file(path, value);
}

static void add_sata_controller(const char *pci, unsigned int vendor, unsigned int device,
                                 unsigned int count, const unsigned int *global_ata)
{
    char controller[4096], path[4096], link[4096], driver[4096];
    char id[32];
    unsigned int i;
    snprintf(controller, sizeof(controller), "%s/devices/pci0000:00/%s", fixture, pci);
    mkdirs(controller);
    snprintf(path, sizeof(path), "%s/vendor", controller);
    snprintf(id, sizeof(id), "0x%04x\n", vendor);
    write_file(path, id);
    snprintf(path, sizeof(path), "%s/device", controller);
    snprintf(id, sizeof(id), "0x%04x\n", device);
    write_file(path, id);
    snprintf(path, sizeof(path), "%s/class", controller);
    write_file(path, "0x010601\n");
    snprintf(driver, sizeof(driver), "%s/bus/pci/drivers/ahci", fixture);
    mkdirs(driver);
    snprintf(link, sizeof(link), "%s/driver", controller);
    CHECK(symlink(driver, link) == 0);
    snprintf(path, sizeof(path), "%s/bus/pci/devices", fixture);
    mkdirs(path);
    snprintf(link, sizeof(link), "%s/bus/pci/devices/%s", fixture, pci);
    CHECK(symlink(controller, link) == 0);
    snprintf(path, sizeof(path), "%s/class/ata_port", fixture);
    mkdirs(path);
    for (i = 0; i < count; ++i) {
        char port[32];
        snprintf(path, sizeof(path), "%s/ata%u/ata_port/ata%u", controller, global_ata[i], global_ata[i]);
        mkdirs(path);
        snprintf(link, sizeof(link), "%s/class/ata_port/ata%u", fixture, global_ata[i]);
        CHECK(symlink(path, link) == 0);
        snprintf(path, sizeof(path), "%s/ata%u/ata_port/ata%u/port_no", controller, global_ata[i], global_ata[i]);
        snprintf(port, sizeof(port), "%u\n", i + 1);
        write_file(path, port);
    }
}

static void add_asm1164(const char *pci, const unsigned int global_ata[4])
{
    add_sata_controller(pci, 0x1b21, 0x1164, 4, global_ata);
}

static void attach_sata_bridge(const char *pci, const char *bridge, unsigned int bridge_id,
                                unsigned int count, const unsigned int *global_ata)
{
    char original[4096], relocated[4096], parent[4096], path[4096], link[4096], value[32];
    unsigned int i;
    snprintf(parent, sizeof(parent), "%s/devices/pci0000:00/%s", fixture, bridge);
    mkdirs(parent);
    snprintf(path, sizeof(path), "%s/vendor", parent); write_file(path, "0x8086\n");
    snprintf(path, sizeof(path), "%s/device", parent);
    snprintf(value, sizeof(value), "0x%04x\n", bridge_id); write_file(path, value);
    snprintf(path, sizeof(path), "%s/class", parent); write_file(path, "0x060400\n");
    snprintf(original, sizeof(original), "%s/devices/pci0000:00/%s", fixture, pci);
    snprintf(relocated, sizeof(relocated), "%s/%s", parent, pci);
    CHECK(rename(original, relocated) == 0);
    snprintf(link, sizeof(link), "%s/bus/pci/devices/%s", fixture, pci);
    CHECK(unlink(link) == 0 && symlink(relocated, link) == 0);
    for (i = 0; i < count; ++i) {
        snprintf(link, sizeof(link), "%s/class/ata_port/ata%u", fixture, global_ata[i]);
        snprintf(path, sizeof(path), "%s/ata%u/ata_port/ata%u", relocated, global_ata[i], global_ata[i]);
        CHECK(unlink(link) == 0 && symlink(path, link) == 0);
    }
}

static void add_nvme(const char *name, const char *bridge, unsigned int bridge_id,
                      const char *endpoint, const char *serial)
{
    char parent[4096], base[4096], path[4096], link[4096], value[32];
    unsigned int controller, ns;
    CHECK(sscanf(name, "nvme%un%u", &controller, &ns) == 2);
    snprintf(parent, sizeof(parent), "%s/devices/pci0000:00/%s", fixture, bridge);
    snprintf(path, sizeof(path), "%s/vendor", parent);
    write_file(path, "0x8086\n");
    snprintf(path, sizeof(path), "%s/device", parent);
    snprintf(value, sizeof(value), "0x%04x\n", bridge_id);
    write_file(path, value);
    snprintf(path, sizeof(path), "%s/class", parent);
    write_file(path, "0x060400\n");
    snprintf(path, sizeof(path), "%s/bus/pci/devices", fixture); mkdirs(path);
    snprintf(link, sizeof(link), "%s/bus/pci/devices/%s", fixture, bridge);
    if (access(link, F_OK) != 0) CHECK(symlink(parent, link) == 0);
    snprintf(base, sizeof(base), "%s/%s/nvme/nvme%u/%s", parent, endpoint, controller, name);
    mkdirs(base);
    snprintf(path, sizeof(path), "%s/device/transport", base);
    write_file(path, "pcie\n");
    snprintf(path, sizeof(path), "%s/device/serial", base);
    write_file(path, serial);
    snprintf(path, sizeof(path), "%s/wwid", base);
    write_file(path, "eui.0011223344556677\n");
    snprintf(link, sizeof(link), "%s/class/block/%s", fixture, name);
    CHECK(symlink(base, link) == 0);
}

static void add_platform_disk(const char *name, const char *controller, unsigned int ata,
                              unsigned int host, const char *driver_name)
{
    char parent[4096], base[4096], path[4096], link[4096], driver[4096];
    snprintf(parent, sizeof(parent), "%s/devices/platform/%s", fixture, controller);
    mkdirs(parent);
    snprintf(driver, sizeof(driver), "%s/bus/platform/drivers/%s", fixture, driver_name);
    mkdirs(driver);
    snprintf(link, sizeof(link), "%s/driver", parent);
    CHECK(symlink(driver, link) == 0);
    snprintf(base, sizeof(base), "%s/ata%u/host%u/target%u:0:0/%u:0:0:0/block/%s",
             parent, ata, host, host, host, name);
    mkdirs(base);
    snprintf(path, sizeof(path), "%s/device/type", base); write_file(path, "0\n");
    snprintf(path, sizeof(path), "%s/device/vendor", base); write_file(path, "ATA     \n");
    snprintf(path, sizeof(path), "%s/device/serial", base); write_file(path, "PLATFORM-ATA\n");
    snprintf(link, sizeof(link), "%s/class/block/%s", fixture, name);
    CHECK(symlink(base, link) == 0);
    snprintf(path, sizeof(path), "%s/ata%u/ata_port/ata%u", parent, ata, ata); mkdirs(path);
    snprintf(link, sizeof(link), "%s/class/ata_port", fixture); mkdirs(link);
    snprintf(link, sizeof(link), "%s/class/ata_port/ata%u", fixture, ata);
    CHECK(symlink(path, link) == 0);
    snprintf(path, sizeof(path), "%s/ata%u/ata_port/ata%u/port_no", parent, ata, ata);
    write_file(path, "1\n");
}

static void test_arm_platform_inventory(void)
{
    struct ugreenctl_disk_scan scan;
    const struct ugreenctl_disk *disk;
    char error[256];
    setup();
    add_platform_disk("sdz", "2a240000.sata", 92, 177, "dwc-ahci");
    add_platform_disk("sda", "2a250000.sata", 13, 91, "ahci");
    add_platform_disk("sdb", "wrong-controller", 65, 76, "other-storage");
    CHECK(ugreenctl_disk_scan_at(fixture, "DH2300", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 2 && scan.profile_known);
    disk = find_disk(&scan, "sdz");
    CHECK(!disk->mapped && !disk->led_supported && !disk->pci[0]);
    CHECK(!strcmp(disk->topology, "platform-2a240000.sata-ata-1"));
    CHECK(!strcmp(disk->serial, "PLATFORM-ATA"));
    CHECK(ugreenctl_disk_resolve(&scan, "sdz", &disk, error, sizeof(error)) != 0 && disk == NULL);
    CHECK(ugreenctl_disk_scan_at(fixture, "DH4300 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 2 && scan.profile_known && !find_disk(&scan, "sda")->mapped);
    teardown();
}

static void add_native_of_node(const char *controller, const char *node, unsigned int address)
{
    static const char compatible[] = "rockchip,rk-ahci\0snps,dwc-ahci";
    static const unsigned char ports[] = {0,0,0,1};
    unsigned char reg[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10,0};
    char base[4096], path[4096], link[4096];
    reg[4] = (unsigned char)(address >> 24); reg[5] = (unsigned char)(address >> 16);
    reg[6] = (unsigned char)(address >> 8); reg[7] = (unsigned char)address;
    snprintf(base, sizeof(base), "%s/firmware/devicetree/base/%s", fixture, node);
    snprintf(path, sizeof(path), "%s/compatible", base); write_binary(path, compatible, sizeof(compatible));
    snprintf(path, sizeof(path), "%s/ports-implemented", base); write_binary(path, ports, sizeof(ports));
    snprintf(path, sizeof(path), "%s/reg", base); write_binary(path, reg, sizeof(reg));
    snprintf(path, sizeof(path), "%s/status", base); write_binary(path, "okay", sizeof("okay"));
    snprintf(link, sizeof(link), "%s/devices/platform/%s/of_node", fixture, controller);
    CHECK(symlink(base, link) == 0);
}

static void attach_sata_of_platform(const char *pci, unsigned int count, const unsigned int *global_ata)
{
    static const char compatible[] = "rockchip,rk3588-pcie\0snps,dw-pcie";
    static const unsigned char reg[] = {0,0,0,0,0xfe,0x17,0,0,0,0,0,0,0,1,0,0};
    char parent[4096], original[4096], relocated[4096], path[4096], link[4096], node[4096];
    unsigned int i;
    snprintf(parent, sizeof(parent), "%s/devices/platform/fe170000.pcie/pci0007:70", fixture); mkdirs(parent);
    snprintf(original, sizeof(original), "%s/devices/pci0000:00/%s", fixture, pci);
    snprintf(relocated, sizeof(relocated), "%s/%s", parent, pci);
    CHECK(rename(original, relocated) == 0);
    snprintf(link, sizeof(link), "%s/bus/pci/devices/%s", fixture, pci);
    CHECK(unlink(link) == 0 && symlink(relocated, link) == 0);
    for (i = 0; i < count; ++i) {
        snprintf(link, sizeof(link), "%s/class/ata_port/ata%u", fixture, global_ata[i]);
        snprintf(path, sizeof(path), "%s/ata%u/ata_port/ata%u", relocated, global_ata[i], global_ata[i]);
        CHECK(unlink(link) == 0 && symlink(path, link) == 0);
    }
    snprintf(node, sizeof(node), "%s/firmware/devicetree/base/pcie@fe170000", fixture); mkdirs(node);
    snprintf(path, sizeof(path), "%s/compatible", node); write_binary(path, compatible, sizeof(compatible));
    snprintf(path, sizeof(path), "%s/reg", node); write_binary(path, reg, sizeof(reg));
    snprintf(path, sizeof(path), "%s/status", node); write_binary(path, "okay", sizeof("okay"));
    snprintf(link, sizeof(link), "%s/devices/platform/fe170000.pcie/of_node", fixture);
    CHECK(symlink(node, link) == 0);
}

static void test_arm_conditional_of_mapping(void)
{
    static const unsigned int asm_ports[] = {103,32};
    static const char model2[] = "UGREEN RK3576 DH2300 Board", model4[] = "UGREEN RK3588C DH4300Plus Board";
    struct ugreenctl_disk_scan scan;
    const struct ugreenctl_disk *disk;
    char error[256], path[4096], link[4096];
    setup();
    add_platform_disk("sdz", "2a240000.sata", 92, 177, "dwc-ahci");
    add_platform_disk("sda", "2a250000.sata", 13, 91, "ahci");
    add_native_of_node("2a240000.sata", "sata@2a240000", 0x2a240000);
    add_native_of_node("2a250000.sata", "sata@2a250000", 0x2a250000);
    snprintf(path, sizeof(path), "%s/firmware/devicetree/base/model", fixture); write_binary(path, model2, sizeof(model2));
    CHECK(ugreenctl_disk_scan_at(fixture, "DH2300", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "sdz")->mapped && find_disk(&scan, "sdz")->slot == 2);
    CHECK(find_disk(&scan, "sda")->slot == 1 && find_disk(&scan, "sda")->led_supported);
    CHECK(!strcmp(find_disk(&scan, "sdz")->mapping_source, "firmware-of-topology"));
    CHECK(ugreenctl_disk_resolve(&scan, "slot1", &disk, error, sizeof(error)) == 0 && !strcmp(disk->name, "sda"));
    snprintf(path, sizeof(path), "%s/class/block/sda", fixture); CHECK(unlink(path) == 0);
    CHECK(ugreenctl_disk_scan_at(fixture, "DH2300", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 1 && find_disk(&scan, "sdz")->slot == 2); /* Empty bay is still a controller port. */
    snprintf(path, sizeof(path), "%s/firmware/devicetree/base/model", fixture); write_binary(path, model4, sizeof(model4));
    CHECK(ugreenctl_disk_scan_at(fixture, "DH2300", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "sdz")->mapped);
    write_binary(path, model2, sizeof(model2));
    snprintf(path, sizeof(path), "%s/firmware/devicetree/base/sata@2a250000/reg", fixture); write_file(path, "wrong-reg");
    CHECK(ugreenctl_disk_scan_at(fixture, "DH2300", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "sdz")->mapped);
    teardown();

    setup();
    add_platform_disk("sdz", "fe230000.sata", 92, 177, "dwc-ahci");
    add_platform_disk("sdy", "fe220000.sata", 13, 91, "ahci");
    add_native_of_node("fe230000.sata", "sata@fe230000", 0xfe230000);
    add_native_of_node("fe220000.sata", "sata@fe220000", 0xfe220000);
    snprintf(path, sizeof(path), "%s/firmware/devicetree/base/model", fixture); write_binary(path, model4, sizeof(model4));
    add_sata_controller("0007:99:00.0", 0x1b21, 0x0612, 2, asm_ports);
    attach_sata_of_platform("0007:99:00.0", 2, asm_ports);
    add_disk("sda", "0007:99:00.0", 103, 1, 81, "BRIDGE-FIRST");
    add_disk("sdb", "0007:99:00.0", 32, 2, 82, "BRIDGE-SECOND");
    CHECK(ugreenctl_disk_scan_at(fixture, "DH4300 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "sdz")->slot == 2 && find_disk(&scan, "sdy")->slot == 1);
    CHECK(find_disk(&scan, "sda")->slot == 4 && find_disk(&scan, "sdb")->slot == 3);
    CHECK(ugreenctl_disk_resolve(&scan, "slot4", &disk, error, sizeof(error)) == 0 && !strcmp(disk->led_id, "disk4"));
    snprintf(path, sizeof(path), "%s/class/ata_port/ata13", fixture); CHECK(unlink(path) == 0);
    CHECK(ugreenctl_disk_scan_at(fixture, "DH4300 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "sda")->mapped && !find_disk(&scan, "sdz")->mapped);
    snprintf(link, sizeof(link), "%s/devices/platform/fe220000.sata/ata13/ata_port/ata13", fixture);
    CHECK(symlink(link, path) == 0);
    snprintf(path, sizeof(path), "%s/bus/pci/devices/0007:99:00.0/device", fixture); write_file(path, "0x1164\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DH4300 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "sda")->mapped);
    write_file(path, "0x0612\n");
    snprintf(path, sizeof(path), "%s/devices/platform/fe170000.pcie/of_node", fixture); CHECK(unlink(path) == 0);
    snprintf(link, sizeof(link), "%s/firmware/devicetree/base/pcie@fe150000", fixture); mkdirs(link);
    CHECK(symlink(link, path) == 0);
    CHECK(ugreenctl_disk_scan_at(fixture, "DH4300 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "sda")->mapped && !find_disk(&scan, "sdb")->mapped);
    teardown();
}

static const struct ugreenctl_disk *find_disk(const struct ugreenctl_disk_scan *scan, const char *name)
{
    size_t i;
    for (i = 0; i < scan->count; ++i)
        if (!strcmp(name, scan->disks[i].name))
            return &scan->disks[i];
    CHECK(0);
    return NULL;
}

static void test_sparse_ata_and_hosts(void)
{
    static const unsigned int intel_ports[] = {9,22}, asm_ports[] = {100,31};
    struct ugreenctl_disk_scan scan;
    const struct ugreenctl_disk *disk;
    char error[256], path[4096];
    setup();
    add_sata_controller("0000:00:17.0", 0x8086, 0x54d3, 2, intel_ports);
    add_sata_controller("0000:01:00.0", 0x1b21, 0x0612, 2, asm_ports);
    add_disk("sdz", "0000:01:00.0", 31, 2, 77, " THIRD ");
    add_disk("sda", "0000:00:17.0", 9, 1, 41, " FIRST\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 2 && scan.profile_known);
    disk = find_disk(&scan, "sdz");
    CHECK(disk->mapped && disk->slot == 4 && !strcmp(disk->led_id, "disk4"));
    CHECK(disk->ata == 31 && disk->ata_port == 2);
    CHECK(!strcmp(disk->topology, "pci-0000:01:00.0-ata-2"));
    CHECK(!strcmp(disk->serial, "THIRD"));
    CHECK(find_disk(&scan, "sda")->slot == 1);
    CHECK(ugreenctl_disk_resolve(&scan, "slot2", &disk, error, sizeof(error)) != 0);
    /* Rescan after removing and adding drives: empty slot does not compact. */
    snprintf(path, sizeof(path), "%s/class/block/sda", fixture);
    CHECK(unlink(path) == 0);
    add_disk("sdc", "0000:00:17.0", 22, 2, 99, "REPLACEMENT");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 2);
    CHECK(find_disk(&scan, "sdc")->slot == 2);
    CHECK(find_disk(&scan, "sdz")->slot == 4);
    teardown();
}

static void test_6800pro_layout(void)
{
    static const unsigned int intel_ports[] = {51,52}, asm_ports[] = {92,93,94,99};
    struct ugreenctl_disk_scan scan;
    char error[256];
    setup();
    add_sata_controller("0000:00:17.0", 0x8086, 0x51d3, 2, intel_ports);
    add_sata_controller("0000:59:00.0", 0x1b21, 0x1164, 4, asm_ports);
    add_disk("sda", "0000:00:17.0", 51, 1, 70, "FIFTH");
    add_disk("sdb", "0000:59:00.0", 92, 1, 120, "FIRST");
    add_disk("sdc", "0000:59:00.0", 99, 4, 125, "FOURTH");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP6800 Pro", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "sda")->slot == 5);
    CHECK(find_disk(&scan, "sdb")->slot == 1);
    CHECK(find_disk(&scan, "sdc")->slot == 4);
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP6800 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.profile_known);
    CHECK(find_disk(&scan, "sda")->mapped && find_disk(&scan, "sda")->slot == 5);
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP6800 Pro extra", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!scan.profile_known && !find_disk(&scan, "sdb")->mapped);
    CHECK(ugreenctl_disk_scan_at(fixture, "", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 3 && !scan.profile_known && !find_disk(&scan, "sdb")->mapped);
    CHECK(ugreenctl_disk_scan_at(fixture, NULL, NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 3 && !scan.profile_known);
    CHECK(ugreenctl_disk_scan_at(fixture, "", "/not-used", &scan, error, sizeof(error)) != 0);
    teardown();
}

static void test_firmware_controller_role_layouts(void)
{
    static const char *const four_models[] = {"DX4600 Pro", "DX4600+", "DX4600", "DX4700", "DX4700+", "DXP4800", "DXP4800S"};
    static const char *const six_models[] = {"DXP6800", "DXP6800 Plus", "DXP6800 Pro", "DXP6800 Ultra"};
    static const char *const eight_models[] = {"DXP8800", "DXP8800 Plus", "DXP8800 Pro", "DXP8800 Ultra"};
    static const unsigned int intel_ports[] = {12,67}, asm_a[] = {103,32,91,5}, asm_b[] = {45,28,76,114};
    struct ugreenctl_disk_scan scan;
    char error[256], path[4096];
    size_t i;
    setup();
    add_sata_controller("0000:00:17.0", 0x8086, 0x54d3, 2, intel_ports);
    add_sata_controller("0000:98:00.0", 0x1b21, 0x0612, 2, asm_a);
    add_disk("sdz", "0000:00:17.0", 67, 2, 135, "INTEL-SECOND");
    add_disk("sda", "0000:98:00.0", 32, 2, 90, "ASM-SECOND");
    for (i = 0; i < sizeof(four_models)/sizeof(four_models[0]); ++i) {
        CHECK(ugreenctl_disk_scan_at(fixture, four_models[i], NULL, &scan, error, sizeof(error)) == 0);
        CHECK(find_disk(&scan, "sdz")->mapped && find_disk(&scan, "sdz")->slot == 2);
        CHECK(find_disk(&scan, "sda")->mapped && find_disk(&scan, "sda")->slot == 4);
    }
    /* A third controller cannot be ignored even when all bays already have a match. */
    add_sata_controller("0000:99:00.0", 0x1b21, 0x0612, 2, asm_b);
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "sda")->mapped && !find_disk(&scan, "sdz")->mapped);
    teardown();

    setup();
    add_sata_controller("0000:00:17.0", 0x8086, 0x51d3, 2, intel_ports);
    add_asm1164("0000:98:00.0", asm_a);
    add_disk("sdz", "0000:00:17.0", 67, 2, 135, "INTEL-SECOND");
    add_disk("sda", "0000:98:00.0", 103, 1, 90, "ASM-FIRST");
    add_disk("sdy", "0000:98:00.0", 5, 4, 91, "ASM-FOURTH");
    for (i = 0; i < sizeof(six_models)/sizeof(six_models[0]); ++i) {
        CHECK(ugreenctl_disk_scan_at(fixture, six_models[i], NULL, &scan, error, sizeof(error)) == 0);
        CHECK(find_disk(&scan, "sdz")->slot == 6 && find_disk(&scan, "sda")->slot == 1);
        CHECK(find_disk(&scan, "sdy")->slot == 4);
    }
    CHECK(ugreenctl_disk_scan_at(fixture, "iDX6011 Pro", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "sdz")->slot == 4 && find_disk(&scan, "sda")->slot == 1);
    CHECK(find_disk(&scan, "sdy")->slot == 6);
    snprintf(path, sizeof(path), "%s/class/ata_port/ata91", fixture);
    CHECK(unlink(path) == 0); /* Unoccupied port missing: no position guessing. */
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP6800 Pro", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "sdz")->mapped && !find_disk(&scan, "sda")->mapped);
    teardown();

    setup();
    add_asm1164("0000:98:00.0", asm_a);
    attach_sata_bridge("0000:98:00.0", "0000:00:1c.4", 0x51bc, 4, asm_a);
    add_asm1164("0000:18:00.0", asm_b);
    attach_sata_bridge("0000:18:00.0", "0000:00:1c.6", 0x51be, 4, asm_b);
    add_disk("sdz", "0000:98:00.0", 32, 2, 135, "FIRST-CONTROLLER-SECOND");
    add_disk("sda", "0000:18:00.0", 76, 3, 90, "SECOND-CONTROLLER-THIRD");
    for (i = 0; i < sizeof(eight_models)/sizeof(eight_models[0]); ++i) {
        CHECK(ugreenctl_disk_scan_at(fixture, eight_models[i], NULL, &scan, error, sizeof(error)) == 0);
        CHECK(find_disk(&scan, "sdz")->mapped && find_disk(&scan, "sdz")->slot == 2);
        CHECK(find_disk(&scan, "sda")->mapped && find_disk(&scan, "sda")->slot == 7);
    }
    snprintf(path, sizeof(path), "%s/devices/pci0000:00/0000:00:1c.6/device", fixture);
    write_file(path, "0x51bc\n"); /* Two identical bridge roles are ambiguous. */
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP8800 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "sdz")->mapped && !find_disk(&scan, "sda")->mapped);
    teardown();

    setup();
    add_sata_controller("0000:00:17.0", 0x8086, 0x4dd3, 2, intel_ports);
    add_disk("sda", "0000:00:17.0", 12, 1, 90, "DH2600-SECOND");
    CHECK(ugreenctl_disk_scan_at(fixture, "DH2600", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "sda")->mapped && find_disk(&scan, "sda")->slot == 2);
    CHECK(find_disk(&scan, "sda")->led_supported && !strcmp(find_disk(&scan, "sda")->led_id, "disk2"));
    teardown();
}

static void test_filters_and_unknown(void)
{
    struct ugreenctl_disk_scan scan;
    char error[256], path[4096], base[4096], link[4096];
    setup();
    add_disk("sda", "0000:01:00.0", 2, 1, 1, "GOOD");
    add_disk("sdb", "0000:08:00.0", 5, 1, 5, "EXTERNAL-PCI");
    add_disk("sdc", "0000:01:00.0", 6, 2, 6, "PARTITION");
    snprintf(path, sizeof(path), "%s/class/block/sdc/partition", fixture);
    write_file(path, "1\n");
    add_disk("sdd", "0000:01:00.0", 8, 2, 8, "RAID");
    snprintf(path, sizeof(path), "%s/class/block/sdd/device/vendor", fixture);
    write_file(path, "MegaRAID\n");
    snprintf(base, sizeof(base), "%s/devices/pci0000:00/0000:00:14.0/usb1/1-2/ata10/host10/target10:0:0/10:0:0:0/block/sde", fixture);
    mkdirs(base);
    snprintf(path, sizeof(path), "%s/device/type", base);
    write_file(path, "0\n");
    snprintf(path, sizeof(path), "%s/device/vendor", base);
    write_file(path, "ATA\n");
    snprintf(link, sizeof(link), "%s/class/block/sde", fixture);
    CHECK(symlink(base, link) == 0);
    snprintf(base, sizeof(base), "%s/devices/virtual/block/loop0", fixture);
    mkdirs(base);
    snprintf(link, sizeof(link), "%s/class/block/loop0", fixture);
    CHECK(symlink(base, link) == 0);
    snprintf(link, sizeof(link), "%s/class/block/sda1", fixture);
    CHECK(symlink(base, link) == 0);
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 2);
    CHECK(!find_disk(&scan, "sda")->mapped);
    CHECK(!find_disk(&scan, "sdb")->mapped);
    /* Missing local port must not silently use global ata2. */
    snprintf(path, sizeof(path), "%s/class/ata_port/ata2/port_no", fixture);
    CHECK(unlink(path) == 0);
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "sda")->mapped);
    CHECK(find_disk(&scan, "sda")->topology[0] == '\0');
    teardown();
}

static void test_conflicts(void)
{
    struct ugreenctl_disk_scan scan;
    char error[256];
    setup();
    add_disk("sda", "0000:01:00.0", 1, 1, 10, "ONE");
    add_disk("sdb", "0000:01:00.0", 9, 1, 19, "TWO");
    add_disk("sdc", "0000:01:00.0", 22, 1, 22, "THREE");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 3);
    CHECK(!scan.disks[0].mapped && !scan.disks[1].mapped && !scan.disks[2].mapped);
    CHECK(scan.disks[0].slot == 0 && scan.disks[2].led_id[0] == '\0');
    teardown();
}

static void test_cached_vpd_serial(void)
{
    static const unsigned char valid[] = {0,0x80,0,13,' ','C','A','C','H','E','D','-','S','N',' ',' ',' '};
    static const unsigned char other[] = {0,0x80,0,5,'O','T','H','E','R'};
    static const unsigned char truncated[] = {0,0x80,0,9,'S','N'};
    static const unsigned char wrong_page[] = {0,0x83,0,2,'S','N'};
    static const unsigned char embedded_nul[] = {0,0x80,0,3,'S',0,'N'};
    static const unsigned char control[] = {0,0x80,0,3,'S','\n','N'};
    static const unsigned char trailing[] = {0,0x80,0,2,'S','N','X'};
    static const unsigned char empty[] = {0,0x80,0,3,' ',' ',' '};
    struct ugreenctl_disk_scan scan;
    const struct ugreenctl_disk *disk;
    char error[256];
    setup();
    add_disk("sda", "0000:59:00.0", 40, 1, 90, "PLACEHOLDER");
    remove_serial_attribute("sda");
    write_vpd("sda", valid, sizeof(valid));
    add_disk("sdb", "0000:59:00.0", 41, 2, 91, "ATTRIBUTE-SN\n");
    write_vpd("sdb", other, sizeof(other));
    add_disk("sdc", "0000:08:00.0", 42, 1, 92, "PLACEHOLDER");
    remove_serial_attribute("sdc");
    write_vpd("sdc", truncated, sizeof(truncated));
    add_disk("sdd", "0000:08:00.0", 43, 2, 93, "PLACEHOLDER");
    remove_serial_attribute("sdd");
    write_vpd("sdd", wrong_page, sizeof(wrong_page));
    add_disk("sde", "0000:08:00.0", 44, 3, 94, "PLACEHOLDER");
    remove_serial_attribute("sde");
    write_vpd("sde", embedded_nul, sizeof(embedded_nul));
    add_disk("sdf", "0000:08:00.0", 45, 4, 95, "PLACEHOLDER");
    remove_serial_attribute("sdf");
    write_vpd("sdf", control, sizeof(control));
    add_disk("sdg", "0000:08:00.0", 46, 5, 96, "PLACEHOLDER");
    remove_serial_attribute("sdg");
    write_vpd("sdg", trailing, sizeof(trailing));
    add_disk("sdh", "0000:08:00.0", 47, 6, 97, "PLACEHOLDER");
    remove_serial_attribute("sdh");
    add_disk("sdi", "0000:08:00.0", 48, 7, 98, "PLACEHOLDER");
    remove_serial_attribute("sdi");
    write_vpd("sdi", empty, sizeof(empty));
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP8800 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 9);
    CHECK(!strcmp(find_disk(&scan, "sda")->serial, "CACHED-SN"));
    CHECK(!strcmp(find_disk(&scan, "sdb")->serial, "ATTRIBUTE-SN"));
    CHECK(ugreenctl_disk_resolve(&scan, "serial:CACHED-SN", &disk, error, sizeof(error)) != 0);
    for (size_t i = 2; i < scan.count; ++i)
        CHECK(scan.disks[i].serial[0] == '\0');
    teardown();
}

static void check_singleton_unknown(const char *model)
{
    struct ugreenctl_disk_scan scan;
    char error[256];
    size_t i;
    CHECK(ugreenctl_disk_scan_at(fixture, model, NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count >= 1);
    for (i = 0; i < scan.count; ++i)
        CHECK(!scan.disks[i].mapped && scan.disks[i].led_id[0] == '\0');
}

static void test_plus_pro_single_controller(void)
{
    static const unsigned int ports[] = {12,90,37,104};
    static const unsigned int other_ports[] = {201,202,203,204};
    struct ugreenctl_disk_scan scan;
    char error[256], path[4096], target[4096];
    setup();
    add_asm1164("0000:07:00.0", ports);
    add_disk("sdz", "0000:07:00.0", 37, 3, 91, "THIRD");
    add_disk("sda", "0000:07:00.0", 12, 1, 72, "FIRST");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 2 && find_disk(&scan, "sdz")->slot == 3 && find_disk(&scan, "sda")->slot == 1);
    CHECK(!strcmp(find_disk(&scan, "sda")->mapping_source, "firmware-asm1164-topology"));
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Pro", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "sdz")->slot == 3);
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800S", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "sda")->mapped && find_disk(&scan, "sda")->slot == 1);
    check_singleton_unknown("DXP4800 Plus extra");
    /* An absent empty port must invalidate the whole structural conversion. */
    snprintf(path, sizeof(path), "%s/class/ata_port/ata90", fixture);
    CHECK(unlink(path) == 0);
    check_singleton_unknown("DXP4800 Plus");
    snprintf(target, sizeof(target), "%s/devices/pci0000:00/0000:07:00.0/ata90/ata_port/ata90", fixture);
    CHECK(symlink(target, path) == 0);
    /* Duplicate and out-of-range local ports cannot be normalized or sorted. */
    snprintf(path, sizeof(path), "%s/class/ata_port/ata90/port_no", fixture);
    write_file(path, "1\n");
    check_singleton_unknown("DXP4800 Plus");
    write_file(path, "5\n");
    check_singleton_unknown("DXP4800 Plus");
    write_file(path, "2\n");
    snprintf(path, sizeof(path), "%s/bus/pci/devices/0000:07:00.0/vendor", fixture);
    write_file(path, "0x8086\n");
    check_singleton_unknown("DXP4800 Pro");
    write_file(path, "0x1b21\n");
    snprintf(path, sizeof(path), "%s/bus/pci/devices/0000:07:00.0/device", fixture);
    write_file(path, "0x1166\n");
    check_singleton_unknown("DXP4800 Pro");
    write_file(path, "0x1164\n");
    snprintf(path, sizeof(path), "%s/bus/pci/devices/0000:07:00.0/class", fixture);
    write_file(path, "0x010600\n");
    check_singleton_unknown("DXP4800 Pro");
    write_file(path, "0x010601\n");
    snprintf(path, sizeof(path), "%s/bus/pci/devices/0000:07:00.0/driver", fixture);
    CHECK(unlink(path) == 0);
    snprintf(target, sizeof(target), "%s/bus/pci/drivers/unknown", fixture);
    mkdirs(target);
    CHECK(symlink(target, path) == 0);
    check_singleton_unknown("DXP4800 Pro");
    CHECK(unlink(path) == 0);
    snprintf(target, sizeof(target), "%s/bus/pci/drivers/ahci", fixture);
    CHECK(symlink(target, path) == 0);
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Pro", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "sda")->mapped);
    /* A second controller is refused even if none of its bays has a disk. */
    add_asm1164("0000:08:00.0", other_ports);
    check_singleton_unknown("DXP4800 Plus");
    snprintf(path, sizeof(path), "%s/bus/pci/devices/0000:08:00.0/device", fixture);
    write_file(path, "0x0612\n");
    check_singleton_unknown("DXP4800 Plus");
    teardown();
    /* A foreign ATA port cannot be treated as one of the controller's ports. */
    setup();
    add_asm1164("0000:05:00.0", ports);
    add_disk("sda", "0000:05:00.0", 12, 1, 72, "FIRST");
    add_disk("sdb", "0000:09:00.0", 110, 2, 91, "FOREIGN");
    check_singleton_unknown("DXP4800 Plus");
    teardown();
}

static void test_dxp2800_em_single_controller(void)
{
    static const unsigned int ports[] = {11,61};
    static const unsigned int extra_ports[] = {101,102};
    struct ugreenctl_disk_scan scan;
    char error[256], path[4096], target[4096];
    setup();
    add_sata_controller("0000:00:17.0", 0x8086, 0x54d3, 2, ports);
    add_disk("sdz", "0000:00:17.0", 61, 2, 90, "SECOND");
    check_singleton_unknown("DXP2800");
    snprintf(path, sizeof(path), "%s/class/dmi/id/product_version", fixture);
    write_file(path, "DXP2800 V1.2 251215\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP2800", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "sdz")->mapped && find_disk(&scan, "sdz")->slot == 2);
    CHECK(!strcmp(scan.profile, "DXP2800")); /* Exact single-controller profile label, not a guessed board revision. */
    write_file(path, "DXP2800 V1.2 251215 extra\n");
    check_singleton_unknown("DXP2800");
    write_file(path, "EM_DXP2800_V1.0.25\n");
    check_singleton_unknown("DXP2800");
    write_file(path, "EM_DXP2800_V1.0.24\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP2800", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.profile_known && scan.count == 1 && find_disk(&scan, "sdz")->slot == 2);
    CHECK(!strcmp(find_disk(&scan, "sdz")->mapping_source, "firmware-intel-ahci-topology"));
    check_singleton_unknown("DXP2800 GT");
    check_singleton_unknown("DXP2800 extra");
    /* Port 1 is empty but remains part of the identity/port-count check. */
    snprintf(path, sizeof(path), "%s/class/ata_port/ata11", fixture);
    CHECK(unlink(path) == 0);
    check_singleton_unknown("DXP2800");
    snprintf(target, sizeof(target), "%s/devices/pci0000:00/0000:00:17.0/ata11/ata_port/ata11", fixture);
    CHECK(symlink(target, path) == 0);
    snprintf(path, sizeof(path), "%s/class/ata_port/ata11/port_no", fixture);
    write_file(path, "2\n");
    check_singleton_unknown("DXP2800");
    write_file(path, "3\n");
    check_singleton_unknown("DXP2800");
    write_file(path, "1\n");
    snprintf(path, sizeof(path), "%s/bus/pci/devices/0000:00:17.0/device", fixture);
    write_file(path, "0x1164\n");
    check_singleton_unknown("DXP2800");
    write_file(path, "0x54d3\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP2800", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "sdz")->slot == 2);
    add_sata_controller("0000:09:00.0", 0x8086, 0x54d3, 2, extra_ports);
    check_singleton_unknown("DXP2800");
    teardown();
}

static void test_nvme_bridge_mapping(void)
{
    static const unsigned int ports[] = {12,90,37,104};
    struct ugreenctl_disk_scan scan;
    const struct ugreenctl_disk *disk;
    char error[256], path[4096];
    setup();
    add_asm1164("0000:07:00.0", ports);
    add_disk("sda", "0000:07:00.0", 12, 1, 72, "SATA-FIRST");
    add_nvme("nvme17n1", "0000:00:08.0", 0x463d, "0000:71:00.0", "NVME-FIRST");
    add_nvme("nvme3n1", "0000:00:09.0", 0x464d, "0000:82:00.0", "NVME-SECOND");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 3);
    disk = find_disk(&scan, "nvme17n1");
    CHECK(disk->mapped && disk->slot == 1 && !strcmp(disk->slot_name, "nvme1"));
    CHECK(!disk->led_supported && !disk->led_id[0]);
    CHECK(find_disk(&scan, "nvme3n1")->slot == 2);
    CHECK(find_disk(&scan, "sda")->mapped && find_disk(&scan, "sda")->slot == 1);
    CHECK(ugreenctl_disk_resolve(&scan, "serial:NVME-FIRST", &disk, error, sizeof(error)) == -ENOTSUP);
    CHECK(strstr(error, "Unsupported") != NULL);
    CHECK(ugreenctl_disk_resolve(&scan, "slot1", &disk, error, sizeof(error)) == 0);
    CHECK(!strcmp(disk->name, "sda"));
    /* A partition and an NVMe-over-fabrics transport are not physical PCIe disks. */
    snprintf(path, sizeof(path), "%s/class/block/nvme3n1/device/transport", fixture);
    write_file(path, "tcp\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 2);
    write_file(path, "pcie\n");
    add_nvme("nvme19n1", "0000:00:08.0", 0x463d, "0000:72:00.0", "DUPLICATE-CONNECTOR");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "nvme17n1")->mapped && !find_disk(&scan, "nvme19n1")->mapped);
    CHECK(find_disk(&scan, "nvme3n1")->mapped);
    teardown();
    /* DXP480T first M.2 root port remains stable if endpoint bus numbers change. */
    setup();
    add_nvme("nvme42n1", "0000:00:06.0", 0x0001, "0000:99:00.0", "480T-FIRST");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP480T Plus", NULL, &scan, error, sizeof(error)) == 0);
    disk = find_disk(&scan, "nvme42n1");
    CHECK(disk->mapped && disk->slot == 1 && !disk->led_supported);
    CHECK(ugreenctl_disk_resolve(&scan, "/dev/nvme42n1", &disk, error, sizeof(error)) == -ENOTSUP);
    CHECK(strstr(error, "Unsupported") != NULL);
    teardown();
}

static void test_official_nvme_connector_matrix(void)
{
    static const struct { const char *model; unsigned int bridges[3]; unsigned int count; } cases[] = {
        {"DX4700+", {0x4db8,0x4dbf,0}, 2},
        {"DXP4800 Plus", {0x463d,0x464d,0}, 2},
        {"DXP4800 Pro", {0xa73d,0xa74d,0}, 2},
        {"DXP6800", {0x464d,0x51b8,0x463d}, 3},
        {"DXP6800 Plus", {0x464d,0x51b8,0x463d}, 3},
        {"DXP6800 Pro", {0x464d,0x51b8,0x463d}, 3},
        {"DXP6800 Ultra", {0xa74d,0x51b8,0xa73d}, 3},
        {"DXP8800", {0x464d,0x51b8,0x463d}, 3},
        {"DXP8800 Plus", {0x464d,0x51b8,0x463d}, 3},
        {"DXP8800 Pro", {0x464d,0x51b8,0x463d}, 3},
        {"DXP8800 Ultra", {0xa74d,0x51b8,0xa73d}, 3},
        {"iDX6011 Pro", {0x7eca,0x7ecb,0}, 2}
    };
    static const char *const old_block_name_models[] = {
        "DH2600", "DXP2800", "DX4600", "DX4600+", "DX4600 Pro", "DX4700", "DXP4800", "DXP4800S"
    };
    static const char *const names[] = {"nvme71n1", "nvme3n1", "nvme19n1"};
    static const char *const roots[] = {"0000:00:06.0", "0000:00:1c.4", "0000:00:1c.6"};
    static const char *const endpoints[] = {"0000:99:00.0", "0000:12:00.0", "0000:47:00.0"};
    struct ugreenctl_disk_scan scan;
    const struct ugreenctl_disk *disk;
    char error[256], slot[32], path[4096];
    size_t i;
    unsigned int n;
    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        setup();
        for (n = 0; n < cases[i].count; ++n)
            add_nvme(names[n], roots[n], cases[i].bridges[n], endpoints[n], "FIRMWARE-CONNECTOR");
        CHECK(ugreenctl_disk_scan_at(fixture, cases[i].model, NULL, &scan, error, sizeof(error)) == 0);
        CHECK(scan.count == cases[i].count);
        for (n = 0; n < cases[i].count; ++n) {
            snprintf(slot, sizeof(slot), "nvme%u", n + 1);
            disk = find_disk(&scan, names[n]);
            CHECK(disk->mapped && disk->slot == n + 1 && !strcmp(disk->slot_name, slot));
            CHECK(!disk->led_supported && !disk->led_id[0]);
            CHECK(ugreenctl_disk_resolve(&scan, slot, &disk, error, sizeof(error)) == -ENOTSUP && disk == NULL);
        }
        teardown();
    }
    setup();
    add_nvme("nvme0n1", roots[0], 0x4db8, endpoints[0], "NAME-ZERO");
    add_nvme("nvme1n1", roots[1], 0x4dbf, endpoints[1], "NAME-ONE");
    for (i = 0; i < sizeof(old_block_name_models)/sizeof(old_block_name_models[0]); ++i) {
        CHECK(ugreenctl_disk_scan_at(fixture, old_block_name_models[i], NULL, &scan, error, sizeof(error)) == 0);
        CHECK(scan.count == 2 && !find_disk(&scan, "nvme0n1")->mapped && !find_disk(&scan, "nvme1n1")->mapped);
        CHECK(strstr(find_disk(&scan, "nvme0n1")->reason, "nvmeN names only") != NULL);
        CHECK(strstr(find_disk(&scan, "nvme0n1")->reason, "scripts/collect-disk-evidence.sh") != NULL);
    }
    CHECK(ugreenctl_disk_scan_at(fixture, "Future Unknown Model", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "nvme0n1")->mapped);
    CHECK(strstr(find_disk(&scan, "nvme0n1")->reason, "NVMe connector layout") != NULL);
    CHECK(strstr(find_disk(&scan, "nvme0n1")->reason, "scripts/collect-disk-evidence.sh") != NULL);
    CHECK(ugreenctl_disk_resolve(&scan, "nvme0n1", &disk, error, sizeof(error)) != 0 && disk == NULL);
    teardown();
    /* An empty second bridge with the same ID is ambiguous too; checking only
     * installed drives would silently choose a physical connector here. */
    setup();
    add_nvme("nvme71n1", roots[0], 0x463d, endpoints[0], "OCCUPIED");
    add_nvme("nvme3n1", roots[1], 0x463d, endpoints[1], "REMOVED");
    snprintf(path, sizeof(path), "%s/class/block/nvme3n1", fixture); CHECK(unlink(path) == 0);
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 1 && !find_disk(&scan, "nvme71n1")->mapped);
    teardown();
}

static void test_nvme_root_path_matrix(void)
{
    struct ugreenctl_disk_scan scan;
    const struct ugreenctl_disk *disk;
    char error[256];
    setup();
    add_nvme("nvme99n1", "0000:00:06.0", 0x0001, "0000:97:00.0", "480T-ONE");
    add_nvme("nvme42n1", "0000:00:06.2", 0x0002, "0000:13:00.0", "480T-TWO");
    add_nvme("nvme3n1", "0000:00:1c.2", 0x51b2, "0000:91:00.0", "480T-THREE");
    add_nvme("nvme0n1", "0000:00:1d.0", 0x51b8, "0000:33:00.0", "480T-FOUR");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP480T Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "nvme99n1")->slot == 1 && find_disk(&scan, "nvme42n1")->slot == 2);
    CHECK(find_disk(&scan, "nvme3n1")->slot == 3 && find_disk(&scan, "nvme0n1")->slot == 4);
    CHECK(ugreenctl_disk_resolve(&scan, "nvme4", &disk, error, sizeof(error)) == -ENOTSUP && disk == NULL);
    teardown();
    setup();
    add_nvme("nvme0n1", "0000:00:01.6", 0x0001, "0000:93:00.0", "GT-M2-ONE");
    add_nvme("nvme42n1", "0000:00:01.4", 0x0002, "0000:13:00.0", "GT-M2-TWO");
    add_nvme("nvme3n1", "0000:00:01.1", 0x0003, "0000:91:00.0", "GT-U2-ONE");
    add_nvme("nvme99n1", "0000:00:01.3", 0x0004, "0000:33:00.0", "GT-U2-TWO");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 GT", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!strcmp(find_disk(&scan, "nvme0n1")->slot_name, "nvme1"));
    CHECK(!strcmp(find_disk(&scan, "nvme42n1")->slot_name, "nvme2"));
    CHECK(!strcmp(find_disk(&scan, "nvme3n1")->slot_name, "ata1"));
    CHECK(!strcmp(find_disk(&scan, "nvme99n1")->slot_name, "ata2"));
    CHECK(ugreenctl_disk_resolve(&scan, "slot2", &disk, error, sizeof(error)) == 0 && !strcmp(disk->led_id, "disk2"));
    teardown();
    setup();
    add_nvme("nvme91n1", "0000:00:01.0", 0x0001, "0000:99:00.0", "IDX-PCIE");
    CHECK(ugreenctl_disk_scan_at(fixture, "iDX6011 Pro", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "nvme91n1")->mapped && !strcmp(find_disk(&scan, "nvme91n1")->slot_name, "nvme3"));
    teardown();
}

static void test_gt_firmware_revision_mapping(void)
{
    static const unsigned int ports[] = {11,61};
    static const char *const bios_versions[] = {
        NULL, "\n", "LL_DXP2800GT_001\n", "RETAIL_DXP2800GT_002\n",
        "FUTURE_DXP2800GT_999\n", "UNKNOWN_LL_NEW_BOARD\n"
    };
    struct ugreenctl_disk_scan scan;
    const struct ugreenctl_disk *disk;
    char error[256], path[4096];
    size_t i;
    setup();
    add_sata_controller("0000:00:08.2", 0x1022, 0x7901, 2, ports);
    add_disk("sdz", "0000:00:08.2", 11, 1, 90, "GT-SATA-FIRST");
    add_disk("sda", "0000:00:08.2", 61, 2, 190, "GT-SATA-SECOND");
    snprintf(path, sizeof(path), "%s/class/dmi/id/bios_version", fixture);
    for (i = 0; i < sizeof(bios_versions) / sizeof(bios_versions[0]); ++i) {
        if (bios_versions[i]) write_file(path, bios_versions[i]);
        CHECK(ugreenctl_disk_scan_at(fixture, "DXP2800 GT", NULL, &scan, error, sizeof(error)) == 0);
        CHECK(!strcmp(scan.profile, "DXP2800 GT/revision-unknown"));
        CHECK(!find_disk(&scan, "sdz")->mapped && !find_disk(&scan, "sda")->mapped);
        CHECK(!find_disk(&scan, "sdz")->led_supported && !find_disk(&scan, "sdz")->led_id[0]);
        CHECK(!find_disk(&scan, "sda")->slot && !find_disk(&scan, "sda")->slot_name[0]);
        CHECK(!strcmp(find_disk(&scan, "sdz")->mapping_source, "unknown"));
        CHECK(strstr(find_disk(&scan, "sdz")->reason, "BIOS/SATA routing is unverified") != NULL);
        CHECK(ugreenctl_disk_resolve(&scan, "sdz", &disk, error, sizeof(error)) != 0 && disk == NULL);
        CHECK(ugreenctl_disk_resolve(&scan, "slot1", &disk, error, sizeof(error)) != 0 && disk == NULL);
    }
    /* Administrator calibration remains explicit and replaces the whole map. */
    snprintf(path, sizeof(path), "%s/calibration", fixture);
    write_file(path, "model=DXP2800 GT\nslot1 topology=pci-0000:00:08.2-ata-2\nslot2 topology=pci-0000:00:08.2-ata-1\n");
    CHECK(chmod(path, 0600) == 0);
    if (geteuid() == 0) {
        CHECK(ugreenctl_disk_scan_at(fixture, "DXP2800 GT", path, &scan, error, sizeof(error)) == 0);
        CHECK(!strcmp(scan.profile, "administrator-calibration"));
        CHECK(find_disk(&scan, "sdz")->slot == 2 && find_disk(&scan, "sda")->slot == 1);
        CHECK(!strcmp(find_disk(&scan, "sdz")->mapping_source, "administrator-calibration"));
        CHECK(ugreenctl_disk_resolve(&scan, "sdz", &disk, error, sizeof(error)) == 0 && disk->slot == 2);
    } else {
        CHECK(ugreenctl_disk_scan_at(fixture, "DXP2800 GT", path, &scan, error, sizeof(error)) != 0);
    }
    teardown();

    setup();
    add_nvme("nvme91n1", "0000:00:01.1", 0x0001, "0000:77:00.0", "GT-U2-ONE");
    add_nvme("nvme73n1", "0000:00:01.2", 0x0002, "0000:23:00.0", "GT-U2-TWO");
    add_nvme("nvme42n1", "0000:00:01.6", 0x0003, "0000:93:00.0", "GT-M2-ONE");
    add_nvme("nvme99n1", "0000:00:01.3", 0x0004, "0000:33:00.0", "GT-M2-TWO");
    snprintf(path, sizeof(path), "%s/class/dmi/id/bios_version", fixture);
    for (i = 0; i < sizeof(bios_versions) / sizeof(bios_versions[0]); ++i) {
        if (bios_versions[i]) write_file(path, bios_versions[i]);
        CHECK(ugreenctl_disk_scan_at(fixture, "DXP2800 GT", NULL, &scan, error, sizeof(error)) == 0);
        CHECK(!strcmp(scan.profile, "DXP2800 GT/revision-unknown"));
        CHECK(find_disk(&scan, "nvme91n1")->mapped && find_disk(&scan, "nvme91n1")->led_supported);
        CHECK(!strcmp(find_disk(&scan, "nvme91n1")->slot_name, "ata1"));
        CHECK(!strcmp(find_disk(&scan, "nvme91n1")->interface_type, "u2"));
        CHECK(!strcmp(find_disk(&scan, "nvme91n1")->led_id, "disk1"));
        CHECK(find_disk(&scan, "nvme73n1")->mapped && !strcmp(find_disk(&scan, "nvme73n1")->led_id, "disk2"));
        CHECK(find_disk(&scan, "nvme42n1")->mapped && !find_disk(&scan, "nvme42n1")->led_supported);
        CHECK(find_disk(&scan, "nvme99n1")->mapped && !find_disk(&scan, "nvme99n1")->led_supported);
        CHECK(!strcmp(find_disk(&scan, "nvme42n1")->slot_name, "nvme1"));
        CHECK(!strcmp(find_disk(&scan, "nvme99n1")->slot_name, "nvme2"));
        CHECK(ugreenctl_disk_resolve(&scan, "disk2", &disk, error, sizeof(error)) == 0 && disk->slot == 2);
        CHECK(ugreenctl_disk_resolve(&scan, "nvme1", &disk, error, sizeof(error)) == -ENOTSUP && disk == NULL);
    }
    /* One unknown SATA route could own either shared bay, never just the bay
     * suggested by a global ATA number or the unverified revision selector. */
    add_sata_controller("0000:00:08.2", 0x1022, 0x7901, 2, ports);
    add_disk("sdz", "0000:00:08.2", 11, 1, 90, "GT-UNKNOWN-SATA");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP2800 GT", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "sdz")->mapped);
    CHECK(!find_disk(&scan, "nvme91n1")->mapped && !find_disk(&scan, "nvme73n1")->mapped);
    CHECK(!find_disk(&scan, "nvme91n1")->led_id[0] && !find_disk(&scan, "nvme73n1")->led_id[0]);
    CHECK(!find_disk(&scan, "nvme91n1")->slot && !find_disk(&scan, "nvme73n1")->slot);
    CHECK(!find_disk(&scan, "nvme91n1")->slot_name[0] && !find_disk(&scan, "nvme73n1")->slot_name[0]);
    CHECK(!find_disk(&scan, "nvme91n1")->led_supported && !find_disk(&scan, "nvme73n1")->led_supported);
    CHECK(!strcmp(find_disk(&scan, "nvme91n1")->mapping_source, "unknown"));
    CHECK(strstr(find_disk(&scan, "nvme91n1")->reason, "Unverified BIOS/SATA routing") != NULL);
    CHECK(find_disk(&scan, "nvme42n1")->mapped && find_disk(&scan, "nvme99n1")->mapped);
    CHECK(ugreenctl_disk_resolve(&scan, "disk1", &disk, error, sizeof(error)) != 0 && disk == NULL);
    CHECK(ugreenctl_disk_resolve(&scan, "disk2", &disk, error, sizeof(error)) != 0 && disk == NULL);
    teardown();

    setup();
    /* Identical revision-independent rules do not bypass duplicate-bay checks. */
    add_nvme("nvme91n1", "0000:00:01.1", 0x0001, "0000:77:00.0", "GT-U2-DUPLICATE-ONE");
    add_nvme("nvme73n1", "0000:00:01.1", 0x0001, "0000:23:00.0", "GT-U2-DUPLICATE-TWO");
    add_nvme("nvme42n1", "0000:00:01.6", 0x0003, "0000:93:00.0", "GT-INDEPENDENT-M2");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP2800 GT", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "nvme91n1")->mapped && !find_disk(&scan, "nvme73n1")->mapped);
    CHECK(strstr(find_disk(&scan, "nvme91n1")->reason, "Conflicting or duplicate") != NULL);
    CHECK(find_disk(&scan, "nvme42n1")->mapped);
    teardown();
}

static void test_gt_four_sata_and_shared_u2_bays(void)
{
    static const unsigned int ports[] = {47,2,88,119};
    struct ugreenctl_disk_scan scan;
    const struct ugreenctl_disk *disk;
    char error[256], path[4096];
    setup();
    add_asm1164("0000:99:00.0", ports);
    attach_sata_bridge("0000:99:00.0", "0000:00:01.2", 0x0001, 4, ports);
    snprintf(path, sizeof(path), "%s/devices/pci0000:00/0000:00:01.2/vendor", fixture); write_file(path, "0x1022\n");
    add_disk("sdz", "0000:99:00.0", 88, 3, 190, "GT-SATA-THIRD");
    add_disk("sda", "0000:99:00.0", 47, 1, 81, "GT-SATA-FIRST");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 GT", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "sdz")->mapped && find_disk(&scan, "sdz")->slot == 3);
    CHECK(find_disk(&scan, "sda")->slot == 1);
    add_nvme("nvme91n1", "0000:00:01.1", 0x0002, "0000:67:00.0", "GT-U2-SHARED-FIRST");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 GT", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "sda")->mapped && !find_disk(&scan, "nvme91n1")->mapped);
    CHECK(find_disk(&scan, "sdz")->mapped && find_disk(&scan, "sdz")->slot == 3);
    CHECK(ugreenctl_disk_resolve(&scan, "slot1", &disk, error, sizeof(error)) != 0 && disk == NULL);
    add_nvme("nvme91n2", "0000:00:01.1", 0x0002, "0000:67:00.0", "GT-U2-SHARED-FIRST");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 GT", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 3 && scan.namespace_count == 2);
    CHECK(!find_disk(&scan, "sda")->mapped && !find_disk(&scan, "nvme91n1")->mapped);
    CHECK(find_disk(&scan, "sdz")->mapped && find_disk(&scan, "sdz")->slot == 3);
    CHECK(ugreenctl_disk_resolve(&scan, "/dev/nvme91n2", &disk, error, sizeof(error)) != 0 && disk == NULL);
    snprintf(path, sizeof(path), "%s/class/ata_port/ata2", fixture); CHECK(unlink(path) == 0);
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 GT", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(!find_disk(&scan, "sdz")->mapped); /* Missing empty controller port invalidates SATA. */
    teardown();
}

static void test_nvme_namespace_aliases(void)
{
    struct ugreenctl_disk_scan scan;
    const struct ugreenctl_disk *disk;
    char error[256], path[4096];
    setup();
    add_nvme("nvme5n2", "0000:00:01.1", 2, "0000:67:00.0", "ONE-PHYSICAL-SSD");
    add_nvme("nvme5n12", "0000:00:01.1", 2, "0000:67:00.0", "ONE-PHYSICAL-SSD");
    snprintf(path, sizeof(path), "%s/class/block/nvme5n2/wwid", fixture);
    write_file(path, "eui.0022334455667788\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 GT", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 1 && scan.namespace_count == 2);
    CHECK(scan.disks[0].mapped && scan.disks[0].led_supported && !strcmp(scan.disks[0].led_id, "disk1"));
    CHECK(!strcmp(scan.disks[0].name, "nvme5n12"));
    CHECK(ugreenctl_disk_resolve(&scan, "/dev/nvme5n2", &disk, error, sizeof(error)) == 0 && disk == &scan.disks[0]);
    CHECK(ugreenctl_disk_resolve(&scan, "nvme5n12", &disk, error, sizeof(error)) == 0 && disk == &scan.disks[0]);
    CHECK(ugreenctl_disk_resolve(&scan, "serial:ONE-PHYSICAL-SSD", &disk, error, sizeof(error)) == 0);
    CHECK(ugreenctl_disk_resolve(&scan, "wwn:eui.0022334455667788", &disk, error, sizeof(error)) == 0);
    CHECK(ugreenctl_disk_resolve(&scan, "wwn:eui.0011223344556677", &disk, error, sizeof(error)) == 0);
    CHECK(ugreenctl_disk_resolve(&scan, "slot1", &disk, error, sizeof(error)) == 0);
    for (size_t i = 0; i < scan.namespace_count; ++i)
        CHECK(!strcmp(scan.namespace_aliases[i].disk_name, scan.disks[0].name));
    /* A shared serial does not merge a second physical controller/bay. */
    add_nvme("nvme8n1", "0000:00:01.3", 3, "0000:73:00.0", "ONE-PHYSICAL-SSD");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 GT", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 2 && scan.namespace_count == 3);
    CHECK(ugreenctl_disk_resolve(&scan, "serial:ONE-PHYSICAL-SSD", &disk, error, sizeof(error)) != 0 && disk == NULL);
    CHECK(strstr(error, "ambiguous") != NULL);
    CHECK(ugreenctl_disk_resolve(&scan, "/dev/nvme8n1", &disk, error, sizeof(error)) == 0 && disk->slot == 2);
    teardown();
    setup();
    add_nvme("nvme5n1", "0000:00:06.0", 2, "0000:67:00.0", "M2-NO-LED");
    add_nvme("nvme5n2", "0000:00:06.0", 2, "0000:67:00.0", "M2-NO-LED");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP480T Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 1 && scan.namespace_count == 2 && scan.disks[0].mapped);
    CHECK(ugreenctl_disk_resolve(&scan, "/dev/nvme5n2", &disk, error, sizeof(error)) == -ENOTSUP && disk == NULL);
    /* Separate controller objects sharing a PCI function are not namespaces. */
    add_nvme("nvme9n1", "0000:00:06.0", 2, "0000:67:00.0", "M2-NO-LED");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP480T Plus", NULL, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 2 && !scan.disks[0].mapped && !scan.disks[1].mapped);
    teardown();
    setup();
    add_nvme("nvme5n1", "0000:00:01.1", 2, "0000:67:00.0", "OLD-CONTROLLER");
    add_nvme("nvme5n2", "0000:00:01.1", 2, "0000:67:00.0", "REPLACED-CONTROLLER");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 GT", NULL, &scan, error, sizeof(error)) != 0);
    CHECK(scan.count == 0 && scan.namespace_count == 0 && strstr(error, "controller identity") != NULL);
    teardown();
}

static void test_hotplug_inventory_recheck(void)
{
    static const unsigned int ports[] = {47,2,88,119};
    struct ugreenctl_disk_scan scan;
    char error[256], path[4096], serial_path[4096];
    pid_t writer;
    int status, result;
    for (unsigned int scenario = 0; scenario < 4; ++scenario) {
        const unsigned int trigger = scenario < 2 ? 3 : 5;
        const unsigned int partial = scenario % 2;
        setup();
        add_asm1164("0000:99:00.0", ports);
        attach_sata_bridge("0000:99:00.0", "0000:00:01.2", 1, 4, ports);
        snprintf(path, sizeof(path), "%s/devices/pci0000:00/0000:00:01.2/vendor", fixture);
        write_file(path, "0x1022\n");
        add_disk("sda", "0000:99:00.0", 47, 1, 81, "GT-SATA-FIRST");
        snprintf(serial_path, sizeof(serial_path), "%s/class/block/sda/device/serial", fixture);
        CHECK(unlink(serial_path) == 0 && mkfifo(serial_path, 0600) == 0);
        alarm(10);
        writer = fork();
        CHECK(writer >= 0);
        if (writer == 0) {
            unsigned int reads = 0;
            for (;;) {
                static const char serial[] = "GT-SATA-FIRST\n";
                const struct timespec delay = {0, 20000000};
                int fd = open(serial_path, O_WRONLY);
                if (fd < 0) _exit(2);
                /* Third read: end-of-scan identity check after the initial
                 * class/block stream reached EOF. Fifth read: slow identity
                 * check inside the final membership walk, whose existing
                 * readdir buffer/EOF cookie can hide a new shared-bay peer.
                 * A fresh name-only directory pass must catch either arrival,
                 * even before the new member's transport is initialized. */
                if (++reads == trigger) {
                    add_nvme("nvme91n1", "0000:00:01.1", 2, "0000:67:00.0", "GT-U2-ARRIVED");
                    if (partial) {
                        snprintf(path, sizeof(path), "%s/class/block/nvme91n1/device/transport", fixture);
                        CHECK(unlink(path) == 0);
                    }
                }
                if (write(fd, serial, sizeof(serial) - 1) != (ssize_t)(sizeof(serial) - 1) || close(fd))
                    _exit(3);
                (void)nanosleep(&delay, NULL);
            }
        }
        result = ugreenctl_disk_scan_at(fixture, "DXP4800 GT", NULL, &scan, error, sizeof(error));
        CHECK(kill(writer, SIGTERM) == 0 && waitpid(writer, &status, 0) == writer);
        alarm(0);
        CHECK(result != 0 && scan.count == 0 && scan.namespace_count == 0);
        CHECK(strstr(error, "inventory changed") != NULL);
        CHECK(unlink(serial_path) == 0);
        write_file(serial_path, "GT-SATA-FIRST\n");
        snprintf(path, sizeof(path), "%s/class/block/nvme91n1/device/transport", fixture);
        write_file(path, "pcie\n");
        CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 GT", NULL, &scan, error, sizeof(error)) == 0);
        CHECK(scan.count == 2 && !find_disk(&scan, "sda")->mapped && !find_disk(&scan, "nvme91n1")->mapped);
        teardown();
    }
}

static void test_calibration(void)
{
    struct ugreenctl_disk_scan scan;
    char error[256], path[4096], symlink_path[4096];
    setup();
    add_disk("sda", "0000:05:00.0", 31, 3, 79, "USER-BAY");
    add_disk("sdb", "0000:05:00.0", 32, 1, 80, "UNCONFIRMED");
    snprintf(path, sizeof(path), "%s/calibration", fixture);
    write_file(path, "model=DXP4800 Plus\nslot1 topology=pci-0000:05:00.0-ata-3\n");
    CHECK(chmod(path, 0600) == 0);
    if (geteuid() != 0) {
        CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", path, &scan, error, sizeof(error)) != 0);
        teardown();
        return;
    }
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", path, &scan, error, sizeof(error)) == 0);
    CHECK(scan.count == 2 && find_disk(&scan, "sda")->slot == 1);
    CHECK(!find_disk(&scan, "sdb")->mapped);
    CHECK(!strcmp(find_disk(&scan, "sda")->mapping_source, "administrator-calibration"));
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Pro", path, &scan, error, sizeof(error)) != 0);
    CHECK(chmod(path, 0666) == 0);
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", path, &scan, error, sizeof(error)) != 0);
    CHECK(chmod(path, 0600) == 0);
    snprintf(symlink_path, sizeof(symlink_path), "%s/link-calibration", fixture);
    CHECK(symlink(path, symlink_path) == 0);
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", symlink_path, &scan, error, sizeof(error)) != 0);
    write_file(path, "model=DXP4800 Plus\nslot1 topology=pci-0000:05:00.0-ata-3\nslot2 topology=pci-0000:05:00.0-ata-3\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", path, &scan, error, sizeof(error)) != 0);
    write_file(path, "model=DXP4800 Plus\nslot1 topology=pci-0000:05:00.0-ata-3\nslot1 topology=pci-0000:05:00.0-ata-1\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", path, &scan, error, sizeof(error)) != 0);
    write_file(path, "model=DXP4800 Plus\nslot1 serial=USER-BAY\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", path, &scan, error, sizeof(error)) != 0);
    write_file(path, "model=DXP4800 Plus\nslot5 topology=pci-0000:05:00.0-ata-3\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP4800 Plus", path, &scan, error, sizeof(error)) != 0);
    write_file(path, "model=DXP2800\nslot3 topology=pci-0000:05:00.0-ata-3\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP2800", path, &scan, error, sizeof(error)) != 0);
    write_file(path, "model=DXP6800 Pro\nslot7 topology=pci-0000:05:00.0-ata-3\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "DXP6800 Pro", path, &scan, error, sizeof(error)) != 0);
    write_file(path, "model=Future Unknown\nslot8 topology=pci-0000:05:00.0-ata-3\n");
    CHECK(ugreenctl_disk_scan_at(fixture, "Future Unknown", path, &scan, error, sizeof(error)) == 0);
    CHECK(find_disk(&scan, "sda")->slot == 8);
    teardown();
}

#endif

int main(void)
{
    test_selectors();
#ifdef __linux__
    test_sparse_ata_and_hosts();
    test_6800pro_layout();
    test_firmware_controller_role_layouts();
    test_arm_platform_inventory();
    test_arm_conditional_of_mapping();
    test_filters_and_unknown();
    test_conflicts();
    test_cached_vpd_serial();
    test_plus_pro_single_controller();
    test_dxp2800_em_single_controller();
    test_nvme_bridge_mapping();
    test_official_nvme_connector_matrix();
    test_nvme_root_path_matrix();
    test_gt_firmware_revision_mapping();
    test_gt_four_sata_and_shared_u2_bays();
    test_nvme_namespace_aliases();
    test_hotplug_inventory_recheck();
    test_calibration();
#endif
    puts("disk mapping tests passed");
    return 0;
}
