// SPDX-License-Identifier: GPL-2.0
/*
 * aod_voltages.c — AMD AOD (Overclocking Data) memory voltage reader
 *
 * Locates the AMD AOD SystemMemory OperationRegion by parsing ACPI SSDT tables,
 * maps it with memremap, then exposes voltage candidates via sysfs at:
 *   /sys/kernel/aod_voltages/scan       — all float-range values in DSPD
 *   /sys/kernel/aod_voltages/mem_vddio  — MemVddio (VDD)
 *   /sys/kernel/aod_voltages/mem_vddq   — MemVddq
 *   /sys/kernel/aod_voltages/mem_vpp    — MemVpp
 *
 * Offsets for the named voltages are set via module parameters after
 * identifying them from the scan output:
 *   modprobe aod_voltages off_vddio=N off_vddq=N off_vpp=N
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/memremap.h>
#include <linux/string.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TuxTimings");
MODULE_DESCRIPTION("AMD AOD memory voltage reader");
MODULE_VERSION("0.1");

/* AOD SSDT OEM table ID (space-padded to 8 bytes) */
#define AOD_OEM_ID   "AOD     "

/*
 * Layout of the AODE OperationRegion (from SSDT9 Field definition):
 *   OUTB  1568 bits = 196 bytes  (offset    0)  — SMI output buffer
 *   AQVS    32 bits =   4 bytes  (offset  196)
 *   SCMI    32 bits =   4 bytes  (offset  200)
 *   SCMD    32 bits =   4 bytes  (offset  204)
 *   DSPD 68128 bits = 8516 bytes (offset  208)  — XMP/timing profiles
 *   RESV    96 bits =  12 bytes  (offset 8724)
 *   RMPD  1120 bits = 140 bytes  (offset 8736)
 *   WCNS  4096 bits = 512 bytes  (offset 8876)  — OC settings/voltages
 *   ...
 *
 * ZenStates-Core Granite Ridge AOD offsets are absolute from AODE start:
 *   MemVddio = 9084  (WCNS + 208)
 *   MemVddq  = 9088  (WCNS + 212)
 *   MemVpp   = 9092  (WCNS + 216)
 */
#define AOD_REGION_SIZE  0x24BB
#define WCNS_OFFSET      8876
#define WCNS_SIZE        512
/* Scan the full region to catch data in any field */
#define SCAN_START       4      /* skip first 4 bytes (status/version) */
#define SCAN_END         AOD_REGION_SIZE

/*
 * Voltages are stored as unsigned 32-bit integers in millivolts.
 * e.g. 1550 mV = 0x0000060E, 1800 mV = 0x00000708
 * Filter range covers all realistic DDR/CPU rails.
 */
#define MV_MIN   500U
#define MV_MAX   3000U

/*
 * AML byte pattern for:  OpRegion (AODE, SystemMemory, ...)
 *   5B 80        — DefOpRegion opcode
 *   41 4F 44 45  — NameSeg 'AODE'
 *   00           — RegionSpace = SystemMemory
 */
static const u8 aode_pattern[] = {
    0x5B, 0x80, 0x41, 0x4F, 0x44, 0x45, 0x00
};

static void *aod_base;          /* remapped AOD region          */
static struct kobject *aod_kobj;

/* Module parameters: byte offsets of each voltage in the AODE region.
 * Defaults are the Granite Ridge ZenStates-Core values (AGESA > 0xB404022).
 * Override if scan shows different offsets on your board. */
static int off_vddio = 9084;
static int off_vddq  = 9088;
static int off_vpp   = 9092;

module_param(off_vddio, int, 0644);
MODULE_PARM_DESC(off_vddio, "Byte offset of MemVddio (VDD) in AOD region");
module_param(off_vddq, int, 0644);
MODULE_PARM_DESC(off_vddq,  "Byte offset of MemVddq in AOD region");
module_param(off_vpp, int, 0644);
MODULE_PARM_DESC(off_vpp,   "Byte offset of MemVpp in AOD region");

/* Read a u32 millivolt value from the AOD region at byte offset. */
static u32 read_mv(int offset)
{
    u32 v;

    if (!aod_base || offset < 0 || offset + 4 > AOD_REGION_SIZE)
        return 0;
    memcpy(&v, (u8 *)aod_base + offset, 4);
    return v;
}

/* sysfs: scan — list all millivolt-range integers in the AOD region with their offsets */
static ssize_t scan_show(struct kobject *kobj,
                         struct kobj_attribute *attr, char *buf)
{
    ssize_t len = 0;
    int     i;

    if (!aod_base)
        return scnprintf(buf, PAGE_SIZE, "error: AOD region not mapped\n");

    len += scnprintf(buf + len, PAGE_SIZE - len,
                     "offset  hex     field  value\n"
                     "------  ------  -----  -------\n");

    for (i = SCAN_START; i < SCAN_END - 4; i += 4) {
        u32 mv = read_mv(i);

        if (mv < MV_MIN || mv > MV_MAX)
            continue;

        const char *field =
            (i <  196) ? "OUTB" :
            (i <  208) ? "CTRL" :
            (i < 8724) ? "DSPD" :
            (i < 8736) ? "RESV" :
            (i < 8876) ? "RMPD" :
            (i < 9388) ? "WCNS" : "TAIL";
        len += scnprintf(buf + len, PAGE_SIZE - len,
                         "%6d  0x%04X  %-4s  %u mV (%u.%03u V)\n",
                         i, i, field, mv, mv / 1000, mv % 1000);

        if (len >= PAGE_SIZE - 64)
            break;
    }

    if (len <= 32) /* only header printed */
        len += scnprintf(buf + len, PAGE_SIZE - len,
                         "(no voltage-range values found)\n");
    return len;
}

static ssize_t show_named(struct kobject *kobj,
                           struct kobj_attribute *attr,
                           char *buf, int offset)
{
    u32 mv;

    if (!aod_base)
        return scnprintf(buf, PAGE_SIZE, "error: not mapped\n");
    if (offset < 0 || offset + 4 > AOD_REGION_SIZE)
        return scnprintf(buf, PAGE_SIZE,
                         "unset — reload with: modprobe aod_voltages off_vddio=N ...\n");

    mv = read_mv(offset);
    return scnprintf(buf, PAGE_SIZE, "%u mV (%u.%03u V)\n",
                     mv, mv / 1000, mv % 1000);
}

static ssize_t mem_vddio_show(struct kobject *k, struct kobj_attribute *a, char *b)
{ return show_named(k, a, b, off_vddio); }

static ssize_t mem_vddq_show(struct kobject *k, struct kobj_attribute *a, char *b)
{ return show_named(k, a, b, off_vddq); }

static ssize_t mem_vpp_show(struct kobject *k, struct kobj_attribute *a, char *b)
{ return show_named(k, a, b, off_vpp); }

/*
 * raw_wcns — hex dump of WCNS field (offset 8876, 512 bytes).
 * Also dumps OUTB (offset 0, 196 bytes) which holds SMI results.
 * Use this to see the actual data format when scan returns nothing.
 */
static ssize_t raw_wcns_show(struct kobject *kobj,
                              struct kobj_attribute *attr, char *buf)
{
    ssize_t len = 0;
    int i;

    if (!aod_base)
        return scnprintf(buf, PAGE_SIZE, "error: not mapped\n");

    len += scnprintf(buf + len, PAGE_SIZE - len, "=== OUTB (0x000, 196 bytes) ===\n");
    for (i = 0; i < 196 && len < PAGE_SIZE - 48; i += 4) {
        u32 v;
        memcpy(&v, (u8 *)aod_base + i, 4);
        if (i % 16 == 0)
            len += scnprintf(buf + len, PAGE_SIZE - len, "%04X: ", i);
        len += scnprintf(buf + len, PAGE_SIZE - len, "%08X ", v);
        if (i % 16 == 12)
            len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
    }

    len += scnprintf(buf + len, PAGE_SIZE - len, "\n=== WCNS (0x%04X, 512 bytes) ===\n",
                     WCNS_OFFSET);
    for (i = WCNS_OFFSET; i < WCNS_OFFSET + WCNS_SIZE && len < PAGE_SIZE - 48; i += 4) {
        u32 v;
        memcpy(&v, (u8 *)aod_base + i, 4);
        if ((i - WCNS_OFFSET) % 16 == 0)
            len += scnprintf(buf + len, PAGE_SIZE - len, "%04X: ", i);
        len += scnprintf(buf + len, PAGE_SIZE - len, "%08X ", v);
        if ((i - WCNS_OFFSET) % 16 == 12)
            len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
    }
    return len;
}

static struct kobj_attribute scan_attr    = __ATTR_RO(scan);
static struct kobj_attribute vddio_attr   = __ATTR_RO(mem_vddio);
static struct kobj_attribute vddq_attr    = __ATTR_RO(mem_vddq);
static struct kobj_attribute vpp_attr     = __ATTR_RO(mem_vpp);
static struct kobj_attribute wcns_attr    = __ATTR_RO(raw_wcns);

static struct attribute *aod_attrs[] = {
    &scan_attr.attr,
    &vddio_attr.attr,
    &vddq_attr.attr,
    &vpp_attr.attr,
    &wcns_attr.attr,
    NULL,
};
static struct attribute_group aod_attr_group = { .attrs = aod_attrs };

/*
 * Parse all SSDT tables for the one with OEM table ID "AOD     ",
 * then scan its AML for the DefOpRegion AODE SystemMemory pattern and
 * extract the physical address.
 */
static phys_addr_t find_aod_phys(void)
{
    struct acpi_table_header *hdr;
    acpi_status status;
    u32 idx;

    for (idx = 1; ; idx++) {
        u8  *aml;
        u32  aml_len, i;

        status = acpi_get_table("SSDT", idx, &hdr);
        if (ACPI_FAILURE(status))
            break;

        if (memcmp(hdr->oem_table_id, AOD_OEM_ID, 8) != 0) {
            acpi_put_table(hdr);
            continue;
        }

        aml     = (u8 *)hdr + sizeof(*hdr);
        aml_len = hdr->length - (u32)sizeof(*hdr);

        for (i = 0; i + sizeof(aode_pattern) + 10 < aml_len; i++) {
            phys_addr_t addr = 0;
            u8 enc;

            if (memcmp(aml + i, aode_pattern, sizeof(aode_pattern)) != 0)
                continue;

            enc = aml[i + sizeof(aode_pattern)];

            if (enc == 0x0C) {
                /* DWordConst: 4-byte LE address */
                u32 tmp32;
                memcpy(&tmp32, aml + i + sizeof(aode_pattern) + 1, 4);
                addr = tmp32;
            } else if (enc == 0x0E) {
                /* QWordConst: 8-byte LE address */
                memcpy(&addr, aml + i + sizeof(aode_pattern) + 1, 8);
            } else {
                continue;
            }

            pr_info("aod_voltages: AODE region phys=0x%llx size=0x%x\n",
                    (unsigned long long)addr, AOD_REGION_SIZE);
            acpi_put_table(hdr);
            return addr;
        }

        pr_warn("aod_voltages: found AOD SSDT but no AODE OpRegion pattern\n");
        acpi_put_table(hdr);
    }

    return 0;
}

static int __init aod_voltages_init(void)
{
    phys_addr_t phys = find_aod_phys();

    if (!phys) {
        pr_err("aod_voltages: AOD SSDT / AODE region not found\n");
        return -ENODEV;
    }

    aod_base = memremap(phys, AOD_REGION_SIZE, MEMREMAP_WB);
    if (!aod_base) {
        pr_err("aod_voltages: memremap(0x%llx) failed\n",
               (unsigned long long)phys);
        return -ENOMEM;
    }

    aod_kobj = kobject_create_and_add("aod_voltages", kernel_kobj);
    if (!aod_kobj) {
        memunmap(aod_base);
        return -ENOMEM;
    }

    if (sysfs_create_group(aod_kobj, &aod_attr_group) != 0) {
        kobject_put(aod_kobj);
        memunmap(aod_base);
        return -ENOMEM;
    }

    pr_info("aod_voltages: ready — /sys/kernel/aod_voltages/scan\n");
    if (off_vddio >= 0 || off_vddq >= 0 || off_vpp >= 0)
        pr_info("aod_voltages: offsets vddio=%d vddq=%d vpp=%d\n",
                off_vddio, off_vddq, off_vpp);
    else
        pr_info("aod_voltages: run 'cat /sys/kernel/aod_voltages/scan' to find offsets\n");

    return 0;
}

static void __exit aod_voltages_exit(void)
{
    if (aod_kobj) {
        sysfs_remove_group(aod_kobj, &aod_attr_group);
        kobject_put(aod_kobj);
    }
    if (aod_base)
        memunmap(aod_base);

    pr_info("aod_voltages: unloaded\n");
}

module_init(aod_voltages_init);
module_exit(aod_voltages_exit);
