#include "dram.h"
#include "util.h"
#include <string.h>
#include <stdio.h>

#define SMN_PATH "/sys/kernel/ryzen_smu_drv/smn"

static uint32_t read_smn(uint32_t address)
{
    FILE *f = fopen(SMN_PATH, "r+b");
    if (!f) return 0;
    /* Write address (little-endian) */
    uint8_t buf[4];
    buf[0] = (uint8_t)(address);
    buf[1] = (uint8_t)(address >> 8);
    buf[2] = (uint8_t)(address >> 16);
    buf[3] = (uint8_t)(address >> 24);
    if (fwrite(buf, 1, 4, f) != 4) { fclose(f); return 0; }
    fflush(f);
    /* Read back value */
    rewind(f);
    if (fread(buf, 1, 4, f) != 4) { fclose(f); return 0; }
    fclose(f);
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static float to_nanoseconds(uint32_t cycles, float freq_mhz)
{
    if (freq_mhz <= 0) return 0;
    float ns = (float)cycles * 2000.0f / freq_mhz;
    if (ns > (float)cycles) ns /= 2.0f;
    return ns;
}

/* Common timing extraction from SMN registers — shared between DDR4 and DDR5 */
static void read_common_timings(uint32_t offset, dram_timings_t *d)
{
    uint32_t reg50204 = read_smn(offset | 0x50204);
    uint32_t reg50208 = read_smn(offset | 0x50208);
    uint32_t reg5020C = read_smn(offset | 0x5020C);
    uint32_t reg50210 = read_smn(offset | 0x50210);
    uint32_t reg50214 = read_smn(offset | 0x50214);
    uint32_t reg50218 = read_smn(offset | 0x50218);
    uint32_t reg5021C = read_smn(offset | 0x5021C);
    uint32_t reg50220 = read_smn(offset | 0x50220);
    uint32_t reg50224 = read_smn(offset | 0x50224);
    uint32_t reg50228 = read_smn(offset | 0x50228);
    uint32_t reg50230 = read_smn(offset | 0x50230);
    uint32_t reg50234 = read_smn(offset | 0x50234);
    uint32_t reg50250 = read_smn(offset | 0x50250);
    uint32_t reg50254 = read_smn(offset | 0x50254);
    uint32_t reg50258 = read_smn(offset | 0x50258);
    uint32_t reg502A4 = read_smn(offset | 0x502A4);

    d->tcl     = bit_slice(reg50204, 5, 0);
    d->trcd_rd = bit_slice(reg50204, 21, 16);
    d->trcd_wr = bit_slice(reg50204, 29, 24);
    if (d->trcd_wr == 0) d->trcd_wr = d->trcd_rd;
    d->tras    = bit_slice(reg50204, 14, 8);
    d->trp     = bit_slice(reg50208, 21, 16);
    d->trc     = bit_slice(reg50208, 7, 0);

    d->trrds   = bit_slice(reg5020C, 4, 0);
    d->trrdl   = bit_slice(reg5020C, 12, 8);
    d->tfaw    = bit_slice(reg50210, 7, 0);
    d->rtp     = bit_slice(reg5020C, 28, 24);
    d->wtrs    = bit_slice(reg50214, 12, 8);
    d->wtrl    = bit_slice(reg50214, 22, 16);
    d->tcwl    = bit_slice(reg50214, 5, 0);
    d->twr     = bit_slice(reg50218, 7, 0);
    if (d->twr == 0) d->twr = d->wtrs;

    d->trc_page = bit_slice(reg5021C, 31, 20);

    d->rdrd_scl = bit_slice(reg50220, 29, 24);
    d->rdrd_sc  = bit_slice(reg50220, 19, 16);
    d->rdrd_sd  = bit_slice(reg50220, 11, 8);
    d->rdrd_dd  = bit_slice(reg50220, 3, 0);

    d->wrwr_scl = bit_slice(reg50224, 29, 24);
    d->wrwr_sc  = bit_slice(reg50224, 19, 16);
    d->wrwr_sd  = bit_slice(reg50224, 11, 8);
    d->wrwr_dd  = bit_slice(reg50224, 3, 0);

    d->rdwr = bit_slice(reg50228, 13, 8);
    d->wrrd = bit_slice(reg50228, 3, 0);
    d->refi = bit_slice(reg50230, 15, 0);

    d->mod_pda = bit_slice(reg50234, 29, 24);
    d->mrd_pda = bit_slice(reg50234, 21, 16);
    d->mod     = bit_slice(reg50234, 13, 8);
    d->mrd     = bit_slice(reg50234, 5, 0);

    d->stag    = bit_slice(reg50250, 26, 16);
    d->stag_sb = bit_slice(reg50250, 8, 0);

    d->cke = bit_slice(reg50254, 28, 24);
    d->xp  = bit_slice(reg50254, 5, 0);

    d->phy_wrd = bit_slice(reg50258, 26, 24);
    d->phy_rdl = bit_slice(reg50258, 23, 16);
    d->phy_wrl = bit_slice(reg50258, 15, 8);

    d->wrpre = bit_slice(reg502A4, 10, 8);
    d->rdpre = bit_slice(reg502A4, 2, 0);
}

static void read_ddr5_timings(dram_timings_t *d)
{
    const uint32_t offset = 0; /* UMC0 */

    /* Ratio -> frequency */
    uint32_t ratio_reg = read_smn(offset | 0x50200);
    float ratio = bit_slice(ratio_reg, 15, 0) / 100.0f;
    float mem_freq = ratio * 200.0f;
    d->frequency_hint_mhz = mem_freq;

    /* GDM, Cmd2T, PowerDown */
    d->gdm_enabled = bit_slice(ratio_reg, 18, 18) == 1;
    uint32_t cmd2t_bit = bit_slice(ratio_reg, 17, 17);
    snprintf(d->cmd2t, sizeof(d->cmd2t), "%s", cmd2t_bit ? "2T" : "1T");
    uint32_t refresh_reg = read_smn(offset | 0x5012C);
    d->power_down_enabled = bit_slice(refresh_reg, 28, 28) == 1;

    read_common_timings(offset, d);

    /* RFC - DDR5: choose first non-default from 4 registers */
    uint32_t trfc_regs[4];
    trfc_regs[0] = read_smn(offset | 0x50260);
    trfc_regs[1] = read_smn(offset | 0x50264);
    trfc_regs[2] = read_smn(offset | 0x50268);
    trfc_regs[3] = read_smn(offset | 0x5026C);
    uint32_t trfc_reg = 0;
    for (int i = 0; i < 4; i++) {
        if (trfc_regs[i] != 0x00C00138) { trfc_reg = trfc_regs[i]; break; }
    }
    if (trfc_reg) {
        d->rfc  = bit_slice(trfc_reg, 15, 0);
        d->rfc2 = bit_slice(trfc_reg, 31, 16);
    }

    /* RFCsb */
    uint32_t rfcsb_regs[4];
    rfcsb_regs[0] = bit_slice(read_smn(offset | 0x502C0), 10, 0);
    rfcsb_regs[1] = bit_slice(read_smn(offset | 0x502C4), 10, 0);
    rfcsb_regs[2] = bit_slice(read_smn(offset | 0x502C8), 10, 0);
    rfcsb_regs[3] = bit_slice(read_smn(offset | 0x502CC), 10, 0);
    for (int i = 0; i < 4; i++) {
        if (rfcsb_regs[i] != 0) { d->rfcsb = rfcsb_regs[i]; break; }
    }

    /* Nanosecond conversions */
    d->trefi_ns  = to_nanoseconds(d->refi, mem_freq);
    d->trfc_ns   = to_nanoseconds(d->rfc, mem_freq);
    d->trfc2_ns  = to_nanoseconds(d->rfc2, mem_freq);
    d->trfcsb_ns = to_nanoseconds(d->rfcsb, mem_freq);
}

static void read_ddr4_timings(dram_timings_t *d)
{
    const uint32_t offset = 0; /* UMC0 */

    read_common_timings(offset, d);

    /* RFC - DDR4: first non-default from 2 registers */
    uint32_t trfc0 = read_smn(offset | 0x50260);
    uint32_t trfc1 = read_smn(offset | 0x50264);
    uint32_t trfc_reg = (trfc0 != trfc1) ? ((trfc0 != 0x21060138) ? trfc0 : trfc1) : trfc0;
    if (trfc_reg) {
        d->rfc  = bit_slice(trfc_reg, 10, 0);
        d->rfc2 = bit_slice(trfc_reg, 21, 11);
    }

    d->cmd2t[0] = '\0';
    d->frequency_hint_mhz = 0;
}

void dram_read_timings(int codename_index, dram_timings_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (codename_index) {
    case 23: /* Granite Ridge DDR5 */
        read_ddr5_timings(out);
        break;
    case 4: case 9: case 10: case 12: case 18: case 19:
        /* Matisse, Summit, Pinnacle, Vermeer, Naples, Chagall (DDR4) */
        read_ddr4_timings(out);
        break;
    default:
        break;
    }
}
