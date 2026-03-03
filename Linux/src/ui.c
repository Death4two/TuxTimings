#include "ui.h"
#include "backend.h"
#include <stdio.h>
#include <string.h>
#include <locale.h>

/* ── CSS theme (GitHub dark) ────────────────────────────────────────── */

#define APP_VERSION "v1.0.2"

/* float→double promotion in variadic snprintf calls is harmless here */
#pragma GCC diagnostic ignored "-Wdouble-promotion"

static const char *css_data =
    "window { background-color: #0D1117; }\n"
    ".header-title { color: #E6EDF3; font-size: 16px; font-weight: bold; }\n"
    ".header-muted { color: #8B949E; font-size: 12px; }\n"
    ".footer-muted { color: #8B949E; font-size: 11px; }\n"
    ".section-title { color: #C9D1D9; font-size: 13px; font-weight: bold; }\n"
    ".label { color: #8B949E; font-size: 12px; }\n"
    ".value-highlight { color: #3FB950; font-size: 12px; }\n"
    ".section-box { background-color: #161B22; border-radius: 6px; padding: 6px; }\n"
    "notebook { background: transparent; }\n"
    "notebook > header { background: transparent; border-bottom: 1px solid #30363D; }\n"
    "notebook > header > tabs > tab { color: #8B949E; background: transparent; padding: 6px 16px; }\n"
    "notebook > header > tabs > tab:checked { color: #E6EDF3; border-bottom: 2px solid #3FB950; }\n"
    "notebook > stack { background: transparent; }\n"
    "dropdown { background-color: #161B22; color: #E6EDF3; }\n"
    "dropdown > button { background-color: #161B22; color: #E6EDF3; border: 1px solid #30363D; }\n"
    "scrolledwindow { background: transparent; }\n";

static void load_css(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css_data);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ── Helpers ────────────────────────────────────────────────────────── */

static GtkWidget *make_label(const char *text, const char *css_class)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, css_class);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_hexpand(l, FALSE);
    return l;
}

static GtkWidget *make_value(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "value-highlight");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_hexpand(l, TRUE);
    return l;
}

static void grid_row(GtkWidget *grid, int row, const char *label_text,
                     GtkWidget **out_lbl, GtkWidget **out_val)
{
    GtkWidget *lbl = make_label(label_text, "label");
    *out_val = make_value("—");
    gtk_widget_set_hexpand(grid, TRUE);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), *out_val, 1, row, 1, 1);
    if (out_lbl) *out_lbl = lbl;
}

static void set_label_text(GtkWidget *label, const char *text)
{
    gtk_label_set_text(GTK_LABEL(label), text);
}

static void set_label_fmt(GtkWidget *label, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(label), buf);
}

static GtkWidget *make_section_box(void)
{
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(frame, "section-box");
    gtk_widget_set_hexpand(frame, TRUE);
    gtk_widget_set_vexpand(frame, TRUE);
    return frame;
}

/* ── Build RAM tab ──────────────────────────────────────────────────── */

static GtkWidget *build_ram_tab(app_widgets_t *w)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_widget_set_hexpand(vbox, TRUE);

    /* ── Top row: DIMM | DIMM info | Voltages ── */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(top, TRUE);
    gtk_box_set_homogeneous(GTK_BOX(top), TRUE);

    /* DIMM speeds */
    GtkWidget *dimm_box = make_section_box();
    {
        GtkWidget *title = make_label("DIMM", "section-title");
        gtk_box_append(GTK_BOX(dimm_box), title);

        GtkWidget *g = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(g), 4);
        gtk_grid_set_column_spacing(GTK_GRID(g), 8);
        int r = 0;
        grid_row(g, r++, "Speed:", NULL, &w->lbl_speed);
        grid_row(g, r++, "MCLK:", NULL, &w->lbl_mclk);
        grid_row(g, r++, "FCLK:", NULL, &w->lbl_fclk);
        grid_row(g, r++, "UCLK:", NULL, &w->lbl_uclk);
        grid_row(g, r++, "BCLK:", NULL, &w->lbl_bclk);
        gtk_box_append(GTK_BOX(dimm_box), g);

        /* GDM / PowerDown / Temp — horizontal: labels row then values row */
        GtkWidget *g2 = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(g2), 2);
        gtk_grid_set_column_spacing(GTK_GRID(g2), 12);
        gtk_widget_set_margin_top(g2, 4);

        GtkWidget *lbl_gdm_h   = make_label("GDM",       "label");
        GtkWidget *lbl_pd_h    = make_label("PowerDown", "label");
        GtkWidget *lbl_temp_h  = make_label("Temp",      "label");
        w->lbl_gdm       = make_value("—");
        w->lbl_powerdown = make_value("—");
        w->lbl_spd_temp  = make_value("—");

        gtk_grid_attach(GTK_GRID(g2), lbl_gdm_h,        0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(g2), lbl_pd_h,         1, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(g2), lbl_temp_h,       2, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(g2), w->lbl_gdm,       0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(g2), w->lbl_powerdown, 1, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(g2), w->lbl_spd_temp,  2, 1, 1, 1);
        gtk_box_append(GTK_BOX(dimm_box), g2);
    }
    gtk_box_append(GTK_BOX(top), dimm_box);

    /* DIMM info */
    GtkWidget *info_box = make_section_box();
    {
        GtkWidget *title = make_label("DIMM", "section-title");
        gtk_box_append(GTK_BOX(info_box), title);

        GtkWidget *g = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(g), 2);
        gtk_grid_set_column_spacing(GTK_GRID(g), 8);
        int r = 0;
        grid_row(g, r++, "Capacity:", NULL, &w->lbl_capacity);
        grid_row(g, r++, "Manufacturer:", NULL, &w->lbl_manufacturer);
        grid_row(g, r++, "Part Number:", NULL, &w->lbl_part_number);
        grid_row(g, r++, "Serial:", NULL, &w->lbl_serial_number);
        grid_row(g, r++, "Rank:", NULL, &w->lbl_rank);
        grid_row(g, r++, "Cmd2T:", NULL, &w->lbl_cmd2t);
        grid_row(g, r++, "Gear:",  &w->row_lbl_intel_gear, &w->lbl_intel_gear);
        gtk_box_append(GTK_BOX(info_box), g);
    }
    gtk_box_append(GTK_BOX(top), info_box);

    /* Voltages */
    GtkWidget *volt_box = make_section_box();
    {
        w->lbl_section_voltages = make_label("Voltages", "section-title");
        gtk_box_append(GTK_BOX(volt_box), w->lbl_section_voltages);

        GtkWidget *g = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(g), 2);
        gtk_grid_set_column_spacing(GTK_GRID(g), 8);
        int r = 0;
        grid_row(g, r++, "VSOC",      &w->row_lbl_vsoc,     &w->lbl_vsoc);
        grid_row(g, r++, "CLDO VDDP", &w->row_lbl_vddp,     &w->lbl_vddp);
        grid_row(g, r++, "VDDG CCD",  &w->row_lbl_vddg_ccd, &w->lbl_vddg_ccd);
        grid_row(g, r++, "VDDG IOD",  &w->row_lbl_vddg_iod,  &w->lbl_vddg_iod);
        grid_row(g, r++, "VDD MISC",  &w->row_lbl_vdd_misc,  &w->lbl_vdd_misc);
        grid_row(g, r++, "MEM VDD",   &w->row_lbl_mem_vdd,   &w->lbl_mem_vdd);
        grid_row(g, r++, "MEM VDDQ",  &w->row_lbl_mem_vddq,  &w->lbl_mem_vddq);
        grid_row(g, r++, "CPU VDDIO", &w->row_lbl_cpu_vddio, &w->lbl_cpu_vddio);
        grid_row(g, r++, "MEM VPP",   &w->row_lbl_mem_vpp,   &w->lbl_mem_vpp);
        grid_row(g, r++, "VCORE",     NULL,                  &w->lbl_vcore);
        grid_row(g, r++, "PPT",       NULL,                  &w->lbl_ppt);
        gtk_box_append(GTK_BOX(volt_box), g);
    }
    gtk_box_append(GTK_BOX(top), volt_box);
    gtk_box_append(GTK_BOX(vbox), top);

    /* ── Timing columns: Primary | Secondary | Tertiary ── */
    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(mid, TRUE);
    gtk_widget_set_vexpand(mid, TRUE);
    gtk_box_set_homogeneous(GTK_BOX(mid), TRUE);

    /* Primary */
    GtkWidget *prim_box = make_section_box();
    {
        w->lbl_section_primary = make_label("Primary Timings", "section-title");
        gtk_box_append(GTK_BOX(prim_box), w->lbl_section_primary);
        GtkWidget *g = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(g), 2);
        gtk_grid_set_column_spacing(GTK_GRID(g), 8);
        int r = 0;
        grid_row(g, r++, "tCL", NULL, &w->lbl_tcl);
        grid_row(g, r++, "tRCDRD", NULL, &w->lbl_trcd_rd);
        grid_row(g, r++, "tRCDWR", NULL, &w->lbl_trcd_wr);
        grid_row(g, r++, "tRP", NULL, &w->lbl_trp);
        grid_row(g, r++, "tRAS", NULL, &w->lbl_tras);
        grid_row(g, r++, "tRC", NULL, &w->lbl_trc);
        grid_row(g, r++, "tRRDS", NULL, &w->lbl_trrds);
        grid_row(g, r++, "tRRDL", NULL, &w->lbl_trrdl);
        grid_row(g, r++, "tFAW", NULL, &w->lbl_tfaw);
        grid_row(g, r++, "tWR", NULL, &w->lbl_twr);
        grid_row(g, r++, "tCWL", NULL, &w->lbl_tcwl);
        grid_row(g, r++, "tRFC (ns)", NULL, &w->lbl_trfc_ns);
        grid_row(g, r++, "tRFC", NULL, &w->lbl_rfc);
        grid_row(g, r++, "tRFC2",  &w->row_lbl_rfc2, &w->lbl_rfc2);
        grid_row(g, r++, "tRFCsb", NULL, &w->lbl_rfcsb);
        /* Intel-specific rows (hidden on AMD) */
        grid_row(g, r++, "tRTL",      &w->row_lbl_trtl,      &w->lbl_trtl);
        grid_row(g, r++, "tPPD",      &w->row_lbl_tppd,      &w->lbl_tppd);
        grid_row(g, r++, "tREFSBRD",  &w->row_lbl_refsbrd,   &w->lbl_refsbrd);
        gtk_box_append(GTK_BOX(prim_box), g);
    }
    gtk_box_append(GTK_BOX(mid), prim_box);

    /* Secondary */
    GtkWidget *sec_box = make_section_box();
    {
        w->lbl_section_secondary = make_label("Secondary Timings", "section-title");
        gtk_box_append(GTK_BOX(sec_box), w->lbl_section_secondary);
        GtkWidget *g = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(g), 2);
        gtk_grid_set_column_spacing(GTK_GRID(g), 8);
        int r = 0;
        grid_row(g, r++, "tRTP", &w->row_lbl_rtp, &w->lbl_rtp);
        grid_row(g, r++, "tWTRS", NULL, &w->lbl_wtrs);
        grid_row(g, r++, "tWTRL", NULL, &w->lbl_wtrl);
        grid_row(g, r++, "tRDWR", NULL, &w->lbl_rdwr);
        grid_row(g, r++, "tWRRD", NULL, &w->lbl_wrrd);
        grid_row(g, r++, "tRDRDSC", NULL, &w->lbl_rdrd_sc);
        grid_row(g, r++, "tRDRDSD", NULL, &w->lbl_rdrd_sd);
        grid_row(g, r++, "tRDRDDD", NULL, &w->lbl_rdrd_dd);
        grid_row(g, r++, "tWRWRSC", NULL, &w->lbl_wrwr_sc);
        grid_row(g, r++, "tWRWRSD", NULL, &w->lbl_wrwr_sd);
        grid_row(g, r++, "tWRWRDD", NULL, &w->lbl_wrwr_dd);
        grid_row(g, r++, "tREFI", NULL, &w->lbl_refi);
        grid_row(g, r++, "tREFI (ns)", NULL, &w->lbl_trefi_ns);
        grid_row(g, r++, "tWRPRE", NULL, &w->lbl_wrpre);
        grid_row(g, r++, "tRDPRE", NULL, &w->lbl_rdpre);
        /* Intel-only secondary rows (hidden on AMD) */
        grid_row(g, r++, "tREFIx9", &w->row_lbl_refi_x9, &w->lbl_refi_x9);
        grid_row(g, r++, "tXSR",    &w->row_lbl_txsr,    &w->lbl_txsr);
        gtk_box_append(GTK_BOX(sec_box), g);
    }
    gtk_box_append(GTK_BOX(mid), sec_box);

    /* Tertiary */
    GtkWidget *tert_box = make_section_box();
    {
        w->lbl_section_tertiary = make_label("Tertiary Timings", "section-title");
        gtk_box_append(GTK_BOX(tert_box), w->lbl_section_tertiary);
        GtkWidget *g = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(g), 2);
        gtk_grid_set_column_spacing(GTK_GRID(g), 8);
        int r = 0;
        grid_row(g, r++, "tRDRDSCL", &w->row_lbl_rdrd_scl, &w->lbl_rdrd_scl);
        grid_row(g, r++, "tWRWRSCL", &w->row_lbl_wrwr_scl, &w->lbl_wrwr_scl);
        grid_row(g, r++, "tCKE",     NULL,                  &w->lbl_cke);
        grid_row(g, r++, "tXP",      NULL,                  &w->lbl_xp);
        /* Intel-only power-down timing rows (hidden on AMD) */
        grid_row(g, r++, "tXPDLL",   &w->row_lbl_xp_dll,  &w->lbl_xp_dll);
        grid_row(g, r++, "tRDPDEN",  &w->row_lbl_rdpden,  &w->lbl_rdpden);
        grid_row(g, r++, "tWRPDEN",  &w->row_lbl_wrpden,  &w->lbl_wrpden);
        grid_row(g, r++, "tPRPDEN",  &w->row_lbl_prpden,  &w->lbl_prpden);
        grid_row(g, r++, "tCPDED",   &w->row_lbl_cpded,   &w->lbl_cpded);
        grid_row(g, r++, "tCSL",     &w->row_lbl_tcsl,    &w->lbl_tcsl);
        grid_row(g, r++, "tCSH",     &w->row_lbl_tcsh,    &w->lbl_tcsh);
        grid_row(g, r++, "tTRCPAGE", &w->row_lbl_trc_page,  &w->lbl_trc_page);
        grid_row(g, r++, "tMOD",     &w->row_lbl_mod,        &w->lbl_mod);
        grid_row(g, r++, "tMODPDA",  &w->row_lbl_mod_pda,    &w->lbl_mod_pda);
        grid_row(g, r++, "tMRD",     &w->row_lbl_mrd,        &w->lbl_mrd);
        grid_row(g, r++, "tMRDPDA",  &w->row_lbl_mrd_pda,    &w->lbl_mrd_pda);
        grid_row(g, r++, "tSTAG",    &w->row_lbl_stag,       &w->lbl_stag);
        grid_row(g, r++, "tSTAGsb",  &w->row_lbl_stag_sb,    &w->lbl_stag_sb);
        grid_row(g, r++, "tPHYWRL",  &w->row_lbl_phy_wrl,    &w->lbl_phy_wrl);
        grid_row(g, r++, "tPHYRDL",  &w->row_lbl_phy_rdl,    &w->lbl_phy_rdl);
        grid_row(g, r++, "tPHYWRD",  &w->row_lbl_phy_wrd,    &w->lbl_phy_wrd);
        gtk_box_append(GTK_BOX(tert_box), g);
    }
    gtk_box_append(GTK_BOX(mid), tert_box);
    gtk_box_append(GTK_BOX(vbox), mid);

    /* Footer */
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *footer_text = make_label("DRAM timings & MCLK/UCLK: SMN. Voltages & FCLK: PM table.", "footer-muted");
    gtk_widget_set_hexpand(footer_text, TRUE);
    w->lbl_footer_type = make_label("DDR5", "value-highlight");
    gtk_box_append(GTK_BOX(footer), footer_text);
    gtk_box_append(GTK_BOX(footer), w->lbl_footer_type);
    gtk_widget_set_margin_top(footer, 8);
    gtk_box_append(GTK_BOX(vbox), footer);

    return vbox;
}

/* ── Build CPU tab ──────────────────────────────────────────────────── */

static GtkWidget *build_cpu_tab(app_widgets_t *w)
{
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(hbox, 8);
    gtk_box_set_homogeneous(GTK_BOX(hbox), TRUE);

    /* Left: VID & per-core voltage */
    GtkWidget *left = make_section_box();
    gtk_box_append(GTK_BOX(left), make_label("VID & per-core voltage", "section-title"));
    w->lbl_vid_voltages = make_value("—");
    gtk_label_set_wrap(GTK_LABEL(w->lbl_vid_voltages), TRUE);
    gtk_box_append(GTK_BOX(left), w->lbl_vid_voltages);
    gtk_box_append(GTK_BOX(hbox), left);

    /* Right: Temps & fans */
    GtkWidget *right = make_section_box();
    gtk_box_append(GTK_BOX(right), make_label("Temp & Fans", "section-title"));

    gtk_box_append(GTK_BOX(right), make_label("Core temps / load / freq:", "label"));
    w->lbl_core_temps = make_value("—");
    gtk_label_set_wrap(GTK_LABEL(w->lbl_core_temps), TRUE);
    gtk_box_append(GTK_BOX(right), w->lbl_core_temps);

    gtk_box_append(GTK_BOX(right), make_label("CCD1 / Die temp:", "label"));
    w->lbl_tctl_tccd = make_value("—");
    gtk_label_set_wrap(GTK_LABEL(w->lbl_tctl_tccd), TRUE);
    gtk_box_append(GTK_BOX(right), w->lbl_tctl_tccd);

    gtk_box_append(GTK_BOX(right), make_label("IOD Hotspot:", "label"));
    w->lbl_iod_hotspot = make_value("—");
    gtk_box_append(GTK_BOX(right), w->lbl_iod_hotspot);

    gtk_box_append(GTK_BOX(right), make_label("Fans:", "label"));
    w->lbl_fans = make_value("—");
    gtk_label_set_wrap(GTK_LABEL(w->lbl_fans), TRUE);
    gtk_box_append(GTK_BOX(right), w->lbl_fans);

    gtk_box_append(GTK_BOX(hbox), right);
    return hbox;
}

/* ── Refresh data → UI ──────────────────────────────────────────────── */

static void refresh_ui(app_widgets_t *w)
{
    backend_read_summary(&w->summary);
    const system_summary_t *s = &w->summary;
    const smu_metrics_t *m = &s->metrics;
    const dram_timings_t *d = &s->dram;

    cpu_vendor_t vendor = s->cpu.vendor;

    /* Relabel and show/hide vendor-specific rows when vendor changes */
    if (vendor != w->last_vendor) {
        gboolean amd = (vendor != CPU_VENDOR_INTEL);

        if (vendor == CPU_VENDOR_INTEL) {
            set_label_text(w->row_lbl_vsoc,     "SA Voltage");
            set_label_text(w->row_lbl_vddp,     "VDDQ TX");
            set_label_text(w->row_lbl_vddg_ccd, "VccDD2");
        } else {
            set_label_text(w->row_lbl_vsoc,     "VSOC");
            set_label_text(w->row_lbl_vddp,     "CLDO VDDP");
            set_label_text(w->row_lbl_vddg_ccd, "VDDG CCD");
        }

        /* AMD-only voltage rows */
        gtk_widget_set_visible(w->row_lbl_vddg_iod,  amd);
        gtk_widget_set_visible(w->lbl_vddg_iod,      amd);
        gtk_widget_set_visible(w->row_lbl_vdd_misc,  amd);
        gtk_widget_set_visible(w->lbl_vdd_misc,      amd);
        gtk_widget_set_visible(w->row_lbl_mem_vdd,   amd);
        gtk_widget_set_visible(w->lbl_mem_vdd,       amd);
        gtk_widget_set_visible(w->row_lbl_mem_vddq,  amd);
        gtk_widget_set_visible(w->lbl_mem_vddq,      amd);
        gtk_widget_set_visible(w->row_lbl_cpu_vddio, amd);
        gtk_widget_set_visible(w->lbl_cpu_vddio,     amd);
        gtk_widget_set_visible(w->row_lbl_mem_vpp,   amd);
        gtk_widget_set_visible(w->lbl_mem_vpp,       amd);

        /* AMD-only primary timing rows */
        gtk_widget_set_visible(w->row_lbl_rfc2, amd);
        gtk_widget_set_visible(w->lbl_rfc2,     amd);

        /* AMD-only secondary timing rows */
        gtk_widget_set_visible(w->row_lbl_rtp, amd);
        gtk_widget_set_visible(w->lbl_rtp,     amd);

        /* AMD-only tertiary timing rows */
        gtk_widget_set_visible(w->row_lbl_rdrd_scl, amd);
        gtk_widget_set_visible(w->lbl_rdrd_scl,     amd);
        gtk_widget_set_visible(w->row_lbl_wrwr_scl, amd);
        gtk_widget_set_visible(w->lbl_wrwr_scl,     amd);
        gtk_widget_set_visible(w->row_lbl_trc_page, amd);
        gtk_widget_set_visible(w->lbl_trc_page,     amd);
        gtk_widget_set_visible(w->row_lbl_mod,      amd);
        gtk_widget_set_visible(w->lbl_mod,           amd);
        gtk_widget_set_visible(w->row_lbl_mod_pda,  amd);
        gtk_widget_set_visible(w->lbl_mod_pda,       amd);
        gtk_widget_set_visible(w->row_lbl_mrd,      amd);
        gtk_widget_set_visible(w->lbl_mrd,           amd);
        gtk_widget_set_visible(w->row_lbl_mrd_pda,  amd);
        gtk_widget_set_visible(w->lbl_mrd_pda,       amd);
        gtk_widget_set_visible(w->row_lbl_stag,     amd);
        gtk_widget_set_visible(w->lbl_stag,          amd);
        gtk_widget_set_visible(w->row_lbl_stag_sb,  amd);
        gtk_widget_set_visible(w->lbl_stag_sb,       amd);
        gtk_widget_set_visible(w->row_lbl_phy_wrl,  amd);
        gtk_widget_set_visible(w->lbl_phy_wrl,       amd);
        gtk_widget_set_visible(w->row_lbl_phy_rdl,  amd);
        gtk_widget_set_visible(w->lbl_phy_rdl,       amd);
        gtk_widget_set_visible(w->row_lbl_phy_wrd,      amd);
        gtk_widget_set_visible(w->lbl_phy_wrd,           amd);

        /* Intel-only row in DIMM info section */
        gtk_widget_set_visible(w->row_lbl_intel_gear, !amd);
        gtk_widget_set_visible(w->lbl_intel_gear,     !amd);

        /* Intel-only timing rows */
        gtk_widget_set_visible(w->row_lbl_trtl,    !amd);
        gtk_widget_set_visible(w->lbl_trtl,         !amd);
        gtk_widget_set_visible(w->row_lbl_tppd,    !amd);
        gtk_widget_set_visible(w->lbl_tppd,         !amd);
        gtk_widget_set_visible(w->row_lbl_refsbrd, !amd);
        gtk_widget_set_visible(w->lbl_refsbrd,      !amd);
        gtk_widget_set_visible(w->row_lbl_refi_x9, !amd);
        gtk_widget_set_visible(w->lbl_refi_x9,      !amd);
        gtk_widget_set_visible(w->row_lbl_txsr,    !amd);
        gtk_widget_set_visible(w->lbl_txsr,         !amd);
        gtk_widget_set_visible(w->row_lbl_xp_dll,  !amd);
        gtk_widget_set_visible(w->lbl_xp_dll,       !amd);
        gtk_widget_set_visible(w->row_lbl_rdpden,  !amd);
        gtk_widget_set_visible(w->lbl_rdpden,       !amd);
        gtk_widget_set_visible(w->row_lbl_wrpden,  !amd);
        gtk_widget_set_visible(w->lbl_wrpden,       !amd);
        gtk_widget_set_visible(w->row_lbl_prpden,  !amd);
        gtk_widget_set_visible(w->lbl_prpden,       !amd);
        gtk_widget_set_visible(w->row_lbl_cpded,   !amd);
        gtk_widget_set_visible(w->lbl_cpded,        !amd);
        gtk_widget_set_visible(w->row_lbl_tcsl,    !amd);
        gtk_widget_set_visible(w->lbl_tcsl,         !amd);
        gtk_widget_set_visible(w->row_lbl_tcsh,    !amd);
        gtk_widget_set_visible(w->lbl_tcsh,         !amd);

        w->last_vendor = vendor;
    }

    /* Header */
    set_label_text(w->lbl_cpu_name, s->cpu.processor_name[0] ? s->cpu.processor_name : s->cpu.name);
    if (vendor == CPU_VENDOR_INTEL)
        set_label_fmt(w->lbl_codename, "%s  ·  Microcode %s",
                      s->cpu.codename, s->cpu.microcode_version);
    else
        set_label_fmt(w->lbl_codename, "%s  ·  SMU %s  ·  PM %s",
                      s->cpu.codename, s->cpu.smu_version, s->cpu.pm_table_version);
    set_label_text(w->lbl_board_info, s->board.display_line);

    /* Module dropdown — populate once */
    if (s->module_count > 0 && !w->modules_populated) {
        GtkStringList *model = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(w->combo_modules)));
        /* Remove placeholder */
        guint n = g_list_model_get_n_items(G_LIST_MODEL(model));
        for (guint i = n; i > 0; i--)
            gtk_string_list_remove(model, i - 1);
        for (int i = 0; i < s->module_count; i++)
            gtk_string_list_append(model, s->modules[i].slot_display);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(w->combo_modules), 0);
        w->selected_module = 0;
        w->modules_populated = 1;
    }

    int mi = w->selected_module;

    /* DIMM speeds */
    set_label_fmt(w->lbl_speed, "%.0f MT/s", s->memory.frequency);
    set_label_fmt(w->lbl_mclk, "%.0f MHz", m->mclk_mhz);
    set_label_fmt(w->lbl_fclk, "%.0f MHz", m->fclk_mhz);
    set_label_fmt(w->lbl_uclk, "%.0f MHz", m->uclk_mhz);
    set_label_fmt(w->lbl_bclk, "%.1f MHz", m->bclk_mhz);
    if (vendor == CPU_VENDOR_INTEL) {
        set_label_text(w->lbl_gdm,       "—");
        set_label_text(w->lbl_powerdown, "—");
    } else {
        set_label_text(w->lbl_gdm,       d->gdm_enabled ? "True" : "False");
        set_label_text(w->lbl_powerdown, d->power_down_enabled ? "True" : "False");
    }

    /* SPD temp */
    if (mi >= 0 && mi < m->spd_temps_count)
        set_label_fmt(w->lbl_spd_temp, "%.1f\u00B0C", m->spd_temps_c[mi]);
    else
        set_label_text(w->lbl_spd_temp, "—");

    /* DIMM info */
    if (mi >= 0 && mi < s->module_count) {
        const memory_module_t *mod = &s->modules[mi];
        set_label_text(w->lbl_capacity, mod->capacity_display);
        set_label_text(w->lbl_manufacturer, mod->manufacturer[0] ? mod->manufacturer : "—");
        set_label_text(w->lbl_part_number, mod->part_number[0] ? mod->part_number : "—");
        set_label_text(w->lbl_serial_number, mod->serial_number[0] ? mod->serial_number : "—");
        const char *rank_str = mod->rank == RANK_QR ? "QR" : mod->rank == RANK_DR ? "DR" : "SR";
        set_label_text(w->lbl_rank, rank_str);
    }
    set_label_text(w->lbl_cmd2t, d->cmd2t[0] ? d->cmd2t : "—");

    /* Voltages — Intel and AMD diverge for top 3 rows */
    if (vendor == CPU_VENDOR_INTEL) {
        if (m->intel_sa_voltage > 0)
            set_label_fmt(w->lbl_vsoc,    "%.4fV", m->intel_sa_voltage);
        else
            set_label_text(w->lbl_vsoc,   "N/A");
        if (m->intel_vddq_tx > 0)
            set_label_fmt(w->lbl_vddp,    "%.4fV", m->intel_vddq_tx);
        else
            set_label_text(w->lbl_vddp,   "N/A");
        if (m->intel_vccdd2 > 0)
            set_label_fmt(w->lbl_vddg_ccd, "%.4fV", m->intel_vccdd2);
        else
            set_label_text(w->lbl_vddg_ccd, "N/A");
        set_label_text(w->lbl_vddg_iod, "—");
        set_label_text(w->lbl_vdd_misc, "—");
        set_label_text(w->lbl_cpu_vddio, "—");
        set_label_text(w->lbl_vcore, "—");
    } else {
        set_label_fmt(w->lbl_vsoc,     "%.4fV", m->vsoc);
        set_label_fmt(w->lbl_vddp,     "%.4fV", m->vddp);
        set_label_fmt(w->lbl_vddg_ccd, "%.4fV", m->vddg_ccd);
        set_label_fmt(w->lbl_vddg_iod, "%.4fV", m->vddg_iod);
        set_label_fmt(w->lbl_vdd_misc, "%.4fV", m->vdd_misc);
        set_label_fmt(w->lbl_cpu_vddio, "%.4fV", m->cpu_vddio);
        set_label_fmt(w->lbl_vcore,    "%.4fV", m->vcore);
    }
    set_label_fmt(w->lbl_mem_vdd,  "%.4fV", m->mem_vdd);
    set_label_fmt(w->lbl_mem_vddq, "%.4fV", m->mem_vddq);
    set_label_fmt(w->lbl_mem_vpp,  "%.4fV", m->mem_vpp);
    set_label_fmt(w->lbl_ppt,      "%.1fW",  m->ppt_w);

    /* Primary timings */
    set_label_fmt(w->lbl_tcl, "%u", d->tcl);
    set_label_fmt(w->lbl_trcd_rd, "%u", d->trcd_rd);
    set_label_fmt(w->lbl_trcd_wr, "%u", d->trcd_wr);
    set_label_fmt(w->lbl_trp, "%u", d->trp);
    set_label_fmt(w->lbl_tras, "%u", d->tras);
    set_label_fmt(w->lbl_trc, "%u", d->trc);
    set_label_fmt(w->lbl_trrds, "%u", d->trrds);
    set_label_fmt(w->lbl_trrdl, "%u", d->trrdl);
    set_label_fmt(w->lbl_tfaw, "%u", d->tfaw);
    set_label_fmt(w->lbl_twr, "%u", d->twr);
    set_label_fmt(w->lbl_tcwl, "%u", d->tcwl);
    set_label_fmt(w->lbl_trfc_ns, "%.0f", d->trfc_ns);
    set_label_fmt(w->lbl_rfc, "%u", d->rfc);
    set_label_fmt(w->lbl_rfc2, "%u", d->rfc2);
    set_label_fmt(w->lbl_rfcsb, "%u", d->rfcsb);
    /* Intel-specific */
    if (vendor == CPU_VENDOR_INTEL) {
        /* Per-rank RTL: show as "R0/R1/R2/R3" */
        if (d->trtl_rank_count > 1) {
            char rtl_buf[64];
            int off = 0;
            for (int i = 0; i < d->trtl_rank_count && i < 8; i++) {
                if (i > 0) rtl_buf[off++] = '/';
                off += snprintf(rtl_buf + off, sizeof(rtl_buf) - off,
                                "%u", d->trtl_per_rank[i]);
            }
            set_label_text(w->lbl_trtl, rtl_buf);
        } else {
            set_label_fmt(w->lbl_trtl, "%u", d->trtl);
        }
        set_label_fmt(w->lbl_intel_gear, "%u", d->intel_gear);
        set_label_fmt(w->lbl_tppd,    "%u", d->tppd);
        set_label_fmt(w->lbl_refsbrd, "%u", d->refsbrd);
    }

    /* Secondary timings */
    set_label_fmt(w->lbl_rtp, "%u", d->rtp);
    set_label_fmt(w->lbl_wtrs, "%u", d->wtrs);
    set_label_fmt(w->lbl_wtrl, "%u", d->wtrl);
    set_label_fmt(w->lbl_rdwr, "%u", d->rdwr);
    set_label_fmt(w->lbl_wrrd, "%u", d->wrrd);
    set_label_fmt(w->lbl_rdrd_sc, "%u", d->rdrd_sc);
    set_label_fmt(w->lbl_rdrd_sd, "%u", d->rdrd_sd);
    set_label_fmt(w->lbl_rdrd_dd, "%u", d->rdrd_dd);
    set_label_fmt(w->lbl_wrwr_sc, "%u", d->wrwr_sc);
    set_label_fmt(w->lbl_wrwr_sd, "%u", d->wrwr_sd);
    set_label_fmt(w->lbl_wrwr_dd, "%u", d->wrwr_dd);
    set_label_fmt(w->lbl_refi, "%u", d->refi);
    set_label_fmt(w->lbl_trefi_ns, "%.0f", d->trefi_ns);
    set_label_fmt(w->lbl_wrpre, "%u", d->wrpre);
    set_label_fmt(w->lbl_rdpre, "%u", d->rdpre);
    if (vendor == CPU_VENDOR_INTEL) {
        set_label_fmt(w->lbl_refi_x9, "%u", d->refi_x9);
        set_label_fmt(w->lbl_txsr,    "%u", d->txsr);
    }

    /* Tertiary timings (AMD-only rows are hidden via visibility; always update value) */
    set_label_fmt(w->lbl_rdrd_scl, "%u", d->rdrd_scl);
    set_label_fmt(w->lbl_wrwr_scl, "%u", d->wrwr_scl);
    set_label_fmt(w->lbl_cke,      "%u", d->cke);
    set_label_fmt(w->lbl_xp,       "%u", d->xp);
    if (vendor == CPU_VENDOR_INTEL) {
        set_label_fmt(w->lbl_xp_dll,  "%u", d->xp_dll);
        set_label_fmt(w->lbl_rdpden,  "%u", d->rdpden);
        set_label_fmt(w->lbl_wrpden,  "%u", d->wrpden);
        set_label_fmt(w->lbl_prpden,  "%u", d->prpden);
        set_label_fmt(w->lbl_cpded,   "%u", d->cpded);
        set_label_fmt(w->lbl_tcsl,    "%u", d->tcsl);
        set_label_fmt(w->lbl_tcsh,    "%u", d->tcsh);
    }
    set_label_fmt(w->lbl_trc_page, "%u", d->trc_page);
    set_label_fmt(w->lbl_mod,      "%u", d->mod);
    set_label_fmt(w->lbl_mod_pda,  "%u", d->mod_pda);
    set_label_fmt(w->lbl_mrd,      "%u", d->mrd);
    set_label_fmt(w->lbl_mrd_pda,  "%u", d->mrd_pda);
    set_label_fmt(w->lbl_stag,     "%u", d->stag);
    set_label_fmt(w->lbl_stag_sb,  "%u", d->stag_sb);
    set_label_fmt(w->lbl_phy_wrl,  "%u", d->phy_wrl);
    /* PhyRdl: per-channel if available */
    if (mi >= 0 && mi < d->phy_rdl_channel_count)
        set_label_fmt(w->lbl_phy_rdl, "%u", d->phy_rdl_per_channel[mi]);
    else
        set_label_fmt(w->lbl_phy_rdl, "%u", d->phy_rdl);
    set_label_fmt(w->lbl_phy_wrd,  "%u", d->phy_wrd);

    /* Footer mem type */
    const char *mem_str = s->memory.type == MEM_DDR5 ? "DDR5" :
                          s->memory.type == MEM_DDR4 ? "DDR4" : "—";
    set_label_text(w->lbl_footer_type, mem_str);

    /* CPU tab — VID & voltages */
    {
        char buf[2048];
        int off = 0;
        if (m->vid > 0)
            off += snprintf(buf + off, sizeof(buf) - off, "VID: %.4f V\n", m->vid);
        for (int i = 0; i < m->core_voltages_count && i < MAX_CORES; i++) {
            if (m->core_voltages[i] > 0)
                off += snprintf(buf + off, sizeof(buf) - off, "C%d: %.4f V\n", i, m->core_voltages[i]);
        }
        if (off == 0) snprintf(buf, sizeof(buf), "—");
        set_label_text(w->lbl_vid_voltages, buf);
    }

    /* CPU tab — Core temps / load / freq */
    {
        char buf[4096];
        int off = 0;
        int count = m->core_temps_count;
        if (m->core_usage_count > count) count = m->core_usage_count;
        if (m->core_freq_count > count) count = m->core_freq_count;
        if (count > MAX_CORES) count = MAX_CORES;

        for (int i = 0; i < count; i++) {
            float temp = (i < m->core_temps_count) ? m->core_temps_c[i] : 0;
            float usage = (i < m->core_usage_count) ? m->core_usage_pct[i] : 0;
            float freq = (i < m->core_freq_count) ? m->core_freq_mhz[i] : 0;
            off += snprintf(buf + off, sizeof(buf) - off,
                            "C%d: %.1f°C  %.0f%%  %.0f MHz\n", i, temp, usage, freq);
        }
        if (off == 0) snprintf(buf, sizeof(buf), "—");
        set_label_text(w->lbl_core_temps, buf);
    }

    /* CPU tab — Tctl/Tccd */
    {
        char buf[512];
        int off = 0;
        if (m->has_tdie) off += snprintf(buf + off, sizeof(buf) - off, "Tdie: %.1f°C  ", m->tdie_c);
        if (m->has_tctl) off += snprintf(buf + off, sizeof(buf) - off, "Tctl: %.1f°C  ", m->tctl_c);
        if (m->has_tccd1) off += snprintf(buf + off, sizeof(buf) - off, "Tccd1: %.1f°C  ", m->tccd1_c);
        if (m->has_tccd2) off += snprintf(buf + off, sizeof(buf) - off, "Tccd2: %.1f°C  ", m->tccd2_c);
        if (off == 0) snprintf(buf, sizeof(buf), "—");
        set_label_text(w->lbl_tctl_tccd, buf);
    }

    /* IOD Hotspot */
    if (m->has_iod_hotspot)
        set_label_fmt(w->lbl_iod_hotspot, "%.1f°C", m->iod_hotspot_c);
    else
        set_label_text(w->lbl_iod_hotspot, "—");

    /* Fans */
    {
        char buf[1024];
        int off = 0;
        for (int i = 0; i < s->fan_count; i++)
            off += snprintf(buf + off, sizeof(buf) - off, "%s: %d RPM\n", s->fans[i].label, s->fans[i].rpm);
        if (off == 0) snprintf(buf, sizeof(buf), "—");
        set_label_text(w->lbl_fans, buf);
    }
}

/* Timer callback */
static gboolean on_refresh(gpointer user_data)
{
    setlocale(LC_NUMERIC, "C");
    app_widgets_t *w = (app_widgets_t *)user_data;
    refresh_ui(w);
    return G_SOURCE_CONTINUE;
}

/* Module dropdown selection changed */
static void on_module_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    app_widgets_t *w = (app_widgets_t *)user_data;
    w->selected_module = (int)gtk_drop_down_get_selected(dropdown);
}

/* ── App activate ───────────────────────────────────────────────────── */

static app_widgets_t s_widgets;

static void on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
    app_widgets_t *w = &s_widgets;
    memset(w, 0, sizeof(*w));
    w->selected_module = 0;

    /* GTK resets LC_NUMERIC; force C locale for dot decimal separators */
    setlocale(LC_NUMERIC, "C");

    load_css();

    /* Window */
    w->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(w->window), "TuxTimings");
    gtk_window_set_default_size(GTK_WINDOW(w->window), 360, 720);
    gtk_window_set_resizable(GTK_WINDOW(w->window), FALSE);

    /* Window icon: use icon theme name (works when installed to hicolor) */
    gtk_window_set_icon_name(GTK_WINDOW(w->window), "tuxtimings");

    /* Main layout */
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(main_box, 10);
    gtk_widget_set_margin_end(main_box, 10);
    gtk_widget_set_margin_top(main_box, 10);
    gtk_widget_set_margin_bottom(main_box, 14);
    gtk_window_set_child(GTK_WINDOW(w->window), main_box);

    /* Header */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *header_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    w->lbl_cpu_name = make_label("TuxTimings", "header-title");
    gtk_widget_set_hexpand(w->lbl_cpu_name, TRUE);
    gtk_box_append(GTK_BOX(header_top), w->lbl_cpu_name);

    /* Module dropdown */
    const char *placeholder[] = {"(detecting...)", NULL};
    GtkStringList *module_model = gtk_string_list_new(placeholder);
    w->combo_modules = gtk_drop_down_new(G_LIST_MODEL(module_model), NULL);
    gtk_widget_set_size_request(w->combo_modules, 200, -1);
    g_signal_connect(w->combo_modules, "notify::selected", G_CALLBACK(on_module_changed), w);
    gtk_box_append(GTK_BOX(header_top), w->combo_modules);
    gtk_box_append(GTK_BOX(header), header_top);

    w->lbl_codename = make_label("", "header-muted");
    w->lbl_smu_version = make_label("", "footer-muted");
    w->lbl_pm_table_version = make_label("", "footer-muted");
    gtk_box_append(GTK_BOX(header), w->lbl_codename);

    /* Board info row: board string left, version right */
    GtkWidget *board_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    w->lbl_board_info = make_label("", "footer-muted");
    gtk_widget_set_hexpand(w->lbl_board_info, TRUE);
    GtkWidget *lbl_version = make_label(APP_VERSION, "footer-muted");
    gtk_label_set_xalign(GTK_LABEL(lbl_version), 1.0f);
    gtk_box_append(GTK_BOX(board_row), w->lbl_board_info);
    gtk_box_append(GTK_BOX(board_row), lbl_version);
    gtk_box_append(GTK_BOX(header), board_row);
    gtk_box_append(GTK_BOX(main_box), header);

    /* Notebook (tabs) */
    GtkWidget *notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);

    GtkWidget *ram_tab = build_ram_tab(w);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), ram_tab, gtk_label_new("RAM"));

    GtkWidget *cpu_tab = build_cpu_tab(w);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), cpu_tab, gtk_label_new("CPU"));

    gtk_box_append(GTK_BOX(main_box), notebook);

    /* Initial data load */
    refresh_ui(w);

    /* 1-second refresh timer */
    g_timeout_add(1000, on_refresh, w);

    gtk_window_present(GTK_WINDOW(w->window));
}

/* ── Public API ─────────────────────────────────────────────────────── */

GtkApplication *ui_create(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    GtkApplication *app = gtk_application_new("com.tuxtimings.app", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    return app;
}
