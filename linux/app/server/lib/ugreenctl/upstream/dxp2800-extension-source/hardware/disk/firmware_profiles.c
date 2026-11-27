/* Official UGOS Pro 1.20 hardware facts. See docs/ALL_MODELS_DISK_EVIDENCE.zh-CN.md. */
#include "disk/firmware_profiles.h"
#include <string.h>

static const struct ugreenctl_disk_firmware_rule rows_0_0[] = {
    {"ata1", "ata_id", "ata2", "sata", false},
    {"ata2", "ata_id", "ata1", "sata", false},
    {"nvme1", "block_name", "nvme0n1", "m2", false},
    {"nvme2", "block_name", "nvme1n1", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_1_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"nvme1", "block_name", "nvme0n1", "m2", false},
    {"nvme2", "block_name", "nvme1n1", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_1_1[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"nvme1", "block_name", "nvme0n1", "m2", false},
    {"nvme2", "block_name", "nvme1n1", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_2_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"nvme1", "block_name", "nvme0n1", "m2", false},
    {"nvme2", "block_name", "nvme1n1", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_3_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"nvme1", "block_name", "nvme0n1", "m2", false},
    {"nvme2", "block_name", "nvme1n1", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_4_0[] = {
    {"ata1", "syspath", "devices/pci0000:00/0000:00:08.2|ata1", "sata", true},
    {"ata2", "syspath", "devices/pci0000:00/0000:00:08.2|ata2", "sata", true},
    {"ata1", "syspath", "devices/pci0000:00/0000:00:01.1|nvme", "u2", true},
    {"ata2", "syspath", "devices/pci0000:00/0000:00:01.2|nvme", "u2", true},
    {"nvme1", "syspath", "devices/pci0000:00/0000:00:01.6|nvme", "m2", false},
    {"nvme2", "syspath", "devices/pci0000:00/0000:00:01.3|nvme", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_4_1[] = {
    {"ata1", "syspath", "devices/pci0000:00/0000:00:08.2|ata2", "sata", true},
    {"ata2", "syspath", "devices/pci0000:00/0000:00:08.2|ata1", "sata", true},
    {"ata1", "syspath", "devices/pci0000:00/0000:00:01.1|nvme", "u2", true},
    {"ata2", "syspath", "devices/pci0000:00/0000:00:01.2|nvme", "u2", true},
    {"nvme1", "syspath", "devices/pci0000:00/0000:00:01.6|nvme", "m2", false},
    {"nvme2", "syspath", "devices/pci0000:00/0000:00:01.3|nvme", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_5_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"nvme1", "block_name", "nvme0n1", "m2", false},
    {"nvme2", "block_name", "nvme1n1", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_6_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"nvme1", "block_name", "nvme0n1", "m2", false},
    {"nvme2", "block_name", "nvme1n1", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_7_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"nvme1", "block_name", "nvme0n1", "m2", false},
    {"nvme2", "block_name", "nvme1n1", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_8_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:4db8", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:4dbf", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_9_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"nvme1", "block_name", "nvme0n1", "m2", false},
    {"nvme2", "block_name", "nvme1n1", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_10_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"nvme1", "block_name", "nvme0n1", "m2", false},
    {"nvme2", "block_name", "nvme1n1", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_11_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:463d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:464d", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_12_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:a73d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:a74d", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_12_1[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:a73d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:a74d", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_13_0[] = {
    {"ata1", "syspath", "devices/pci0000:00/0000:00:01.2|ata1", "sata", true},
    {"ata2", "syspath", "devices/pci0000:00/0000:00:01.2|ata2", "sata", true},
    {"ata3", "syspath", "devices/pci0000:00/0000:00:01.2|ata3", "sata", true},
    {"ata4", "syspath", "devices/pci0000:00/0000:00:01.2|ata4", "sata", true},
    {"ata1", "syspath", "devices/pci0000:00/0000:00:01.1|nvme", "u2", true},
    {"ata2", "syspath", "devices/pci0000:00/0000:00:01.3|nvme", "u2", true},
    {"nvme1", "syspath", "devices/pci0000:00/0000:00:01.6|nvme", "m2", false},
    {"nvme2", "syspath", "devices/pci0000:00/0000:00:01.4|nvme", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_14_0[] = {
    {"ata1", "syspath", "devices/pci0000:00/0000:00:06.0|ata1", "sata", true},
    {"ata2", "syspath", "devices/pci0000:00/0000:00:06.0|ata2", "sata", true},
    {"ata3", "syspath", "devices/pci0000:00/0000:00:06.0|ata3", "sata", true},
    {"ata4", "syspath", "devices/pci0000:00/0000:00:06.0|ata4", "sata", true},
    {"nvme1", "syspath", "devices/pci0000:00/0000:00:1c.2|nvme", "m2", false},
    {"nvme2", "syspath", "devices/pci0000:00/0000:00:1c.3|nvme", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_15_0[] = {
    {"ata1", "ata_id", "ata3", "sata", true},
    {"ata2", "ata_id", "ata4", "sata", true},
    {"ata3", "ata_id", "ata5", "sata", true},
    {"ata4", "ata_id", "ata6", "sata", true},
    {"ata5", "ata_id", "ata1", "sata", true},
    {"ata6", "ata_id", "ata2", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:464d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:51b8", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:463d", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_16_0[] = {
    {"ata1", "ata_id", "ata3", "sata", true},
    {"ata2", "ata_id", "ata4", "sata", true},
    {"ata3", "ata_id", "ata5", "sata", true},
    {"ata4", "ata_id", "ata6", "sata", true},
    {"ata5", "ata_id", "ata1", "sata", true},
    {"ata6", "ata_id", "ata2", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:a74d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:51b8", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:a73d", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_17_0[] = {
    {"ata1", "ata_id", "ata3", "sata", true},
    {"ata2", "ata_id", "ata4", "sata", true},
    {"ata3", "ata_id", "ata5", "sata", true},
    {"ata4", "ata_id", "ata6", "sata", true},
    {"ata5", "ata_id", "ata1", "sata", true},
    {"ata6", "ata_id", "ata2", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:464d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:51b8", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:463d", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_18_0[] = {
    {"ata1", "ata_id", "ata3", "sata", true},
    {"ata2", "ata_id", "ata4", "sata", true},
    {"ata3", "ata_id", "ata5", "sata", true},
    {"ata4", "ata_id", "ata6", "sata", true},
    {"ata5", "ata_id", "ata1", "sata", true},
    {"ata6", "ata_id", "ata2", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:464d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:51b8", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:463d", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_19_0[] = {
    {"ata1", "ata_id", "ata3", "sata", true},
    {"ata2", "ata_id", "ata4", "sata", true},
    {"ata3", "ata_id", "ata5", "sata", true},
    {"ata4", "ata_id", "ata6", "sata", true},
    {"ata5", "ata_id", "ata1", "sata", true},
    {"ata6", "ata_id", "ata2", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:464d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:51b8", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:463d", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_20_0[] = {
    {"ata1", "ata_id", "ata3", "sata", true},
    {"ata2", "ata_id", "ata4", "sata", true},
    {"ata3", "ata_id", "ata5", "sata", true},
    {"ata4", "ata_id", "ata6", "sata", true},
    {"ata5", "ata_id", "ata1", "sata", true},
    {"ata6", "ata_id", "ata2", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:a74d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:51b8", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:a73d", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_21_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"ata5", "ata_id", "ata5", "sata", true},
    {"ata6", "ata_id", "ata6", "sata", true},
    {"ata7", "ata_id", "ata7", "sata", true},
    {"ata8", "ata_id", "ata8", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:464d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:51b8", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:463d", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_22_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"ata5", "ata_id", "ata5", "sata", true},
    {"ata6", "ata_id", "ata6", "sata", true},
    {"ata7", "ata_id", "ata7", "sata", true},
    {"ata8", "ata_id", "ata8", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:a74d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:51b8", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:a73d", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_23_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"ata5", "ata_id", "ata5", "sata", true},
    {"ata6", "ata_id", "ata6", "sata", true},
    {"ata7", "ata_id", "ata7", "sata", true},
    {"ata8", "ata_id", "ata8", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:464d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:51b8", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:463d", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_24_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"ata5", "ata_id", "ata5", "sata", true},
    {"ata6", "ata_id", "ata6", "sata", true},
    {"ata7", "ata_id", "ata7", "sata", true},
    {"ata8", "ata_id", "ata8", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:464d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:51b8", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:463d", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_25_0[] = {
    {"ata1", "ata_id", "ata1", "sata", true},
    {"ata2", "ata_id", "ata2", "sata", true},
    {"ata3", "ata_id", "ata3", "sata", true},
    {"ata4", "ata_id", "ata4", "sata", true},
    {"ata5", "ata_id", "ata5", "sata", true},
    {"ata6", "ata_id", "ata6", "sata", true},
    {"ata7", "ata_id", "ata7", "sata", true},
    {"ata8", "ata_id", "ata8", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:a74d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:51b8", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:a73d", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_26_0[] = {
    {"nvme1", "pcie_address", "pci0000:00/0000:00:06.0/0000:01:00.0", "m2", false},
    {"nvme2", "pcie_address", "pci0000:00/0000:00:06.2/0000:02:00.0", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:51b2", "m2", false},
    {"nvme4", "pcie_bridge_id", "8086:51b8", "m2", false},
};

static const struct ugreenctl_disk_firmware_rule rows_27_0[] = {
    {"ata1", "ata_id", "ata3", "sata", true},
    {"ata2", "ata_id", "ata4", "sata", true},
    {"ata3", "ata_id", "ata5", "sata", true},
    {"ata4", "ata_id", "ata6", "sata", true},
    {"ata5", "ata_id", "ata1", "sata", true},
    {"ata6", "ata_id", "ata2", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:7e4d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:7eca", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:7ecc", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_28_0[] = {
    {"ata1", "ata_id", "ata3", "sata", true},
    {"ata2", "ata_id", "ata4", "sata", true},
    {"ata3", "ata_id", "ata5", "sata", true},
    {"ata4", "ata_id", "ata6", "sata", true},
    {"ata5", "ata_id", "ata1", "sata", true},
    {"ata6", "ata_id", "ata2", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:7e4d", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:7eca", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:7ecc", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_29_0[] = {
    {"ata1", "ata_id", "ata3", "sata", true},
    {"ata2", "ata_id", "ata4", "sata", true},
    {"ata3", "ata_id", "ata1", "sata", true},
    {"ata4", "ata_id", "ata2", "sata", true},
    {"ata5", "ata_id", "ata5", "sata", true},
    {"ata6", "ata_id", "ata6", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:7eca", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:7ecb", "m2", false},
    {"nvme3", "pcie_address", "pci0000:00/0000:00:01.0/0000:01:00.0", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_30_0[] = {
    {"ata1", "ata_id", "ata3", "sata", true},
    {"ata2", "ata_id", "ata4", "sata", true},
    {"ata3", "ata_id", "ata1", "sata", true},
    {"ata4", "ata_id", "ata2", "sata", true},
    {"ata5", "ata_id", "ata5", "sata", true},
    {"ata6", "ata_id", "ata6", "sata", true},
    {"nvme1", "pcie_bridge_id", "8086:e361", "m2", false},
    {"nvme2", "pcie_bridge_id", "8086:e366", "m2", false},
    {"nvme3", "pcie_bridge_id", "8086:e365", "pcie", false},
};

static const struct ugreenctl_disk_firmware_rule rows_32_0[] = {
    {"ata1", "ata_id", "ata2", "sata", false},
    {"ata2", "ata_id", "ata1", "sata", false},
};

static const struct ugreenctl_disk_firmware_rule rows_32_1[] = {
    {"ata1", "ata_id", "ata2", "sata", false},
    {"ata2", "ata_id", "ata1", "sata", false},
};

static const struct ugreenctl_disk_firmware_rule rows_32_2[] = {
    {"ata1", "ata_id", "ata2", "sata", false},
    {"ata2", "ata_id", "ata1", "sata", false},
};

static const struct ugreenctl_disk_firmware_rule rows_33_0[] = {
    {"ata1", "ata_id", "ata2", "sata", false},
    {"ata2", "ata_id", "ata1", "sata", false},
    {"ata3", "ata_id", "ata4", "sata", false},
    {"ata4", "ata_id", "ata3", "sata", false},
};

static const struct ugreenctl_disk_firmware_rule rows_33_1[] = {
    {"ata1", "ata_id", "ata2", "sata", false},
    {"ata2", "ata_id", "ata1", "sata", false},
    {"ata3", "ata_id", "ata4", "sata", false},
    {"ata4", "ata_id", "ata3", "sata", false},
};

static const struct ugreenctl_disk_firmware_layout layouts[] = {
    {"DH2600", "rev1", rows_0_0, 4},
    {"DXP2800", "rev1", rows_1_0, 4},
    {"DXP2800", "rev2", rows_1_1, 4},
    {"DXP2800S", "rev1", rows_2_0, 4},
    {"DX4600 Pro", "rev1", rows_3_0, 6},
    {"DXP2800 GT", "rev1", rows_4_0, 6},
    {"DXP2800 GT", "rev2", rows_4_1, 6},
    {"DX4600+", "rev1", rows_5_0, 6},
    {"DX4600", "rev1", rows_6_0, 6},
    {"DX4700", "rev1", rows_7_0, 6},
    {"DX4700+", "rev1", rows_8_0, 6},
    {"DXP4800", "rev1", rows_9_0, 6},
    {"DXP4800S", "rev1", rows_10_0, 6},
    {"DXP4800 Plus", "rev1", rows_11_0, 6},
    {"DXP4800 Pro", "rev1", rows_12_0, 6},
    {"DXP4800 Pro", "rev2", rows_12_1, 6},
    {"DXP4800 GT", "rev1", rows_13_0, 8},
    {"DXP4900", "rev1", rows_14_0, 6},
    {"DXP6800", "rev1", rows_15_0, 9},
    {"DXP6800S", "rev1", rows_16_0, 9},
    {"DXP6800 Plus", "rev1", rows_17_0, 9},
    {"DXP6800 Pro", "rev1", rows_18_0, 9},
    {"FORT 6", "rev1", rows_19_0, 9},
    {"DXP6800 Ultra", "rev1", rows_20_0, 9},
    {"DXP8800", "rev1", rows_21_0, 11},
    {"DXP8800S", "rev1", rows_22_0, 11},
    {"DXP8800 Plus", "rev1", rows_23_0, 11},
    {"DXP8800 Pro", "rev1", rows_24_0, 11},
    {"DXP8800 Ultra", "rev1", rows_25_0, 11},
    {"DXP480T Plus", "rev1", rows_26_0, 4},
    {"iDX6011", "rev1", rows_27_0, 9},
    {"iDX6012", "rev1", rows_28_0, 9},
    {"iDX6011 Pro", "rev1", rows_29_0, 9},
    {"iDX6021", "rev1", rows_30_0, 9},
    {"DH2300", "rev1", rows_32_0, 2},
    {"DH2300", "rev2", rows_32_1, 2},
    {"DH2300", "rev3", rows_32_2, 2},
    {"DH4300 Plus", "rev1", rows_33_0, 4},
    {"DH4300 Plus", "rev2", rows_33_1, 4},
};

const struct ugreenctl_disk_firmware_layout *ugreenctl_disk_firmware_layout(
    const char *model, const char *revision)
{
    size_t i;
    if (!model || !revision) return NULL;
    for (i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i)
        if (!strcmp(model, layouts[i].model) && !strcmp(revision, layouts[i].revision))
            return &layouts[i];
    return NULL;
}
