#include "backend.h"
#include "pm_table.h"
#include "dram.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define SMU_PATH "/sys/kernel/ryzen_smu_drv"

/* ── Cached static data ─────────────────────────────────────────────── */
static int  s_cached_static = 0;
static char s_processor_name[STR_LEN];
static char s_board_product[STR_LEN];
static char s_bios_version[STR_LEN];
static char s_bios_date[STR_SHORT];
static char s_agesa_version[STR_LEN];
static memory_module_t s_modules[MAX_MODULES];
static int  s_module_count;

/* Previous /proc/stat per-logical-cpu times for usage delta */
#define MAX_LOGICAL_CPUS 256
static uint64_t s_prev_idle[MAX_LOGICAL_CPUS];
static uint64_t s_prev_total[MAX_LOGICAL_CPUS];
static int      s_prev_valid[MAX_LOGICAL_CPUS];

/* ── Helpers ────────────────────────────────────────────────────────── */

static int read_smu_string(const char *name, char *buf, size_t sz)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", SMU_PATH, name);
    return read_file_string(path, buf, sz);
}

static uint32_t read_smu_uint32(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", SMU_PATH, name);
    uint8_t buf[4];
    if (read_file_bytes(path, buf, 4) < 4) return 0;
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static int read_codename_index(void)
{
    char buf[32];
    if (!read_smu_string("codename", buf, sizeof(buf))) return -1;
    return atoi(buf);
}

static const char *map_codename(int idx)
{
    switch (idx) {
    case 1:  return "Colfax";
    case 2:  return "Renoir";
    case 3:  return "Picasso";
    case 4:  return "Matisse";
    case 5:  return "Threadripper";
    case 6:  return "Castle Peak";
    case 7:  return "Raven Ridge";
    case 8:  return "Raven Ridge 2";
    case 9:  return "Summit Ridge";
    case 10: return "Pinnacle Ridge";
    case 11: return "Rembrandt";
    case 12: return "Vermeer";
    case 13: return "Vangogh";
    case 14: return "Cezanne";
    case 15: return "Milan";
    case 16: return "Dali";
    case 17: return "Luciene";
    case 18: return "Naples";
    case 19: return "Chagall";
    case 20: return "Raphael";
    case 21: return "Phoenix";
    case 22: return "Strix Point";
    case 23: return "Granite Ridge";
    case 24: return "Hawk Point";
    case 25: return "Storm Peak";
    default: return "Unknown";
    }
}

/* ── dmidecode parsing ──────────────────────────────────────────────── */

static void parse_dmidecode_processor(void)
{
    char *out = run_command("dmidecode -t processor");
    if (!out) return;

    int in_proc = 0;
    char *line = strtok(out, "\n");
    while (line) {
        if (strstr(line, "Processor Information"))
            in_proc = 1;
        else if (in_proc && strstr(line, "Version:")) {
            const char *colon = strchr(line, ':');
            if (colon) {
                colon++;
                while (*colon == ' ') colon++;
                snprintf(s_processor_name, STR_LEN, "%s", colon);
            }
            break;
        }
        line = strtok(NULL, "\n");
    }
    free(out);
}

static void parse_dmidecode_board(void)
{
    char *out = run_command("dmidecode -s baseboard-product-name");
    if (out) { snprintf(s_board_product, STR_LEN, "%s", out); free(out); }
    /* trim trailing newline */
    char *nl = strchr(s_board_product, '\n');
    if (nl) *nl = '\0';

    out = run_command("dmidecode -s bios-version");
    if (out) { snprintf(s_bios_version, STR_LEN, "%s", out); free(out); }
    nl = strchr(s_bios_version, '\n');
    if (nl) *nl = '\0';

    out = run_command("dmidecode -s bios-release-date");
    if (out) { snprintf(s_bios_date, STR_SHORT, "%s", out); free(out); }
    nl = strchr(s_bios_date, '\n');
    if (nl) *nl = '\0';
}

static uint64_t parse_capacity(const char *val)
{
    if (!val) return 0;
    if (strstr(val, "No Module") || strstr(val, "Not Installed"))
        return 0;
    uint64_t size = 0;
    char unit[16] = {0};
    if (sscanf(val, "%lu %15s", &size, unit) < 2) return 0;
    /* upper-case unit */
    for (char *p = unit; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (strcmp(unit, "GB") == 0 || strcmp(unit, "GIB") == 0)
        return size * 1024ULL * 1024ULL * 1024ULL;
    if (strcmp(unit, "MB") == 0 || strcmp(unit, "MIB") == 0)
        return size * 1024ULL * 1024ULL;
    if (strcmp(unit, "KB") == 0 || strcmp(unit, "KIB") == 0)
        return size * 1024ULL;
    return size;
}

static void build_module_display(memory_module_t *m, int index)
{
    /* slot label from bank locator */
    int bank = -1;
    char channel = 0;
    if (sscanf(m->bank_label, "BANK %d", &bank) == 1 || sscanf(m->bank_label, "Bank %d", &bank) == 1) {
        char ch = 'A' + (char)(bank / 2);
        int num = (bank % 2) + 1;
        snprintf(m->slot_label, sizeof(m->slot_label), "%c%d", ch, num);
    } else if (sscanf(m->bank_label, "P0 CHANNEL %c", &channel) == 1 ||
               sscanf(m->bank_label, "P0 Channel %c", &channel) == 1) {
        /* e.g. "P0 CHANNEL A" + "DIMM 1" -> "A1" */
        channel = (char)toupper((unsigned char)channel);
        int dimm_num = 1;
        sscanf(m->device_locator, "DIMM %d", &dimm_num);
        snprintf(m->slot_label, sizeof(m->slot_label), "%c%d", channel, dimm_num);
    } else if (m->device_locator[0]) {
        snprintf(m->slot_label, sizeof(m->slot_label), "%.7s", m->device_locator);
    } else if (m->bank_label[0]) {
        snprintf(m->slot_label, sizeof(m->slot_label), "%.7s", m->bank_label);
    } else {
        snprintf(m->slot_label, sizeof(m->slot_label), "Slot %d", index);
    }

    double gib = (double)m->capacity_bytes / (1024.0 * 1024.0 * 1024.0);
    snprintf(m->capacity_display, sizeof(m->capacity_display), "%.1f GiB", gib);
    snprintf(m->slot_display, sizeof(m->slot_display), "Module %d: %s - %s", index + 1, m->slot_label, m->capacity_display);
}

static void parse_dmidecode_memory(void)
{
    char *out = run_command("dmidecode -t memory");
    if (!out) return;

    s_module_count = 0;
    memory_module_t cur;
    memset(&cur, 0, sizeof(cur));
    int in_device = 0;
    uint64_t cap = 0;

    char *line = strtok(out, "\n");
    while (line) {
        if (strstr(line, "Memory Device") && !strstr(line, "Mapped")) {
            /* save previous */
            if (in_device && cap > 0 && s_module_count < MAX_MODULES) {
                cur.capacity_bytes = cap;
                build_module_display(&cur, s_module_count);
                s_modules[s_module_count++] = cur;
            }
            memset(&cur, 0, sizeof(cur));
            cap = 0;
            in_device = 1;
            line = strtok(NULL, "\n");
            continue;
        }
        if (!in_device) { line = strtok(NULL, "\n"); continue; }

        /* trim leading whitespace */
        const char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        const char *colon = strchr(trimmed, ':');
        if (!colon) { line = strtok(NULL, "\n"); continue; }

        /* key/value */
        int klen = (int)(colon - trimmed);
        const char *val = colon + 1;
        while (*val == ' ') val++;

        if (klen == 4 && strncmp(trimmed, "Size", 4) == 0)
            cap = parse_capacity(val);
        else if (klen == 7 && strncmp(trimmed, "Locator", 7) == 0)
            snprintf(cur.device_locator, STR_LEN, "%s", val);
        else if (klen == 12 && strncmp(trimmed, "Bank Locator", 12) == 0)
            snprintf(cur.bank_label, STR_LEN, "%s", val);
        else if (klen == 12 && strncmp(trimmed, "Manufacturer", 12) == 0) {
            if (strcmp(val, "Unknown") != 0 && strcmp(val, "Not Specified") != 0)
                snprintf(cur.manufacturer, STR_LEN, "%s", val);
        }
        else if (klen == 11 && strncmp(trimmed, "Part Number", 11) == 0) {
            if (strcmp(val, "Unknown") != 0 && strcmp(val, "NO DIMM") != 0 && strcmp(val, "Not Specified") != 0)
                snprintf(cur.part_number, STR_LEN, "%s", val);
        }
        else if (klen == 13 && strncmp(trimmed, "Serial Number", 13) == 0) {
            if (strcmp(val, "Unknown") != 0 && strcmp(val, "Not Specified") != 0 && strcmp(val, "00000000") != 0)
                snprintf(cur.serial_number, STR_LEN, "%s", val);
        }
        else if (klen == 4 && strncmp(trimmed, "Rank", 4) == 0) {
            int r = atoi(val);
            cur.rank = (r == 4) ? RANK_QR : (r == 2) ? RANK_DR : RANK_SR;
        }
        else if (strstr(trimmed, "Configured Memory Speed") || strstr(trimmed, "Configured Clock Speed")) {
            uint32_t mhz = 0;
            sscanf(val, "%u", &mhz);
            cur.clock_speed_mhz = mhz;
        }

        line = strtok(NULL, "\n");
    }
    /* last device */
    if (in_device && cap > 0 && s_module_count < MAX_MODULES) {
        cur.capacity_bytes = cap;
        build_module_display(&cur, s_module_count);
        s_modules[s_module_count++] = cur;
    }
    free(out);
}

/* ── AGESA version ──────────────────────────────────────────────────── */

static int agesa_allowed(unsigned char c)
{
    return isalnum(c) || c == ' ' || c == '.' || c == '-';
}

static int find_agesa_in_buf(const uint8_t *buf, size_t len, char *out, size_t outsz)
{
    const char marker[] = "AGESA!V9";
    size_t mlen = strlen(marker);
    if (len < mlen) return 0;

    for (size_t i = 0; i <= len - mlen; i++) {
        if (memcmp(buf + i, marker, mlen) != 0) continue;
        size_t start = i + mlen;
        /* skip non-allowed */
        while (start < len && !agesa_allowed(buf[start])) start++;
        size_t end = start;
        while (end < len && agesa_allowed(buf[end])) end++;
        if (end > start) {
            size_t n = end - start;
            if (n >= outsz) n = outsz - 1;
            memcpy(out, buf + start, n);
            out[n] = '\0';
            return 1;
        }
        return 0;
    }
    return 0;
}

static void read_agesa_version(void)
{
    /* 1) /dev/mem BIOS region 0xE0000–0xFFFFF */
    FILE *f = fopen("/dev/mem", "rb");
    if (f) {
        const long base = 0xE0000;
        const int len = 0x100000 - 0xE0000;
        uint8_t *buf = malloc(len);
        if (buf) {
            if (fseek(f, base, SEEK_SET) == 0) {
                size_t rd = fread(buf, 1, len, f);
                if (rd > 0 && find_agesa_in_buf(buf, rd, s_agesa_version, STR_LEN)) {
                    free(buf);
                    fclose(f);
                    return;
                }
            }
            free(buf);
        }
        fclose(f);
    }

    /* 2) ACPI tables */
    const char *acpi_paths[] = {
        "/sys/firmware/acpi/tables/DSDT",
        "/sys/firmware/acpi/tables/FACP",
        "/sys/firmware/acpi/tables/XSDT",
        "/sys/firmware/acpi/tables/RSDT",
        NULL
    };
    for (int i = 0; acpi_paths[i]; i++) {
        FILE *af = fopen(acpi_paths[i], "rb");
        if (!af) continue;
        fseek(af, 0, SEEK_END);
        long sz = ftell(af);
        if (sz <= 0) { fclose(af); continue; }
        rewind(af);
        uint8_t *buf = malloc(sz);
        if (!buf) { fclose(af); continue; }
        size_t rd = fread(buf, 1, sz, af);
        fclose(af);
        if (rd > 0 && find_agesa_in_buf(buf, rd, s_agesa_version, STR_LEN)) {
            free(buf);
            return;
        }
        free(buf);
    }

    /* 3) dmidecode -t bios output (contains AGESA in BIOS version or ROM info) */
    char *out = run_command("dmidecode -t bios");
    if (out) {
        if (find_agesa_in_buf((const uint8_t *)out, strlen(out), s_agesa_version, STR_LEN)) {
            free(out);
            return;
        }
        free(out);
    }

    /* 4) Scan entire SMBIOS/DMI raw blob */
    const char *dmi_paths[] = {
        "/sys/firmware/dmi/tables/DMI",
        "/sys/firmware/dmi/tables/smbios_entry_point",
        NULL
    };
    for (int i = 0; dmi_paths[i]; i++) {
        FILE *df = fopen(dmi_paths[i], "rb");
        if (!df) continue;
        fseek(df, 0, SEEK_END);
        long sz = ftell(df);
        if (sz <= 0) { fclose(df); continue; }
        rewind(df);
        uint8_t *buf = malloc(sz);
        if (!buf) { fclose(df); continue; }
        size_t rd = fread(buf, 1, sz, df);
        fclose(df);
        if (rd > 0 && find_agesa_in_buf(buf, rd, s_agesa_version, STR_LEN)) {
            free(buf);
            return;
        }
        free(buf);
    }
}

/* ── Total memory from /proc/meminfo ────────────────────────────────── */

static void read_total_memory(char *buf, size_t sz)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) { buf[0] = '\0'; return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        uint64_t kb = 0;
        if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) {
            double gib = (double)kb / 1024.0 / 1024.0;
            snprintf(buf, sz, "%.1f GiB", gib);
            fclose(f);
            return;
        }
    }
    fclose(f);
    buf[0] = '\0';
}

/* ── Memory type from codename ──────────────────────────────────────── */

static mem_type_t mem_type_for_codename(int idx)
{
    switch (idx) {
    case 23: return MEM_DDR5;
    case 4: case 9: case 10: case 12: case 18: case 19:
        return MEM_DDR4;
    default: return MEM_UNKNOWN;
    }
}

/* ── hwmon helpers ──────────────────────────────────────────────────── */

/* Find a hwmon directory by name match. Returns path in buf or empty. */
static int find_hwmon_by_name(const char *match, char *buf, size_t sz)
{
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char namepath[512];
        snprintf(namepath, sizeof(namepath), "/sys/class/hwmon/%s/name", ent->d_name);
        char name[64];
        if (!read_file_string(namepath, name, sizeof(name))) continue;
        /* case-insensitive contains */
        char name_lower[64];
        int nli = 0;
        for (; name[nli] && nli < 63; nli++)
            name_lower[nli] = (char)tolower((unsigned char)name[nli]);
        name_lower[nli] = '\0';
        if (strstr(name_lower, match)) {
            snprintf(buf, sz, "/sys/class/hwmon/%s", ent->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static float read_temp_input(const char *hwmon_dir, int index)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/temp%d_input", hwmon_dir, index);
    int raw = read_int_file(path);
    if (raw == 0) return -1.0f;
    float c = raw / 1000.0f;
    return (c >= 0.0f && c <= 150.0f) ? c : -1.0f;
}

/* ── Zenpower overlay ───────────────────────────────────────────────── */

static void apply_zenpower(smu_metrics_t *m)
{
    char zpdir[512];
    if (!find_hwmon_by_name("zenpower", zpdir, sizeof(zpdir))) return;

    DIR *d = opendir(zpdir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        /* voltage inputs: in*_label / in*_input (millivolts) */
        if (strncmp(ent->d_name, "in", 2) == 0 && strstr(ent->d_name, "_label")) {
            int idx = 0;
            if (sscanf(ent->d_name, "in%d_label", &idx) != 1) continue;
            char lpath[512], vpath[512], label[64];
            snprintf(lpath, sizeof(lpath), "%s/in%d_label", zpdir, idx);
            snprintf(vpath, sizeof(vpath), "%s/in%d_input", zpdir, idx);
            if (!read_file_string(lpath, label, sizeof(label))) continue;
            /* lower-case */
            for (char *p = label; *p; p++) *p = (char)tolower((unsigned char)*p);
            float mv = (float)read_int_file(vpath);
            if (mv == 0) continue;
            float v = mv / 1000.0f;
            if (strstr(label, "vddcr_soc") || strstr(label, "vsoc") || strstr(label, "svi2_soc"))
                m->vsoc = v;
            else if (strstr(label, "vddio_mem") || strstr(label, "vddmem") || strstr(label, "mem vdd"))
                m->mem_vdd = v;
            else if (strstr(label, "vddq") && strstr(label, "mem"))
                m->mem_vddq = v;
            else if (strstr(label, "vpp") && strstr(label, "mem"))
                m->mem_vpp = v;
        }
        /* power: power*_label / power*_input (microwatts) */
        if (strncmp(ent->d_name, "power", 5) == 0 && strstr(ent->d_name, "_label")) {
            int idx = 0;
            if (sscanf(ent->d_name, "power%d_label", &idx) != 1) continue;
            char lpath[512], vpath[512], label[64];
            snprintf(lpath, sizeof(lpath), "%s/power%d_label", zpdir, idx);
            snprintf(vpath, sizeof(vpath), "%s/power%d_input", zpdir, idx);
            if (!read_file_string(lpath, label, sizeof(label))) continue;
            for (char *p = label; *p; p++) *p = (char)tolower((unsigned char)*p);
            float uw = (float)read_int_file(vpath);
            if (uw == 0) continue;
            float watts = uw / 1000000.0f;
            if ((strstr(label, "rapl") || strstr(label, "package")) && watts >= 0.1f && watts <= 500.0f)
                m->ppt_w = watts;
        }
        /* current: curr*_label / curr*_input (milliamps) */
        if (strncmp(ent->d_name, "curr", 4) == 0 && strstr(ent->d_name, "_label")) {
            int idx = 0;
            if (sscanf(ent->d_name, "curr%d_label", &idx) != 1) continue;
            char lpath[512], vpath[512], label[64];
            snprintf(lpath, sizeof(lpath), "%s/curr%d_label", zpdir, idx);
            snprintf(vpath, sizeof(vpath), "%s/curr%d_input", zpdir, idx);
            if (!read_file_string(lpath, label, sizeof(label))) continue;
            for (char *p = label; *p; p++) *p = (char)tolower((unsigned char)*p);
            float ma = (float)read_int_file(vpath);
            if (ma == 0) continue;
            float amps = ma / 1000.0f;
            if ((strstr(label, "core") || strstr(label, "svi2_c_core")) && amps >= 0.01f && amps <= 300.0f)
                m->package_current_a = amps;
        }
    }
    closedir(d);
}

/* ── k10temp Tctl/Tccd overlay ──────────────────────────────────────── */

static void apply_k10temp_tctl_tccd(smu_metrics_t *m)
{
    char k10dir[512];
    if (find_hwmon_by_name("k10temp", k10dir, sizeof(k10dir))) {
        float tctl = read_temp_input(k10dir, 1);
        float tccd1 = read_temp_input(k10dir, 3);
        float tccd2 = read_temp_input(k10dir, 4);
        if (tctl >= 0) { m->tctl_c = tctl; m->has_tctl = true; }
        if (tccd1 >= 0) { m->tccd1_c = tccd1; m->has_tccd1 = true; }
        if (tccd2 >= 0) { m->tccd2_c = tccd2; m->has_tccd2 = true; }

        /* Derive Tdie from Tctl if PM table didn't provide it */
        if (!m->has_tdie && m->has_tctl) {
            m->tdie_c = m->tctl_c;
            m->has_tdie = true;
        }
        return;
    }

    /* Fallback: zenpower Tdie/Tctl/Tccd1 from labels */
    char zpdir[512];
    if (!find_hwmon_by_name("zenpower", zpdir, sizeof(zpdir))) return;

    DIR *d = opendir(zpdir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strncmp(ent->d_name, "temp", 4) != 0 || !strstr(ent->d_name, "_label"))
            continue;
        int idx = 0;
        if (sscanf(ent->d_name, "temp%d_label", &idx) != 1) continue;
        char lpath[512], label[64];
        snprintf(lpath, sizeof(lpath), "%s/temp%d_label", zpdir, idx);
        if (!read_file_string(lpath, label, sizeof(label))) continue;
        float c = read_temp_input(zpdir, idx);
        if (c < 0) continue;
        /* case insensitive match */
        char lower[64];
        int li = 0;
        for (; label[li] && li < 63; li++)
            lower[li] = (char)tolower((unsigned char)label[li]);
        lower[li] = '\0';
        if (strstr(lower, "tdie")) { m->tdie_c = c; m->has_tdie = true; }
        else if (strstr(lower, "tctl")) { m->tctl_c = c; m->has_tctl = true; }
        else if (strstr(lower, "tccd1")) { m->tccd1_c = c; m->has_tccd1 = true; }
        else if (strstr(lower, "tccd2")) { m->tccd2_c = c; m->has_tccd2 = true; }
    }
    closedir(d);

    if (!m->has_tdie && m->has_tctl) {
        m->tdie_c = m->tctl_c;
        m->has_tdie = true;
    }
    if (!m->has_tdie && m->cpu_temp_c > 0) {
        m->tdie_c = m->cpu_temp_c;
        m->has_tdie = true;
    }
}

/* ── Per-core temps from hwmon "Core N" labels ──────────────────────── */

static void apply_per_core_temps_hwmon(smu_metrics_t *m)
{
    DIR *hwmon = opendir("/sys/class/hwmon");
    if (!hwmon) return;
    struct dirent *ent;
    while ((ent = readdir(hwmon))) {
        if (ent->d_name[0] == '.') continue;
        char dir[512];
        snprintf(dir, sizeof(dir), "/sys/class/hwmon/%s", ent->d_name);

        DIR *d2 = opendir(dir);
        if (!d2) continue;
        struct dirent *e2;
        while ((e2 = readdir(d2))) {
            if (strncmp(e2->d_name, "temp", 4) != 0 || !strstr(e2->d_name, "_label"))
                continue;
            int idx = 0;
            if (sscanf(e2->d_name, "temp%d_label", &idx) != 1) continue;
            char lpath[512], label[64];
            snprintf(lpath, sizeof(lpath), "%s/temp%d_label", dir, idx);
            if (!read_file_string(lpath, label, sizeof(label))) continue;
            if (strncmp(label, "Core ", 5) != 0) continue;
            int core = atoi(label + 5);
            if (core < 0 || core >= MAX_CORES) continue;
            float c = read_temp_input(dir, idx);
            if (c >= 0) {
                m->core_temps_c[core] = c;
                if (core >= m->core_temps_count)
                    m->core_temps_count = core + 1;
            }
        }
        closedir(d2);
    }
    closedir(hwmon);
}

/* ── SPD5118 DIMM temps ─────────────────────────────────────────────── */

static void read_spd_temps(smu_metrics_t *m)
{
    /* Try hwmon entries named spd5118 */
    DIR *hwmon = opendir("/sys/class/hwmon");
    if (!hwmon) return;
    struct dirent *ent;
    m->spd_temps_count = 0;
    while ((ent = readdir(hwmon)) && m->spd_temps_count < MAX_MODULES) {
        if (ent->d_name[0] == '.') continue;
        char namepath[512], name[64];
        snprintf(namepath, sizeof(namepath), "/sys/class/hwmon/%s/name", ent->d_name);
        if (!read_file_string(namepath, name, sizeof(name))) continue;
        char lower[64];
        int li = 0;
        for (; name[li] && li < 63; li++)
            lower[li] = (char)tolower((unsigned char)name[li]);
        lower[li] = '\0';
        if (!strstr(lower, "spd5118")) continue;

        char tpath[512];
        snprintf(tpath, sizeof(tpath), "/sys/class/hwmon/%s/temp1_input", ent->d_name);
        int raw = read_int_file(tpath);
        if (raw == 0) continue;
        float c = raw / 1000.0f;
        if (c >= 0.0f && c <= 150.0f)
            m->spd_temps_c[m->spd_temps_count++] = c;
    }
    closedir(hwmon);
}

/* ── Fan speeds (Nuvoton nct6xxx) ───────────────────────────────────── */

static void read_fans(fan_reading_t *fans, int *count)
{
    *count = 0;
    DIR *hwmon = opendir("/sys/class/hwmon");
    if (!hwmon) return;
    struct dirent *ent;
    while ((ent = readdir(hwmon))) {
        if (ent->d_name[0] == '.') continue;
        char namepath[512], name[64];
        snprintf(namepath, sizeof(namepath), "/sys/class/hwmon/%s/name", ent->d_name);
        if (!read_file_string(namepath, name, sizeof(name))) continue;
        char lower[64];
        int li = 0;
        for (; name[li] && li < 63; li++)
            lower[li] = (char)tolower((unsigned char)name[li]);
        lower[li] = '\0';
        if (strncmp(lower, "nct6", 4) != 0 && !strstr(lower, "nuvoton"))
            continue;

        char dir[512];
        snprintf(dir, sizeof(dir), "/sys/class/hwmon/%s", ent->d_name);
        int found_any = 0;
        for (int i = 1; i <= 7 && *count < MAX_FANS; i++) {
            char path[512];
            snprintf(path, sizeof(path), "%s/fan%d_input", dir, i);
            int rpm = read_int_file(path);
            if (rpm <= 0) continue;
            fan_reading_t *fan = &fans[(*count)++];
            if (i == 7)
                snprintf(fan->label, sizeof(fan->label), "Pump");
            else
                snprintf(fan->label, sizeof(fan->label), "Fan%d", i);
            fan->rpm = rpm;
            found_any = 1;
        }
        if (found_any) break;
    }
    closedir(hwmon);
}

/* ── /proc/stat per-core usage ──────────────────────────────────────── */

static void read_core_usage(smu_metrics_t *m)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;

    float logical_usage[MAX_LOGICAL_CPUS];
    int logical_count = 0;
    memset(logical_usage, 0, sizeof(logical_usage));

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0 || !isdigit((unsigned char)line[3]))
            continue;
        int cpuid = 0;
        uint64_t user, nice, sys, idle, iowait, irq, softirq, steal, guest, gnice;
        user = nice = sys = idle = iowait = irq = softirq = steal = guest = gnice = 0;
        if (sscanf(line, "cpu%d %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &cpuid, &user, &nice, &sys, &idle, &iowait, &irq, &softirq,
                   &steal, &guest, &gnice) < 5)
            continue;
        if (cpuid < 0 || cpuid >= MAX_LOGICAL_CPUS) continue;

        uint64_t idle_all = idle + iowait;
        uint64_t total = user + nice + sys + idle + iowait + irq + softirq + steal + guest + gnice;
        if (total == 0) continue;

        float usage = 0;
        if (s_prev_valid[cpuid]) {
            uint64_t di = idle_all - s_prev_idle[cpuid];
            uint64_t dt = total - s_prev_total[cpuid];
            if (dt > 0) {
                usage = (1.0f - (float)di / (float)dt) * 100.0f;
                if (usage < 0) usage = 0;
                if (usage > 100) usage = 100;
            }
        }
        s_prev_idle[cpuid] = idle_all;
        s_prev_total[cpuid] = total;
        s_prev_valid[cpuid] = 1;

        if (cpuid < MAX_LOGICAL_CPUS) {
            logical_usage[cpuid] = usage;
            if (cpuid >= logical_count) logical_count = cpuid + 1;
        }
    }
    fclose(f);

    /* Aggregate SMT pairs: core N = avg(cpu 2N, cpu 2N+1) */
    int max_core = (logical_count + 1) / 2;
    if (max_core > MAX_CORES) max_core = MAX_CORES;
    for (int c = 0; c < max_core; c++) {
        int l0 = c * 2, l1 = c * 2 + 1;
        float sum = 0; int cnt = 0;
        if (l0 < logical_count) { sum += logical_usage[l0]; cnt++; }
        if (l1 < logical_count) { sum += logical_usage[l1]; cnt++; }
        m->core_usage_pct[c] = cnt > 0 ? sum / cnt : 0;
    }
    m->core_usage_count = max_core;
}

/* ── Per-core frequency from cpufreq ────────────────────────────────── */

static void read_core_freq(smu_metrics_t *m)
{
    float logical_freq[MAX_LOGICAL_CPUS];
    int logical_count = 0;
    memset(logical_freq, 0, sizeof(logical_freq));

    for (int i = 0; i < MAX_LOGICAL_CPUS; i++) {
        char path[256];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        int khz = read_int_file(path);
        if (khz <= 0) {
            if (i > 0) break;
            continue;
        }
        logical_freq[i] = khz / 1000.0f;
        if (i >= logical_count) logical_count = i + 1;
    }

    int max_core = (logical_count + 1) / 2;
    if (max_core > MAX_CORES) max_core = MAX_CORES;
    for (int c = 0; c < max_core; c++) {
        int l0 = c * 2, l1 = c * 2 + 1;
        float sum = 0; int cnt = 0;
        if (l0 < logical_count && logical_freq[l0] > 0) { sum += logical_freq[l0]; cnt++; }
        if (l1 < logical_count && logical_freq[l1] > 0) { sum += logical_freq[l1]; cnt++; }
        m->core_freq_mhz[c] = cnt > 0 ? sum / cnt : 0;
    }
    m->core_freq_count = max_core;
}

/* ── BCLK from MSR ──────────────────────────────────────────────────── */

static uint64_t read_msr(int cpu, uint32_t reg)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    uint8_t buf[8];
    if (lseek(fd, (off_t)reg, SEEK_SET) < 0) { close(fd); return 0; }
    if (read(fd, buf, 8) != 8) { close(fd); return 0; }
    close(fd);

    uint64_t val = 0;
    for (int i = 0; i < 8; i++) val |= (uint64_t)buf[i] << (i * 8);
    return val;
}

static float try_read_bclk(void)
{
    /*
     * AMD Zen BCLK derivation:
     *   MSR 0xC0010064 (P-state 0) contains CpuFid and CpuDfsId.
     *   P0 frequency = (CpuFid / CpuDfsId) * 200 MHz
     *   BCLK = actual_freq / multiplier
     *
     * We read the TSC frequency (most stable) or fall back to cpuinfo_max_freq,
     * then derive BCLK = actual_freq / P0_multiplier.
     *
     * Alternatively, if the kernel exposes tsc_khz in dmesg/cpuinfo, use that.
     */

    /* Read P0 state MSR */
    uint64_t msr = read_msr(0, 0xC0010064);
    if (msr == 0) return 0;

    uint32_t cpuFid   = (uint32_t)(msr & 0xFF);
    uint32_t cpuDfsId = (uint32_t)((msr >> 8) & 0x3F);
    if (cpuDfsId == 0 || cpuFid == 0) return 0;

    double p0_mult = ((double)cpuFid / (double)cpuDfsId) * 2.0;
    if (p0_mult <= 0.1 || p0_mult > 200.0) return 0;

    /*
     * Get actual reference frequency:
     * 1. Try cpuinfo_max_freq (stable, represents max boost at current BCLK)
     * 2. Fall back to scaling_max_freq
     * 3. Fall back to scaling_cur_freq
     */
    float ref_mhz = 0;
    const char *freq_files[] = {
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq",
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
        NULL
    };
    for (int i = 0; freq_files[i]; i++) {
        int khz = read_int_file(freq_files[i]);
        if (khz > 0) { ref_mhz = khz / 1000.0f; break; }
    }
    if (ref_mhz <= 0) return 0;

    /*
     * The max freq reported by cpuinfo_max_freq corresponds to the max boost
     * P-state. Read the max boost P-state (highest enabled) to get its multiplier.
     * For simplicity, use P0 which is the highest performance state.
     *
     * BCLK = ref_mhz / p0_mult
     */
    float bclk = (float)(ref_mhz / p0_mult);
    return (bclk >= 80.0f && bclk <= 120.0f) ? bclk : 0;
}

/* ── Read PM table binary ───────────────────────────────────────────── */

static int read_pm_table_raw(float **out_floats, int *out_count)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/pm_table", SMU_PATH);

    FILE *f = fopen(path, "rb");
    if (!f) { *out_floats = NULL; *out_count = 0; return 0; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 4) { fclose(f); *out_floats = NULL; *out_count = 0; return 0; }
    rewind(f);

    uint8_t *raw = malloc(sz);
    if (!raw) { fclose(f); *out_floats = NULL; *out_count = 0; return 0; }
    size_t rd = fread(raw, 1, sz, f);
    fclose(f);
    if ((int)rd < 4) { free(raw); *out_floats = NULL; *out_count = 0; return 0; }

    int count = (int)rd / 4;
    float *floats = malloc(count * sizeof(float));
    if (!floats) { free(raw); *out_floats = NULL; *out_count = 0; return 0; }
    memcpy(floats, raw, count * sizeof(float));
    free(raw);

    *out_floats = floats;
    *out_count = count;
    return 1;
}


/* ── Public API ─────────────────────────────────────────────────────── */

int backend_is_supported(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/version", SMU_PATH);
    return file_exists(path);
}

void backend_read_summary(system_summary_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Cache static data on first call */
    if (!s_cached_static) {
        parse_dmidecode_processor();
        parse_dmidecode_board();
        parse_dmidecode_memory();
        read_agesa_version();
        s_cached_static = 1;
    }

    /* CPU info */
    int codename_idx = read_codename_index();
    snprintf(out->cpu.name, STR_LEN, "AMD Ryzen (from ryzen_smu)");
    snprintf(out->cpu.processor_name, STR_LEN, "%s", s_processor_name);
    snprintf(out->cpu.codename, STR_SHORT, "%s", map_codename(codename_idx));
    read_smu_string("version", out->cpu.smu_version, STR_SHORT);

    uint32_t pm_ver = read_smu_uint32("pm_table_version");
    if (pm_ver)
        snprintf(out->cpu.pm_table_version, STR_SHORT, "PM table 0x%08X", pm_ver);

    /* Board info */
    snprintf(out->board.motherboard, STR_LEN, "%s", s_board_product);
    snprintf(out->board.bios_version, STR_LEN, "%s", s_bios_version);
    snprintf(out->board.bios_date, STR_SHORT, "%s", s_bios_date);
    snprintf(out->board.agesa_version, STR_LEN, "%s", s_agesa_version);
    snprintf(out->board.display_line, sizeof(out->board.display_line),
             "%s | BIOS %s (%s) | AGESA %s",
             s_board_product, s_bios_version, s_bios_date,
             s_agesa_version[0] ? s_agesa_version : "N/A");

    /* Modules (cached) */
    out->module_count = s_module_count;
    memcpy(out->modules, s_modules, s_module_count * sizeof(memory_module_t));

    /* PM table → metrics */
    float *pm_floats = NULL;
    int pm_count = 0;
    if (read_pm_table_raw(&pm_floats, &pm_count)) {
        pm_table_read(pm_ver, pm_floats, pm_count, codename_idx, &out->metrics);
        free(pm_floats);
    }

    /* BCLK from MSR */
    out->metrics.bclk_mhz = try_read_bclk();

    /* hwmon overlays */
    apply_zenpower(&out->metrics);
    apply_per_core_temps_hwmon(&out->metrics);
    apply_k10temp_tctl_tccd(&out->metrics);
    read_spd_temps(&out->metrics);

    /* Per-core usage and frequency */
    read_core_usage(&out->metrics);
    read_core_freq(&out->metrics);

    /* DRAM timings */
    dram_read_timings(codename_idx, &out->dram);

    /* Memory config */
    out->memory.type = mem_type_for_codename(codename_idx);
    float mem_freq = out->dram.frequency_hint_mhz;
    if (mem_freq == 0 && out->memory.type == MEM_DDR4 && out->metrics.mclk_mhz > 0)
        mem_freq = out->metrics.mclk_mhz;
    out->memory.frequency = mem_freq;
    read_total_memory(out->memory.total_capacity, sizeof(out->memory.total_capacity));

    /* Build part number string from unique module part numbers */
    char *pn_buf = out->memory.part_number;
    pn_buf[0] = '\0';
    for (int i = 0; i < s_module_count; i++) {
        if (s_modules[i].part_number[0] == '\0') continue;
        /* check duplicate */
        if (strstr(pn_buf, s_modules[i].part_number)) continue;
        if (pn_buf[0] != '\0') strncat(pn_buf, ", ", STR_LEN - strlen(pn_buf) - 1);
        strncat(pn_buf, s_modules[i].part_number, STR_LEN - strlen(pn_buf) - 1);
    }

    /* Fans */
    read_fans(out->fans, &out->fan_count);
}
