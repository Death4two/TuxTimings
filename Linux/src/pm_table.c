#include "pm_table.h"
#include <string.h>
#include <math.h>

/* Named PM table index entry */
typedef struct { int index; int field_offset; } pm_entry_t;

/* Field offsets into smu_metrics_t for named entries */
enum {
    F_FCLK, F_UCLK, F_MCLK, F_VSOC, F_VDDP, F_VDDG_IOD, F_VDDG_CCD,
    F_VDD_MISC, F_VCORE, F_IOD_HOTSPOT
};

typedef struct {
    pm_entry_t named[12];
    int named_count;
    int vid_idx, ppt_idx, socket_power_idx;
    int core_voltage_start, core_temp_start;
    int max_cores;
} pm_family_map_t;

/* Granite Ridge (default/fallback) */
static const pm_family_map_t GRANITE_RIDGE = {
    .named = {
        {11, F_IOD_HOTSPOT}, {58, F_VDD_MISC}, {71, F_FCLK}, {75, F_UCLK},
        {79, F_MCLK}, {83, F_VSOC}, {259, F_VDDG_IOD}, {261, F_VDDG_CCD},
        {269, F_VDDP}, {271, F_VCORE}
    },
    .named_count = 10,
    .vid_idx = 275, .ppt_idx = 3, .socket_power_idx = 29,
    .core_voltage_start = 309, .core_temp_start = 317, .max_cores = 8
};

/* Vermeer 0x380804 (5900X/5950X 16-core, older BIOS) */
static const pm_family_map_t VERMEER_380804 = {
    .named = {
        {11, F_IOD_HOTSPOT}, {48, F_FCLK}, {50, F_UCLK}, {51, F_MCLK},
        {44, F_VSOC}, {137, F_VDDP}, {138, F_VDDG_IOD}, {139, F_VDDG_CCD},
        {40, F_VCORE}
    },
    .named_count = 9,
    .vid_idx = 10, .ppt_idx = 1, .socket_power_idx = 29,
    .core_voltage_start = 185, .core_temp_start = 201, .max_cores = 16
};

/* Vermeer 0x380805 (5900X/5950X 16-core, newer BIOS) */
static const pm_family_map_t VERMEER_380805 = {
    .named = {
        {11, F_IOD_HOTSPOT}, {48, F_FCLK}, {50, F_UCLK}, {51, F_MCLK},
        {44, F_VSOC}, {137, F_VDDP}, {138, F_VDDG_IOD}, {139, F_VDDG_CCD},
        {39, F_VCORE}
    },
    .named_count = 9,
    .vid_idx = 10, .ppt_idx = 1, .socket_power_idx = 29,
    .core_voltage_start = 188, .core_temp_start = 204, .max_cores = 16
};

/* Vermeer 0x380904 (5600X 8-core, older BIOS) */
static const pm_family_map_t VERMEER_380904 = {
    .named = {
        {11, F_IOD_HOTSPOT}, {48, F_FCLK}, {50, F_UCLK}, {51, F_MCLK},
        {44, F_VSOC}, {137, F_VDDP}, {138, F_VDDG_IOD}, {139, F_VDDG_CCD},
        {40, F_VCORE}
    },
    .named_count = 9,
    .vid_idx = 10, .ppt_idx = 1, .socket_power_idx = 29,
    .core_voltage_start = 177, .core_temp_start = 185, .max_cores = 8
};

/* Vermeer 0x380905 (5600X 8-core, newer BIOS) */
static const pm_family_map_t VERMEER_380905 = {
    .named = {
        {11, F_IOD_HOTSPOT}, {48, F_FCLK}, {50, F_UCLK}, {51, F_MCLK},
        {44, F_VSOC}, {137, F_VDDP}, {138, F_VDDG_IOD}, {139, F_VDDG_CCD},
        {39, F_VCORE}
    },
    .named_count = 9,
    .vid_idx = 10, .ppt_idx = 1, .socket_power_idx = 29,
    .core_voltage_start = 180, .core_temp_start = 188, .max_cores = 8
};

/* Cezanne 0x400005 (5700G APU) */
static const pm_family_map_t CEZANNE_400005 = {
    .named = {
        {29, F_IOD_HOTSPOT}, {409, F_FCLK}, {410, F_UCLK}, {411, F_MCLK},
        {102, F_VSOC}, {565, F_VDDP}, {98, F_VCORE}
    },
    .named_count = 7,
    .vid_idx = 28, .ppt_idx = 5, .socket_power_idx = 38,
    .core_voltage_start = 208, .core_temp_start = 216, .max_cores = 8
};

/* Matisse 0x240903 (3700X/3800X 8-core) */
static const pm_family_map_t MATISSE_240903 = {
    .named = {
        {11, F_IOD_HOTSPOT}, {48, F_FCLK}, {50, F_UCLK}, {51, F_MCLK},
        {44, F_VSOC}, {125, F_VDDP}, {126, F_VDDG_IOD}, {39, F_VCORE}
    },
    .named_count = 8,
    .vid_idx = 10, .ppt_idx = 1, .socket_power_idx = 29,
    .core_voltage_start = 155, .core_temp_start = 163, .max_cores = 8
};

/* Matisse 0x240803 (3950X 16-core) */
static const pm_family_map_t MATISSE_240803 = {
    .named = {
        {11, F_IOD_HOTSPOT}, {48, F_FCLK}, {50, F_UCLK}, {51, F_MCLK},
        {44, F_VSOC}, {125, F_VDDP}, {126, F_VDDG_IOD}, {40, F_VCORE}
    },
    .named_count = 8,
    .vid_idx = 10, .ppt_idx = 1, .socket_power_idx = 29,
    .core_voltage_start = 163, .core_temp_start = 179, .max_cores = 16
};

/* Renoir 0x370003 (4800U APU) */
static const pm_family_map_t RENOIR_370003 = {
    .named = {
        {29, F_IOD_HOTSPOT}, {371, F_FCLK}, {372, F_UCLK}, {373, F_MCLK},
        {101, F_VSOC}, {527, F_VDDP}, {97, F_VCORE}
    },
    .named_count = 7,
    .vid_idx = 28, .ppt_idx = 5, .socket_power_idx = 38,
    .core_voltage_start = 200, .core_temp_start = 208, .max_cores = 8
};

/* Renoir 0x370005 (Renoir v2 APU) */
static const pm_family_map_t RENOIR_370005 = {
    .named = {
        {29, F_IOD_HOTSPOT}, {378, F_FCLK}, {379, F_UCLK}, {380, F_MCLK},
        {101, F_VSOC}, {534, F_VDDP}, {97, F_VCORE}
    },
    .named_count = 7,
    .vid_idx = 28, .ppt_idx = 5, .socket_power_idx = 38,
    .core_voltage_start = 207, .core_temp_start = 215, .max_cores = 8
};

/* Raven Ridge 0x1E0004 (2500U APU) */
static const pm_family_map_t RAVEN_1E0004 = {
    .named = {
        {61, F_IOD_HOTSPOT}, {166, F_FCLK}, {167, F_UCLK}, {168, F_MCLK},
        {65, F_VSOC}, {60, F_VDDP}, {61, F_VCORE}
    },
    .named_count = 7,
    .vid_idx = 57, .ppt_idx = 5, .socket_power_idx = 38,
    .core_voltage_start = 104, .core_temp_start = 108, .max_cores = 4
};

static const pm_family_map_t *get_family_map(uint32_t version)
{
    switch (version) {
    case 0x380804: return &VERMEER_380804;
    case 0x380805: return &VERMEER_380805;
    case 0x380904: return &VERMEER_380904;
    case 0x380905: return &VERMEER_380905;
    case 0x400005: return &CEZANNE_400005;
    case 0x240903: return &MATISSE_240903;
    case 0x240803: return &MATISSE_240803;
    case 0x370003: return &RENOIR_370003;
    case 0x370005: return &RENOIR_370005;
    case 0x1E0004: return &RAVEN_1E0004;
    default:       return &GRANITE_RIDGE;
    }
}

static inline float safe_get(const float *t, int count, int idx)
{
    return (idx >= 0 && idx < count) ? t[idx] : 0.0f;
}

static void apply_named(const pm_family_map_t *map, const float *t, int count, smu_metrics_t *m)
{
    for (int i = 0; i < map->named_count; i++) {
        float v = safe_get(t, count, map->named[i].index);
        switch (map->named[i].field_offset) {
        case F_FCLK:        m->fclk_mhz = v; break;
        case F_UCLK:        m->uclk_mhz = v; break;
        case F_MCLK:        m->mclk_mhz = v; break;
        case F_VSOC:        m->vsoc = v; break;
        case F_VDDP:        m->vddp = v; break;
        case F_VDDG_IOD:    m->vddg_iod = v; break;
        case F_VDDG_CCD:    m->vddg_ccd = v; break;
        case F_VDD_MISC:    m->vdd_misc = v; break;
        case F_VCORE:       m->vcore = v; break;
        case F_IOD_HOTSPOT: {
            if (v >= 1.0f && v <= 150.0f) {
                m->iod_hotspot_c = v;
                m->has_iod_hotspot = true;
            }
            break;
        }
        }
    }
}

/* Granite Ridge specific: byte-offset based reading (ZenStates-Core style) */
static void read_granite_ridge_offsets(const float *t, int count, smu_metrics_t *m)
{
    /* Byte offsets -> float index = offset / 4 */
    m->fclk_mhz  = safe_get(t, count, 0x11C / 4);
    m->uclk_mhz  = safe_get(t, count, 0x12C / 4);
    m->mclk_mhz  = safe_get(t, count, 0x13C / 4);
    m->vsoc       = safe_get(t, count, 0x14C / 4);
    m->vddp       = safe_get(t, count, 0x434 / 4);
    m->vddg_iod   = safe_get(t, count, 0x40C / 4);
    m->vddg_ccd   = safe_get(t, count, 0x414 / 4);
    m->vdd_misc   = safe_get(t, count, 0xE8 / 4);
    m->vcore      = safe_get(t, count, 0x43C / 4);
}

/* Try plausible PM table indices for power */
static float try_plausible_power(const float *t, int count)
{
    int candidates[] = {29, 1, 13, 38, 5, 220, 187, 42, 0};
    for (int i = 0; i < 9; i++) {
        float v = safe_get(t, count, candidates[i]);
        if (v >= 0.5f && v <= 400.0f) return v;
    }
    return 0.0f;
}

static float try_plausible_current(const float *t, int count)
{
    int candidates[] = {41, 46, 3, 10, 11, 4};
    for (int i = 0; i < 6; i++) {
        float v = safe_get(t, count, candidates[i]);
        if (v >= 0.5f && v <= 200.0f) return v;
    }
    return 0.0f;
}

static float try_plausible_temp(const float *t, int count)
{
    int candidates[] = {1, 448, 449};
    for (int i = 0; i < 3; i++) {
        float v = safe_get(t, count, candidates[i]);
        if (v >= 1.0f && v <= 150.0f) return v;
    }
    return 0.0f;
}

/* Read known PM table indices for PPT, core temps, core clocks, VID, core voltages */
static void read_known_indices(const float *t, int count, smu_metrics_t *m)
{
    /* PPT: try several candidates */
    int ppt_cands[] = {3, 1, 13, 29, 5, 38};
    for (int i = 0; i < 6; i++) {
        float v = safe_get(t, count, ppt_cands[i]);
        if (v >= 0.5f && v <= 400.0f) { m->ppt_w = v; break; }
    }

    /* Core temps (indices 317-324) */
    if (count > 324) {
        m->core_temps_count = 8;
        for (int i = 0; i < 8; i++)
            m->core_temps_c[i] = t[317 + i];
    }

    /* Tdie (indices 448-449) */
    if (count > 449) {
        float a = t[448], b = t[449];
        if (a >= 1.0f && a <= 150.0f) { m->tdie_c = a; m->has_tdie = true; }
        else if (b >= 1.0f && b <= 150.0f) { m->tdie_c = b; m->has_tdie = true; }
        else if (a > 0 && b > 0) { m->tdie_c = (a + b) * 0.5f; m->has_tdie = true; }
    }

    /* Core clocks GHz (indices 325-340) */
    if (count > 340) {
        m->core_clocks_count = 16;
        for (int i = 0; i < 16; i++)
            m->core_clocks_ghz[i] = t[325 + i];
    }

    /* VID (index 275) */
    if (count > 275)
        m->vid = t[275];

    /* Core voltages (indices 309-316) */
    if (count > 316) {
        m->core_voltages_count = 8;
        for (int i = 0; i < 8; i++)
            m->core_voltages[i] = t[309 + i];
    }

    /* IOD hotspot (index 11) */
    if (count > 11) {
        float v = t[11];
        if (v >= 1.0f && v <= 150.0f) {
            m->iod_hotspot_c = v;
            m->has_iod_hotspot = true;
        }
    }
}

void pm_table_read(uint32_t version, const float *table, int count,
                   int codename_index, smu_metrics_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!table || count < 4) return;

    if (codename_index == 23) {
        /* Granite Ridge: use byte-offset based reading */
        read_granite_ridge_offsets(table, count, out);
        read_known_indices(table, count, out);

        out->package_power_w = try_plausible_power(table, count);
        out->package_current_a = try_plausible_current(table, count);

        /* Core clock: use max from per-core clocks if available */
        if (out->core_clocks_count > 0) {
            float max_ghz = 0;
            for (int i = 0; i < out->core_clocks_count; i++)
                if (out->core_clocks_ghz[i] > max_ghz) max_ghz = out->core_clocks_ghz[i];
            if (max_ghz >= 0.5f && max_ghz <= 6.5f)
                out->core_clock_mhz = max_ghz * 1000.0f;
        }

        if (out->has_tdie && out->tdie_c > 0)
            out->cpu_temp_c = out->tdie_c;
        else
            out->cpu_temp_c = try_plausible_temp(table, count);
    } else {
        /* Generic: use version-based family map for named entries,
         * plus fallback heuristics for power/temp/clock */
        const pm_family_map_t *map = get_family_map(version);
        apply_named(map, table, count, out);

        /* Per-family core temps and voltages */
        int nc = map->max_cores;
        if (nc > MAX_CORES) nc = MAX_CORES;
        if (map->core_temp_start + nc <= count) {
            out->core_temps_count = nc;
            for (int i = 0; i < nc; i++)
                out->core_temps_c[i] = table[map->core_temp_start + i];
        }
        if (map->core_voltage_start + nc <= count) {
            out->core_voltages_count = nc;
            for (int i = 0; i < nc; i++)
                out->core_voltages[i] = table[map->core_voltage_start + i];
        }

        float v = safe_get(table, count, map->vid_idx);
        if (v > 0) out->vid = v;

        float ppt_v = safe_get(table, count, map->ppt_idx);
        if (ppt_v >= 0.5f && ppt_v <= 400.0f) out->ppt_w = ppt_v;

        float sp = safe_get(table, count, map->socket_power_idx);
        if (sp >= 0.5f && sp <= 400.0f) out->package_power_w = sp;
        else out->package_power_w = try_plausible_power(table, count);

        out->package_current_a = try_plausible_current(table, count);
        out->cpu_temp_c = try_plausible_temp(table, count);

        /* IOD hotspot from known indices */
        read_known_indices(table, count, out);
    }
}
