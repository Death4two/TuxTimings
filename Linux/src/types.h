#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_CORES       16
#define MAX_MODULES     4
#define MAX_FANS        8
#define STR_LEN         256
#define STR_SHORT       64

typedef enum { MEM_UNKNOWN, MEM_DDR4, MEM_DDR5, MEM_LPDDR4, MEM_LPDDR5 } mem_type_t;
typedef enum { RANK_SR, RANK_DR, RANK_QR } mem_rank_t;

typedef enum { CPU_VENDOR_UNKNOWN, CPU_VENDOR_AMD, CPU_VENDOR_INTEL } cpu_vendor_t;
typedef enum { INTEL_GEN_UNKNOWN, INTEL_GEN_12_14, INTEL_GEN_15 } intel_gen_t;

typedef struct {
    char bank_label[STR_LEN];
    char device_locator[STR_LEN];
    char manufacturer[STR_LEN];
    char part_number[STR_LEN];
    char serial_number[STR_LEN];
    uint64_t capacity_bytes;
    uint32_t clock_speed_mhz;
    mem_rank_t rank;
    /* Derived */
    char slot_label[24];   /* e.g. "A1", "Slot 0" */
    char slot_display[96]; /* e.g. "Module 1: A1 - 48.0 GiB" */
    char capacity_display[32]; /* e.g. "16.0 GiB" */
} memory_module_t;

typedef struct {
    float frequency;       /* MT/s */
    mem_type_t type;
    char total_capacity[32];
    char part_number[STR_LEN];
} memory_config_t;

typedef struct {
    char name[STR_LEN];
    char processor_name[STR_LEN];
    char codename[STR_SHORT];
    char smu_version[STR_SHORT];
    char pm_table_version[STR_SHORT];
    /* Vendor detection */
    cpu_vendor_t vendor;
    intel_gen_t  intel_gen;
    uint32_t     cpu_model;
    char         microcode_version[STR_SHORT];
} cpu_info_t;

typedef struct {
    char motherboard[STR_LEN];
    char bios_version[STR_LEN];
    char bios_date[STR_SHORT];
    char agesa_version[STR_LEN];
    char display_line[1024];
} board_info_t;

typedef struct {
    /* Power */
    float package_power_w; /* reserved — populated but not yet displayed */
    float ppt_w;
    float package_current_a; /* reserved — populated but not yet displayed */
    /* Voltages */
    float vcore, vsoc, vddp, vddg_ccd, vddg_iod, vdd_misc;
    float cpu_vddio, mem_vdd, mem_vddq, mem_vpp;
    float vid;
    /* Clocks */
    float core_clock_mhz, bclk_mhz; /* reserved */
    float fclk_mhz, uclk_mhz, mclk_mhz;
    float memory_clock_mhz; /* reserved */
    float core_clocks_ghz[MAX_CORES]; /* reserved */
    int   core_clocks_count; /* reserved */
    /* Temps */
    float cpu_temp_c;
    float core_temps_c[MAX_CORES];
    int   core_temps_count;
    float tdie_c;   bool has_tdie;
    float tctl_c;   bool has_tctl;
    float tccd1_c;  bool has_tccd1;
    float tccd2_c;  bool has_tccd2;
    float iod_hotspot_c; bool has_iod_hotspot;
    /* Per-core */
    float core_voltages[MAX_CORES];
    int   core_voltages_count;
    float core_usage_pct[MAX_CORES];
    int   core_usage_count;
    float core_freq_mhz[MAX_CORES];
    int   core_freq_count;
    /* SPD temps */
    float spd_temps_c[MAX_MODULES];
    int   spd_temps_count;
    /* Intel voltages (populated on Intel path only) */
    float intel_sa_voltage;
    float intel_vddq_tx;
    float intel_vccdd2;
} smu_metrics_t;

typedef struct {
    /* Primary */
    uint32_t tcl, trcd_rd, trcd_wr, trp, tras, trc;
    /* Secondary */
    uint32_t trrds, trrdl, tfaw, twr, tcwl;
    uint32_t rtp, wtrs, wtrl, rdwr, wrrd;
    uint32_t rdrd_scl, wrwr_scl;
    uint32_t rdrd_sc, rdrd_sd, rdrd_dd;
    uint32_t wrwr_sc, wrwr_sd, wrwr_dd;
    uint32_t refi, wrpre, rdpre;
    /* Tertiary */
    uint32_t trc_page, mod, mod_pda, mrd, mrd_pda;
    uint32_t stag, stag_sb, cke, xp;
    uint32_t phy_wrd, phy_wrl, phy_rdl;
    uint32_t phy_rdl_per_channel[MAX_MODULES];
    int      phy_rdl_channel_count;
    /* RFC */
    uint32_t rfc, rfc2, rfcsb;
    /* Nanoseconds */
    float trefi_ns, trfc_ns;
    float trfc2_ns, trfcsb_ns; /* reserved — computed but not displayed */
    /* Flags */
    bool gdm_enabled, power_down_enabled;
    char cmd2t[4];
    float frequency_hint_mhz;
    /* Intel-specific */
    uint32_t trtl;              /* Round-trip latency rank 0 (legacy single-rank display) */
    uint32_t trtl_per_rank[8];  /* Per-rank RTL values */
    int      trtl_rank_count;   /* Number of populated rank RTL values */
    uint32_t intel_gear;        /* 1, 2, or 4 */
    int      intel_channel_count;
    /* Intel power-down / exit timings (from TC_PWDEN 0xE050) */
    uint32_t xp_dll;            /* tXPDLL — power-down exit with DLL lock */
    uint32_t rdpden;            /* tRDPDEN — read to power-down entry */
    uint32_t wrpden;            /* tWRPDEN — write to power-down entry */
    uint32_t prpden;            /* tPRPDEN — precharge to power-down entry */
    uint32_t tcsl;              /* tCSL — chip select low on PD exit */
    uint32_t tcsh;              /* tCSH — chip select high on PD exit */
    uint32_t cpded;             /* tCPDED — command/power-down exit delay (GS_CFG) */
    /* Intel refresh extras */
    uint32_t refi_x9;           /* tREFIx9 — max refresh interval (TC_RFP) */
    uint32_t txsr;              /* tXSR — self-refresh exit (TC_SREXITTP) */
    uint32_t refsbrd;           /* tREFSBRD — refresh-to-bank-read (TC_ACT) */
    uint32_t tppd;              /* tPPD — PRE to PRE delay (TC_PRE) */
} dram_timings_t;

typedef struct {
    char label[16];
    int rpm;
} fan_reading_t;

typedef struct {
    cpu_info_t cpu;
    memory_config_t memory;
    board_info_t board;
    memory_module_t modules[MAX_MODULES];
    int module_count;
    smu_metrics_t metrics;
    dram_timings_t dram;
    fan_reading_t fans[MAX_FANS];
    int fan_count;
} system_summary_t;

#endif /* TYPES_H */
