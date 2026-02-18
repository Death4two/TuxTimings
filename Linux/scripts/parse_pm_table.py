#!/usr/bin/env python3
"""
Parse the PM table exposed by ryzen_smu_drv.
Detects the PM table version and uses per-family index mappings
(derived from pm_tables.c) so the correct pm_element(i) is read
for each logical value on every supported CPU.

Output keys are identical across all families for backend compatibility:
  FCLK, UCLK, MCLK, VSOC, VDDP, VDDG_IOD, VDDG_CCD, VDD_MISC, VCORE,
  IOD_HOTSPOT, VID, PPT, POWER, CORE_VOLTAGE_0..N, CORE_TEMP_0..N
"""
import struct
import sys
from pathlib import Path

PM_TABLE_PATH = Path("/sys/kernel/ryzen_smu_drv/pm_table")
PM_TABLE_VERSION_PATH = Path("/sys/kernel/ryzen_smu_drv/pm_table_version")

# ---------------------------------------------------------------------------
# Per-version index tables.
#
# "named"              – list of (pm_element_index, output_key)
# "vid"                – pm_element index for VID (VID_LIMIT)
# "ppt"                – pm_element index for PPT_VALUE (W) or PPT_VALUE slow on APU
# "socket_power"       – pm_element index for SOCKET_POWER (W)
# "core_voltage_start" – first pm_element index for consecutive CORE_VOLTAGE
# "core_temp_start"    – first pm_element index for consecutive CORE_TEMP
# "max_cores"          – how many cores this table version supports
# ---------------------------------------------------------------------------

# Granite Ridge (default / fallback for unknown versions)
GRANITE_RIDGE = {
    "named": [
        (11,  "IOD_HOTSPOT"),
        (58,  "VDD_MISC"),
        (71,  "FCLK"),
        (75,  "UCLK"),
        (79,  "MCLK"),
        (83,  "VSOC"),
        (259, "VDDG_IOD"),
        (261, "VDDG_CCD"),
        (269, "VDDP"),
        (271, "VCORE"),
    ],
    "vid": 275,
    "ppt": 3,             # Granite Ridge: index 3 for CPU PPT
    "socket_power": 29,
    "core_voltage_start": 309,
    "core_temp_start": 317,
    "max_cores": 8,
}

# ── Zen 3 Vermeer (Desktop) ─────────────────────────────────────────────────

VERMEER_0x380804 = {  # 5900X / 5950X  16-core  (older BIOS, SMU ≤56.45)
    "named": [
        (11,  "IOD_HOTSPOT"),   # VID_VALUE
        (48,  "FCLK"),          # FCLK_FREQ
        (50,  "UCLK"),          # UCLK_FREQ
        (51,  "MCLK"),          # MEMCLK_FREQ
        (44,  "VSOC"),          # SOC_SET_VOLTAGE
        (137, "VDDP"),          # V_VDDP
        (138, "VDDG_IOD"),      # V_VDDG_IOD
        (139, "VDDG_CCD"),      # V_VDDG_CCD
        (40,  "VCORE"),         # CPU_SET_VOLTAGE
    ],
    "vid": 10,
    "ppt": 1,
    "socket_power": 29,
    "core_voltage_start": 185,
    "core_temp_start": 201,
    "max_cores": 16,
}

VERMEER_0x380805 = {  # 5900X / 5950X  16-core  (newer BIOS, SMU 56.50+)
    "named": [
        (11,  "IOD_HOTSPOT"),
        (48,  "FCLK"),
        (50,  "UCLK"),
        (51,  "MCLK"),
        (44,  "VSOC"),
        (137, "VDDP"),
        (138, "VDDG_IOD"),
        (139, "VDDG_CCD"),
        (39,  "VCORE"),         # shifted 40→39
    ],
    "vid": 10,
    "ppt": 1,
    "socket_power": 29,
    "core_voltage_start": 188,
    "core_temp_start": 204,
    "max_cores": 16,
}

VERMEER_0x380904 = {  # 5600X  8-core  (older BIOS)
    "named": [
        (11,  "IOD_HOTSPOT"),
        (48,  "FCLK"),
        (50,  "UCLK"),
        (51,  "MCLK"),
        (44,  "VSOC"),
        (137, "VDDP"),
        (138, "VDDG_IOD"),
        (139, "VDDG_CCD"),
        (40,  "VCORE"),
    ],
    "vid": 10,
    "ppt": 1,
    "socket_power": 29,
    "core_voltage_start": 177,
    "core_temp_start": 185,
    "max_cores": 8,
}

VERMEER_0x380905 = {  # 5600X  8-core  (newer BIOS)
    "named": [
        (11,  "IOD_HOTSPOT"),
        (48,  "FCLK"),
        (50,  "UCLK"),
        (51,  "MCLK"),
        (44,  "VSOC"),
        (137, "VDDP"),
        (138, "VDDG_IOD"),
        (139, "VDDG_CCD"),
        (39,  "VCORE"),
    ],
    "vid": 10,
    "ppt": 1,
    "socket_power": 29,
    "core_voltage_start": 180,
    "core_temp_start": 188,
    "max_cores": 8,
}

# ── Zen 3 Cezanne (APU) ─────────────────────────────────────────────────────

CEZANNE_0x400005 = {  # 5700G  8-core APU
    "named": [
        (29,  "IOD_HOTSPOT"),   # VID_VALUE
        (409, "FCLK"),          # FCLK_FREQ
        (410, "UCLK"),          # UCLK_FREQ
        (411, "MCLK"),          # MEMCLK_FREQ
        (102, "VSOC"),          # SOC_SET_VOLTAGE
        (565, "VDDP"),          # V_VDDP
        (98,  "VCORE"),         # CPU_SET_VOLTAGE
    ],
    "vid": 28,
    "ppt": 5,             # PPT_VALUE (slow) on APU
    "socket_power": 38,
    "core_voltage_start": 208,
    "core_temp_start": 216,
    "max_cores": 8,
}

# ── Zen 2 Matisse (Desktop) ─────────────────────────────────────────────────

MATISSE_0x240903 = {  # 3700X / 3800X  8-core
    "named": [
        (11,  "IOD_HOTSPOT"),
        (48,  "FCLK"),
        (50,  "UCLK"),
        (51,  "MCLK"),
        (44,  "VSOC"),
        (125, "VDDP"),          # V_VDDP
        (126, "VDDG_IOD"),      # V_VDDG (single on Zen 2)
        (39,  "VCORE"),         # CPU_SET_VOLTAGE
    ],
    "vid": 10,
    "ppt": 1,
    "socket_power": 29,
    "core_voltage_start": 155,
    "core_temp_start": 163,
    "max_cores": 8,
}

MATISSE_0x240803 = {  # 3950X  16-core
    "named": [
        (11,  "IOD_HOTSPOT"),
        (48,  "FCLK"),
        (50,  "UCLK"),
        (51,  "MCLK"),
        (44,  "VSOC"),
        (125, "VDDP"),
        (126, "VDDG_IOD"),      # V_VDDG
        (40,  "VCORE"),
    ],
    "vid": 10,
    "ppt": 1,
    "socket_power": 29,
    "core_voltage_start": 163,
    "core_temp_start": 179,
    "max_cores": 16,
}

# ── Zen 2 Renoir (APU) ──────────────────────────────────────────────────────

RENOIR_0x370003 = {  # 4800U etc.  8-core APU
    "named": [
        (29,  "IOD_HOTSPOT"),
        (371, "FCLK"),
        (372, "UCLK"),
        (373, "MCLK"),
        (101, "VSOC"),
        (527, "VDDP"),
        (97,  "VCORE"),
    ],
    "vid": 28,
    "ppt": 5,
    "socket_power": 38,
    "core_voltage_start": 200,
    "core_temp_start": 208,
    "max_cores": 8,
}

RENOIR_0x370005 = {  # Renoir v2  8-core APU
    "named": [
        (29,  "IOD_HOTSPOT"),
        (378, "FCLK"),
        (379, "UCLK"),
        (380, "MCLK"),
        (101, "VSOC"),
        (534, "VDDP"),
        (97,  "VCORE"),
    ],
    "vid": 28,
    "ppt": 5,
    "socket_power": 38,
    "core_voltage_start": 207,
    "core_temp_start": 215,
    "max_cores": 8,
}

# ── Zen 1 Raven Ridge (APU) ─────────────────────────────────────────────────

RAVEN_0x1E0004 = {  # 2500U etc.  4-core APU
    "named": [
        (61,  "IOD_HOTSPOT"),   # VID_VALUE
        (166, "FCLK"),
        (167, "UCLK"),
        (168, "MCLK"),
        (65,  "VSOC"),          # SOC_SET_VOLTAGE
        (60,  "VDDP"),          # V_VDDP
        (61,  "VCORE"),         # CPU_SET_VOLTAGE (same idx as VID_VALUE)
    ],
    "vid": 57,
    "ppt": 5,
    "socket_power": 38,
    "core_voltage_start": 104,
    "core_temp_start": 108,
    "max_cores": 4,
}

# ── Version dispatch ─────────────────────────────────────────────────────────

VERSION_MAP = {
    0x380804: VERMEER_0x380804,
    0x380805: VERMEER_0x380805,
    0x380904: VERMEER_0x380904,
    0x380905: VERMEER_0x380905,
    0x400005: CEZANNE_0x400005,
    0x240903: MATISSE_0x240903,
    0x240803: MATISSE_0x240803,
    0x370003: RENOIR_0x370003,
    0x370005: RENOIR_0x370005,
    0x1E0004: RAVEN_0x1E0004,
}


def detect_version():
    try:
        text = PM_TABLE_VERSION_PATH.read_text().strip()
        return int(text, 0)
    except (FileNotFoundError, PermissionError, ValueError):
        return None


def main():
    try:
        data = PM_TABLE_PATH.read_bytes()
    except (PermissionError, FileNotFoundError) as e:
        print(f"ERROR:{e}", file=sys.stderr)
        sys.exit(1)

    if len(data) < 4:
        sys.exit(1)

    n = len(data) // 4
    floats = list(struct.unpack(f"<{n}f", data))

    version = detect_version()
    mapping = VERSION_MAP.get(version, GRANITE_RIDGE)

    for idx, key in mapping["named"]:
        if idx < len(floats):
            print(f"{key}={floats[idx]}")

    vid_idx = mapping["vid"]
    if vid_idx < len(floats):
        print(f"VID={floats[vid_idx]}")

    ppt_idx = mapping.get("ppt")
    if ppt_idx is not None and ppt_idx < len(floats):
        v = floats[ppt_idx]
        if 0.5 <= v <= 400:
            print(f"PPT={v}")
    sock_idx = mapping.get("socket_power")
    if sock_idx is not None and sock_idx < len(floats):
        v = floats[sock_idx]
        if 0.5 <= v <= 400:
            print(f"POWER={v}")

    start = mapping["core_voltage_start"]
    for i in range(mapping["max_cores"]):
        idx = start + i
        if idx < len(floats):
            print(f"CORE_VOLTAGE_{i}={floats[idx]}")

    start = mapping["core_temp_start"]
    for i in range(mapping["max_cores"]):
        idx = start + i
        if idx < len(floats):
            print(f"CORE_TEMP_{i}={floats[idx]}")


if __name__ == "__main__":
    main()
