#ifndef UI_H
#define UI_H

#include "types.h"
#include <gtk/gtk.h>

/* Holds all UI label widgets for live updates. */
typedef struct {
    GtkWidget *window;

    /* Header */
    GtkWidget *lbl_cpu_name;
    GtkWidget *lbl_codename;
    GtkWidget *lbl_smu_version;
    GtkWidget *lbl_pm_table_version;
    GtkWidget *lbl_board_info;
    GtkWidget *combo_modules;

    /* RAM tab — DIMM section */
    GtkWidget *lbl_speed, *lbl_mclk, *lbl_fclk, *lbl_uclk, *lbl_bclk;
    GtkWidget *lbl_gdm, *lbl_powerdown, *lbl_spd_temp;

    /* RAM tab — DIMM info */
    GtkWidget *lbl_capacity, *lbl_manufacturer, *lbl_part_number;
    GtkWidget *lbl_serial_number, *lbl_rank, *lbl_cmd2t;

    /* RAM tab — Voltages */
    GtkWidget *lbl_vsoc, *lbl_vddp, *lbl_vddg_ccd, *lbl_vddg_iod;
    GtkWidget *lbl_vdd_misc, *lbl_mem_vdd, *lbl_mem_vddq;
    GtkWidget *lbl_cpu_vddio, *lbl_mem_vpp, *lbl_vcore, *lbl_ppt;

    /* Section title labels (relabeled on Intel) */
    GtkWidget *lbl_section_voltages;
    GtkWidget *lbl_section_primary;
    GtkWidget *lbl_section_secondary;
    GtkWidget *lbl_section_tertiary;

    /* Voltage row labels that change on Intel */
    GtkWidget *row_lbl_vsoc;       /* "VSOC"      → "SA Voltage" */
    GtkWidget *row_lbl_vddp;       /* "CLDO VDDP" → "VDDQ TX"   */
    GtkWidget *row_lbl_vddg_ccd;   /* "VDDG CCD"  → "VccDD2"    */

    /* AMD-only voltage row labels (hidden on Intel) */
    GtkWidget *row_lbl_vddg_iod, *row_lbl_vdd_misc;
    GtkWidget *row_lbl_mem_vdd, *row_lbl_mem_vddq;
    GtkWidget *row_lbl_cpu_vddio, *row_lbl_mem_vpp;

    /* AMD-only timing row labels (hidden on Intel) */
    GtkWidget *row_lbl_rfc2;
    GtkWidget *row_lbl_rtp;
    GtkWidget *row_lbl_rdrd_scl, *row_lbl_wrwr_scl;
    GtkWidget *row_lbl_trc_page;
    GtkWidget *row_lbl_mod, *row_lbl_mod_pda;
    GtkWidget *row_lbl_mrd, *row_lbl_mrd_pda;
    GtkWidget *row_lbl_stag, *row_lbl_stag_sb;
    GtkWidget *row_lbl_phy_wrl, *row_lbl_phy_rdl, *row_lbl_phy_wrd;

    /* Intel-specific timing value labels */
    GtkWidget *row_lbl_trtl, *lbl_trtl;
    GtkWidget *row_lbl_intel_gear;
    GtkWidget *lbl_intel_gear;
    /* Intel-only extended timing rows (hidden on AMD) */
    GtkWidget *row_lbl_tppd,    *lbl_tppd;
    GtkWidget *row_lbl_refsbrd, *lbl_refsbrd;
    GtkWidget *row_lbl_refi_x9, *lbl_refi_x9;
    GtkWidget *row_lbl_txsr,    *lbl_txsr;
    GtkWidget *row_lbl_xp_dll,  *lbl_xp_dll;
    GtkWidget *row_lbl_rdpden,  *lbl_rdpden;
    GtkWidget *row_lbl_wrpden,  *lbl_wrpden;
    GtkWidget *row_lbl_prpden,  *lbl_prpden;
    GtkWidget *row_lbl_cpded,   *lbl_cpded;
    GtkWidget *row_lbl_tcsl,    *lbl_tcsl;
    GtkWidget *row_lbl_tcsh,    *lbl_tcsh;

    /* Vendor tracking for relabel-on-change */
    cpu_vendor_t last_vendor;

    /* RAM tab — Primary timings */
    GtkWidget *lbl_tcl, *lbl_trcd_rd, *lbl_trcd_wr, *lbl_trp, *lbl_tras, *lbl_trc;
    GtkWidget *lbl_trrds, *lbl_trrdl, *lbl_tfaw, *lbl_twr, *lbl_tcwl;
    GtkWidget *lbl_trfc_ns, *lbl_rfc, *lbl_rfc2, *lbl_rfcsb;

    /* RAM tab — Secondary timings */
    GtkWidget *lbl_rtp, *lbl_wtrs, *lbl_wtrl, *lbl_rdwr, *lbl_wrrd;
    GtkWidget *lbl_rdrd_sc, *lbl_rdrd_sd, *lbl_rdrd_dd;
    GtkWidget *lbl_wrwr_sc, *lbl_wrwr_sd, *lbl_wrwr_dd;
    GtkWidget *lbl_refi, *lbl_trefi_ns, *lbl_wrpre, *lbl_rdpre;

    /* RAM tab — Tertiary timings */
    GtkWidget *lbl_rdrd_scl, *lbl_wrwr_scl, *lbl_cke, *lbl_xp;
    GtkWidget *lbl_trc_page, *lbl_mod, *lbl_mod_pda, *lbl_mrd, *lbl_mrd_pda;
    GtkWidget *lbl_stag, *lbl_stag_sb, *lbl_phy_wrl, *lbl_phy_rdl, *lbl_phy_wrd;

    /* RAM tab — Footer */
    GtkWidget *lbl_footer_type;

    /* CPU tab */
    GtkWidget *lbl_vid_voltages;
    GtkWidget *lbl_core_temps;
    GtkWidget *lbl_tctl_tccd;
    GtkWidget *lbl_iod_hotspot;
    GtkWidget *lbl_fans;

    /* Data */
    system_summary_t summary;
    int selected_module;
    int modules_populated;
} app_widgets_t;

/* Build the UI and start the refresh timer. Returns the GtkApplication. */
GtkApplication *ui_create(int argc, char **argv);

#endif
