// SPDX-License-Identifier: GPL-2.0
/*
 * intel.c — Intel DDR5 timing reader for TuxTimings
 *
 * Reads memory controller timings from MCHBAR (Memory Controller Host Base
 * Address Register) via /dev/mem for Intel 12th–15th gen CPUs.
 *
 * Register layout reference: pyhwinfo (memory.py) by intommy/others
 * Supports: Alder Lake (i12), Raptor Lake (i13/14), Arrow Lake (i15)
 */

#include "intel.h"

/*
 * Intel CPU spoof mode — build with:  make INTEL_DEBUG=1
 * Defines SPOOF_INTEL_CPU and plausible DDR5-6000 values so the Intel
 * UI layout can be tested on AMD (or any) hardware without reboot.
 */
#ifdef SPOOF_INTEL_CPU
#  ifndef SPOOF_INTEL_MODEL
#    define SPOOF_INTEL_MODEL    0xB7
#  endif
#  ifndef SPOOF_INTEL_CODENAME
#    define SPOOF_INTEL_CODENAME "Raptor Lake (13/14th gen) [SPOOFED]"
#  endif
#  ifndef SPOOF_INTEL_MICROCODE
#    define SPOOF_INTEL_MICROCODE "0x0000002E"
#  endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <dirent.h>
#include <ctype.h>

/* ── MCHBAR constants ──────────────────────────────────────────────────── */

#define MCHBAR_SIZE        0x20000U
#define MCHBAR_PCI_OFFSET  0x48        /* offset in PCI config space of 0000:00:00.0 */

/* Per-channel base: 0xE000 + (0x800 * channel).  We read channel 0. */
#define CH_BASE            0xE000U
#define CH_STRIDE          0x0800U

/* Per-channel register offsets (relative to CH_BASE) */
#define OFF_TC_PRE         0x000U   /* tRP, tRAS, tRCD (i12/14), tWRPRE, tRDPRE */
#define OFF_TC_ACT_I12     0x008U   /* tFAW, tRRD — i12/14 */
#define OFF_TC_ACT_I15     0x138U   /* tFAW, tRRD, tRCD — i15 (Arrow Lake) */
#define OFF_RDRD           0x00CU   /* tRDRD variants */
#define OFF_RDWR           0x010U   /* tRDWR */
#define OFF_WRRD           0x014U   /* tWRRD */
#define OFF_WRWR           0x018U   /* tWRWR */
#define OFF_RTL            0x020U   /* tRTL */
#define OFF_PWDEN          0x050U   /* tCKE, tXP */
#define OFF_CAS            0x070U   /* tCL, tCWL */
#define OFF_GS_CFG         0x088U   /* GEAR, CMD_STRETCH */
#define OFF_REFRESH_I12    0x43CU   /* tREFI, tRFC — i12/14 */
#define OFF_REFRESH_I15    0x4A0U   /* tREFI, tRFC, tRFCpb — i15 */
#define OFF_RFCPB_I12      0x488U   /* tRFCpb — i12/14 */
#define OFF_RFP            0x438U   /* tREFIx9, refresh watermarks */
#define OFF_SREXITTP       0x4C0U   /* tXSR — self-refresh exit */

/* DDR5 burst length (half-cycle counting: BL16 / 2 = 8) */
#define DDR5_BL 8U

/* ── MCHBAR mapping state ──────────────────────────────────────────────── */

static volatile uint8_t *s_mchbar = NULL;   /* mmap'd region */

/* ── Bit extraction helper ─────────────────────────────────────────────── */

static inline uint32_t bits32(uint32_t v, int hi, int lo)
{
    uint32_t mask = (hi == 31) ? 0xFFFFFFFFU : ((1U << (hi + 1)) - 1U);
    return (v & mask) >> lo;
}

static inline uint64_t bits64(uint64_t v, int hi, int lo)
{
    uint64_t mask = (hi == 63) ? UINT64_MAX : (((uint64_t)1 << (hi + 1)) - 1ULL);
    return (v & mask) >> lo;
}

/* ── MCHBAR register reads ─────────────────────────────────────────────── */

static uint32_t mc_read32(uint32_t off)
{
    uint32_t v;
    if (!s_mchbar || off + 4 > MCHBAR_SIZE) return 0;
    memcpy(&v, (const void *)(s_mchbar + off), 4);
    return v;
}

static uint64_t mc_read64(uint32_t off)
{
    uint64_t v;
    if (!s_mchbar || off + 8 > MCHBAR_SIZE) return 0;
    memcpy(&v, (const void *)(s_mchbar + off), 8);
    return v;
}

/* ── PCI config + mmap ─────────────────────────────────────────────────── */

static uint64_t read_mchbar_base(void)
{
    /* MCHBAR BAR is 8 bytes at offset 0x48 in PCI config space of 0000:00:00.0 */
    int fd = open("/sys/bus/pci/devices/0000:00:00.0/config", O_RDONLY);
    if (fd < 0) return 0;
    uint64_t val = 0;
    ssize_t n = pread(fd, &val, 8, MCHBAR_PCI_OFFSET);
    close(fd);
    if (n != 8) return 0;
    if (!(val & 1)) return 0;   /* enable bit not set */
    return val & ~(uint64_t)1;  /* strip enable bit to get physical address */
}

static int map_mchbar(void)
{
    if (s_mchbar) return 1;

    uint64_t base = read_mchbar_base();
    if (!base) return 0;

    int fd = open("/dev/mem", O_RDONLY);
    if (fd < 0) return 0;

    void *m = mmap(NULL, MCHBAR_SIZE, PROT_READ, MAP_SHARED, fd, (off_t)base);
    close(fd);
    if (m == MAP_FAILED) return 0;

    s_mchbar = (volatile uint8_t *)m;
    return 1;
}

void intel_cleanup(void)
{
    if (s_mchbar) {
        munmap((void *)s_mchbar, MCHBAR_SIZE);
        s_mchbar = NULL;
    }
}

/* ── CPU detection ─────────────────────────────────────────────────────── */

int intel_detect_cpu(cpu_info_t *out_cpu)
{
#ifdef SPOOF_INTEL_CPU
    out_cpu->vendor    = CPU_VENDOR_INTEL;
    out_cpu->intel_gen = INTEL_GEN_12_14;
    out_cpu->cpu_model = SPOOF_INTEL_MODEL;
    snprintf(out_cpu->codename,          sizeof(out_cpu->codename),
             "%s", SPOOF_INTEL_CODENAME);
    snprintf(out_cpu->microcode_version, sizeof(out_cpu->microcode_version),
             "%s", SPOOF_INTEL_MICROCODE);
    snprintf(out_cpu->processor_name,    sizeof(out_cpu->processor_name),
             "Intel Core i9-13900K [SPOOFED]");
    snprintf(out_cpu->name,              sizeof(out_cpu->name),
             "Intel Core i9-13900K [SPOOFED]");
    return 1;
#endif

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return 0;

    int     is_intel   = 0;
    uint32_t model     = 0;
    uint32_t microcode = 0;
    char     proc_name[STR_LEN] = {0};
    int      got_model = 0;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        const char *colon = strchr(line, ':');
        if (!colon) continue;
        const char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;

        if (strncmp(line, "vendor_id", 9) == 0) {
            if (strstr(val, "GenuineIntel")) is_intel = 1;
        } else if (strncmp(line, "model name", 10) == 0 && !proc_name[0]) {
            snprintf(proc_name, sizeof(proc_name), "%s", val);
            /* Strip trailing newline */
            size_t l = strlen(proc_name);
            while (l > 0 && (proc_name[l-1] == '\n' || proc_name[l-1] == '\r'))
                proc_name[--l] = '\0';
        } else if (strncmp(line, "model\t", 6) == 0 && !got_model) {
            sscanf(val, "%u", &model);
            got_model = 1;
        } else if (strncmp(line, "microcode", 9) == 0) {
            sscanf(val, "%x", &microcode);
        }

        /* Stop after first CPU block once we have everything */
        if (is_intel && got_model && microcode && proc_name[0])
            break;
    }
    fclose(f);

    if (!is_intel) return 0;

    out_cpu->vendor    = CPU_VENDOR_INTEL;
    out_cpu->cpu_model = model;
    snprintf(out_cpu->microcode_version, sizeof(out_cpu->microcode_version),
             "0x%08X", microcode);
    snprintf(out_cpu->processor_name, sizeof(out_cpu->processor_name),
             "%s", proc_name);
    snprintf(out_cpu->name, sizeof(out_cpu->name), "%s", proc_name);

    /* Map model to generation and codename */
    switch (model) {
    case 0x97: case 0x9A:
        out_cpu->intel_gen = INTEL_GEN_12_14;
        snprintf(out_cpu->codename, sizeof(out_cpu->codename),
                 "Alder Lake (12th gen)");
        break;
    case 0xB7: case 0xBF:
        out_cpu->intel_gen = INTEL_GEN_12_14;
        snprintf(out_cpu->codename, sizeof(out_cpu->codename),
                 "Raptor Lake (13/14th gen)");
        break;
    case 0xC6:
        out_cpu->intel_gen = INTEL_GEN_15;
        snprintf(out_cpu->codename, sizeof(out_cpu->codename),
                 "Arrow Lake (15th gen)");
        break;
    default:
        out_cpu->intel_gen = INTEL_GEN_UNKNOWN;
        snprintf(out_cpu->codename, sizeof(out_cpu->codename),
                 "Intel (model 0x%02X)", model);
        break;
    }

    return 1;
}

/* ── Timing reader ─────────────────────────────────────────────────────── */

void intel_read_timings(intel_gen_t gen, dram_timings_t *d)
{
    memset(d, 0, sizeof(*d));

#ifdef SPOOF_INTEL_CPU
    /* Spoofed: return plausible DDR5-6000 timings for UI testing */
    d->tcl      = 30;
    d->trcd_rd  = 36;
    d->trcd_wr  = 36;
    d->trp      = 36;
    d->tras     = 68;
    d->trc      = d->tras + d->trp;
    d->tfaw     = 16;
    d->trrds    = 8;
    d->trrdl    = 8;
    d->tcwl     = 28;
    d->cke      = 7;
    d->xp       = 7;
    d->refi     = 9360;
    d->rfc      = 410;
    d->rfcsb    = 128;
    d->rdrd_sc  = 8;
    d->rdrd_sd  = 8;
    d->rdrd_dd  = 8;
    d->rdwr     = 14;
    d->wrrd     = 14;
    d->wrwr_sc  = 8;
    d->wrwr_sd  = 8;
    d->wrwr_dd  = 10;
    d->twr      = 48;
    d->wtrl     = 12;
    d->wtrs     = 4;
    d->trtl            = 90;
    d->trtl_per_rank[0] = 90; d->trtl_per_rank[1] = 90;
    d->trtl_per_rank[2] = 25; d->trtl_per_rank[3] = 90;
    d->trtl_rank_count  = 4;
    d->intel_gear = 2;
    d->wrpre    = 24;
    d->rdpre    = 6;
    d->intel_channel_count = 2;
    d->xp_dll   = 63;
    d->rdpden   = 50;
    d->wrpden   = 126;
    d->prpden   = 2;
    d->tcsl     = 6;
    d->tcsh     = 34;
    d->cpded    = 13;
    d->refi_x9  = 255;
    d->txsr     = 1062;
    d->refsbrd  = 79;
    d->tppd     = 4;
    snprintf(d->cmd2t, sizeof(d->cmd2t), "1N");
    return;
#endif

    if (!map_mchbar()) return;

    const uint32_t cb = CH_BASE;   /* channel 0 */

    /* ── 0x000: TC_PRE ──────────────────────────────────────────────── */
    uint64_t tc_pre = mc_read64(cb + OFF_TC_PRE);

    d->trp  = (uint32_t)bits64(tc_pre, 7,  0);
    d->tppd = (uint32_t)bits64(tc_pre, 23, 20);

    if (gen == INTEL_GEN_12_14) {
        d->rdpre   = (uint32_t)bits64(tc_pre, 19, 13);
        d->trcd_wr = (uint32_t)bits64(tc_pre, 31, 24);
        d->wrpre   = (uint32_t)bits64(tc_pre, 41, 32);
        d->tras    = (uint32_t)bits64(tc_pre, 50, 42);
        d->trcd_rd = (uint32_t)bits64(tc_pre, 58, 51);
    } else {
        /* Arrow Lake: tRAS at [53:45], tRCD in TC_ACT at 0x138 */
        d->rdpre   = (uint32_t)bits64(tc_pre, 26, 20);
        d->wrpre   = (uint32_t)bits64(tc_pre, 42, 33);
        d->tras    = (uint32_t)bits64(tc_pre, 53, 45);
    }

    /* ── TC_ACT: tFAW, tRRD, tRCD (i15 only) ───────────────────────── */
    uint32_t act_off = (gen == INTEL_GEN_12_14) ? OFF_TC_ACT_I12 : OFF_TC_ACT_I15;
    uint64_t tc_act  = mc_read64(cb + act_off);

    d->tfaw    = (uint32_t)bits64(tc_act, 8,  0);
    d->trrds   = (uint32_t)bits64(tc_act, 14, 9);
    d->trrdl   = (uint32_t)bits64(tc_act, 21, 15);
    d->refsbrd = (uint32_t)bits64(tc_act, 31, 24);

    if (gen == INTEL_GEN_15) {
        d->trcd_rd = (uint32_t)bits64(tc_act, 29, 22);
        d->trcd_wr = (uint32_t)bits64(tc_act, 39, 32);
    }

    /* ── 0x00C: RDRD ────────────────────────────────────────────────────
     *  [6:0]   tRDRD_sg (same group)        → rdrd_sc
     *  [14:8]  tRDRD_dg (different group)   → rdrd_sd
     *  [23:16] tRDRD_dr (different rank)    — skipped (no struct field)
     *  [31:24] tRDRD_dd (different DIMM)    → rdrd_dd              ── */
    uint32_t rdrd = mc_read32(cb + OFF_RDRD);
    d->rdrd_sc = bits32(rdrd,  6, 0);
    d->rdrd_sd = bits32(rdrd, 14, 8);
    d->rdrd_dd = bits32(rdrd, 31, 24);

    /* ── 0x010: RDWR ────────────────────────────────────────────────── */
    d->rdwr = bits32(mc_read32(cb + OFF_RDWR), 7, 0);

    /* ── 0x014: WRRD — bits [8:0] = tWRRD_sg, bits [17:9] = tWRRD_dg ── */
    uint32_t wrrd_raw = mc_read32(cb + OFF_WRRD);
    d->wrrd           = bits32(wrrd_raw,  8, 0);   /* tWRRD_sg */

    /* ── 0x018: WRWR ────────────────────────────────────────────────────
     *  [6:0]   tWRWR_sg (same group)        → wrwr_sc
     *  [14:8]  tWRWR_dg (different group)   → wrwr_sd
     *  [22:16] tWRWR_dr (different rank)    — skipped (no struct field)
     *  [31:24] tWRWR_dd (different DIMM)    → wrwr_dd              ── */
    uint32_t wrwr = mc_read32(cb + OFF_WRWR);
    d->wrwr_sc = bits32(wrwr,  6,  0);
    d->wrwr_sd = bits32(wrwr, 14,  8);
    d->wrwr_dd = bits32(wrwr, 31, 24);

    /* ── 0x020: RTL — 8 bytes, one rank latency per byte ───────────── */
    {
        uint64_t rtl = mc_read64(cb + OFF_RTL);
        int nr = 0;
        for (int i = 0; i < 8; i++) {
            uint32_t v = (uint32_t)((rtl >> (i * 8)) & 0xFF);
            d->trtl_per_rank[i] = v;
            if (v > 0) nr = i + 1;
        }
        d->trtl_rank_count = nr ? nr : 1;
        d->trtl = d->trtl_per_rank[0];
    }

    /* ── 0x050: PWDEN — full power-down timing register ────────────────
     *  [6:0]   tCKE      [13:7]  tXP      [20:14] tXP_DLL
     *  [28:21] tRDPDEN   [41:32] tWRPDEN  [47:42] tCSH
     *  [53:48] tCSL      [63:59] tPRPDEN                           ── */
    uint64_t pwden = mc_read64(cb + OFF_PWDEN);
    d->cke    = (uint32_t)bits64(pwden,  6,  0);
    d->xp     = (uint32_t)bits64(pwden, 13,  7);
    d->xp_dll = (uint32_t)bits64(pwden, 20, 14);
    d->rdpden = (uint32_t)bits64(pwden, 28, 21);
    d->wrpden = (uint32_t)bits64(pwden, 41, 32);
    d->tcsh   = (uint32_t)bits64(pwden, 47, 42);
    d->tcsl   = (uint32_t)bits64(pwden, 53, 48);
    d->prpden = (uint32_t)bits64(pwden, 63, 59);

    /* ── 0x070: CAS — tCL, tCWL ────────────────────────────────────── */
    uint64_t cas = mc_read64(cb + OFF_CAS);
    d->tcl  = (uint32_t)bits64(cas, 22, 16);
    d->tcwl = (uint32_t)bits64(cas, 31, 24);

    /* ── 0x088: GS_CFG — GEAR, CMD_STRETCH, tCPDED ─────────────────── */
    uint64_t gs = mc_read64(cb + OFF_GS_CFG);
    d->cpded = (uint32_t)bits64(gs, 60, 56);
    if (gen == INTEL_GEN_12_14) {
        uint32_t cmd = (uint32_t)bits64(gs, 4, 3);
        static const char *cr_map[] = {"1N", "2N", "3N", "N:1"};
        snprintf(d->cmd2t, sizeof(d->cmd2t), "%s", cr_map[cmd & 3]);
        int gear4 = (int)bits64(gs, 15, 15);
        int gear2 = (int)bits64(gs, 31, 31);
        d->intel_gear = gear4 ? 4 : (gear2 ? 2 : 1);
    } else {
        uint32_t cmd = (uint32_t)bits64(gs, 3, 3);
        snprintf(d->cmd2t, sizeof(d->cmd2t), "%s", cmd ? "2N" : "1N");
        d->intel_gear = (int)bits64(gs, 8, 8) ? 2 : 4;
    }

    /* ── Refresh: tREFI, tRFC, tRFCpb ──────────────────────────────── */
    if (gen == INTEL_GEN_12_14) {
        uint64_t ref = mc_read64(cb + OFF_REFRESH_I12);
        d->refi  = (uint32_t)bits64(ref, 17, 0);
        d->rfc   = (uint32_t)bits64(ref, 30, 18);
        uint32_t rfcpb = mc_read32(cb + OFF_RFCPB_I12);
        d->rfcsb = bits32(rfcpb, 20, 10);
    } else {
        uint64_t ref = mc_read64(cb + OFF_REFRESH_I15);
        d->refi  = (uint32_t)bits64(ref, 17, 0);
        d->rfc   = (uint32_t)bits64(ref, 30, 18);
        d->rfcsb = (uint32_t)bits64(ref, 50, 40);
    }

    /* ── 0x438: TC_RFP — tREFIx9 watermark ─────────────────────────── */
    d->refi_x9 = bits32(mc_read32(cb + OFF_RFP), 31, 24);

    /* ── 0x4C0: TC_SREXITTP — self-refresh exit ─────────────────────── */
    d->txsr = bits32(mc_read32(cb + OFF_SREXITTP), 12, 0);

    /* Derived */
    d->trc = d->tras + d->trp;
    d->intel_channel_count = 2;   /* assume dual-channel; refine if needed */

    /* Calculated timings */
    if (d->tcwl > 0) {
        if (d->wrpre > d->tcwl + DDR5_BL)
            d->twr  = d->wrpre - d->tcwl - DDR5_BL;
        /* tWTR_L uses wrrd (same-dimm / sg path) */
        if (d->wrrd > d->tcwl + DDR5_BL + 2)
            d->wtrl = d->wrrd - d->tcwl - DDR5_BL - 2;
    }
    /* tWTR_S: bits [17:9] of same WRRD register (dg path) */
    {
        uint32_t wrrd_dg = bits32(wrrd_raw, 17, 9);
        if (d->tcwl > 0 && wrrd_dg > d->tcwl + DDR5_BL + 2)
            d->wtrs = wrrd_dg - d->tcwl - DDR5_BL - 2;
    }
}

/* ── Voltage reader ────────────────────────────────────────────────────── */

void intel_read_voltages(smu_metrics_t *m)
{
    /* Package power via Intel RAPL powercap sysfs */
    DIR *dir = opendir("/sys/devices/virtual/powercap/intel-rapl");
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char name_path[640], name_buf[64];
        snprintf(name_path, sizeof(name_path),
                 "/sys/devices/virtual/powercap/intel-rapl/%s/name",
                 ent->d_name);

        FILE *f = fopen(name_path, "r");
        if (!f) continue;
        int ok = (fgets(name_buf, sizeof(name_buf), f) != NULL);
        fclose(f);
        if (!ok) continue;

        /* Strip newline */
        size_t l = strlen(name_buf);
        while (l > 0 && (name_buf[l-1] == '\n' || name_buf[l-1] == '\r'))
            name_buf[--l] = '\0';

        if (strcmp(name_buf, "package-0") != 0) continue;

        char pw_path[640];
        snprintf(pw_path, sizeof(pw_path),
                 "/sys/devices/virtual/powercap/intel-rapl/%s/constraint_0_power_limit_uw",
                 ent->d_name);
        FILE *pf = fopen(pw_path, "r");
        if (pf) {
            long uw = 0;
            if (fscanf(pf, "%ld", &uw) == 1 && uw > 0)
                m->ppt_w = (float)uw / 1000000.0f;
            fclose(pf);
        }
        break;
    }
    closedir(dir);

    /*
     * SA voltage, VDDQ TX, VccDD2: no standard Linux sysfs interface
     * without a specialized driver. Fields remain 0; UI displays "N/A".
     */
}
