#!/usr/bin/env python3
"""
Parse PM table and output key=value pairs for TuxTimings.
Same indices as backend: power, PPT, current, temp, core clocks.
"""
import struct
import sys
from pathlib import Path

PM_TABLE_PATH = Path("/sys/kernel/ryzen_smu_drv/pm_table")

# Byte offset / 4 = float index
OFFSETS = {
    "POWER": 0,
    "CORE_MHZ": 2,
    "FCLK": 0x11C // 4,   # 71
    "UCLK": 0x12C // 4,   # 75
    "MCLK": 0x13C // 4,   # 79
    "VSOC": 0x14C // 4,   # 83
    "VDDP": 0x434 // 4,   # 269
    "VDDG_IOD": 0x40C // 4,  # 259
    "VDDG_CCD": 0x414 // 4,  # 261
    "VDD_MISC": 0xE8 // 4,   # 58
    # VCORE emitted separately with 0.25-2.2 V range check (idx 271)
}

# Indices used by backend for power/current/temp (same as RyzenSmuBackend.ReadKnownPmIndices / TryPlausible*)
IDX_PPT_PRIMARY = 3
IDX_PPT_FALLBACK = 26
IDX_CURRENT = (41, 46, 3, 4)   # try in order
IDX_TEMP_EARLY = 1
IDX_POWER_CANDIDATES = (220, 187, 29, 42, 0, 1)  # package power: 220 from user dumps, then fallbacks


def plausible_power(v):
    return v is not None and 0.5 <= v <= 400


def plausible_current(v):
    return v is not None and 0.5 <= v <= 200


def plausible_temp(v):
    return v is not None and 1 <= v <= 150


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

    def at(i):
        return floats[i] if 0 <= i < len(floats) else None

    # Power: try known package power index 220 first, then other candidates
    power = None
    for idx in IDX_POWER_CANDIDATES:
        v = at(idx)
        if plausible_power(v):
            power = v
            break
    if power is None and plausible_power(at(0)):
        power = at(0)

    # PPT (indices 3, 26)
    ppt = None
    if plausible_power(at(IDX_PPT_PRIMARY)):
        ppt = at(IDX_PPT_PRIMARY)
    elif plausible_power(at(IDX_PPT_FALLBACK)):
        ppt = at(IDX_PPT_FALLBACK)

    # Current (A)
    current = None
    for idx in IDX_CURRENT:
        v = at(idx)
        if plausible_current(v):
            current = v
            break

    # Temp: index 1 for early temp, 448/449 for tdie-style temp
    temp = None
    if plausible_temp(at(IDX_TEMP_EARLY)):
        temp = at(IDX_TEMP_EARLY)
    if n > 449:
        a, b = at(448), at(449)
        if plausible_temp(a):
            temp = a
        elif plausible_temp(b):
            temp = b
        elif a is not None and b is not None and a > 0 and b > 0:
            temp = (a + b) / 2.0

    # Core MHz: index 2 might be MHz or GHz
    core_mhz = None
    v2 = at(2)
    if v2 is not None:
        if 500 <= v2 <= 6500:
            core_mhz = v2
        elif 0.5 <= v2 <= 6.5:
            core_mhz = v2 * 1000
    if core_mhz is None and n > 340:
        # Per-core GHz at 325-340; use max * 1000
        vals = [floats[i] for i in range(325, min(341, n)) if 0.5 <= floats[i] <= 6.5]
        if vals:
            core_mhz = max(vals) * 1000

    # Per-core temps °C from indices 317-324 (up to 8 cores)
    core_temps = []
    if n > 324:
        for i in range(8):
            idx = 317 + i
            if idx < n:
                v = at(idx)
                if v is not None and 0 <= v <= 150:
                    core_temps.append(v)

    # Emit key=value for all standard offsets
    for key, idx in OFFSETS.items():
        if idx < len(floats):
            print(f"{key}={floats[idx]}")

    # VCORE from idx 271 (Granite Ridge); only if in plausible range 0.25-2.2 V
    vcore_val = at(271)
    if vcore_val is not None and 0.25 <= vcore_val <= 2.2:
        print(f"VCORE={vcore_val}")
    else:
        print("VCORE=0")

    # Override / add parsed power, PPT, current, temp, core
    if power is not None:
        print(f"POWER={power}")
    if ppt is not None:
        print(f"PPT={ppt}")
    if current is not None:
        print(f"CURRENT={current}")
    if temp is not None:
        print(f"TEMP={temp}")
    if core_mhz is not None:
        print(f"CORE_MHZ={core_mhz}")
    for i, t in enumerate(core_temps):
        print(f"CORE_TEMP_{i}={t}")

if __name__ == "__main__":
    main()
