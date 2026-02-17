#!/usr/bin/env python3
"""
Parse PM table and output key=value pairs for ZenTimings.
Uses same logic as dump_pm_voltages.py - reads and parses floats.
"""
import struct
import sys
from pathlib import Path

PM_TABLE_PATH = Path("/sys/kernel/ryzen_smu_drv/pm_table")

# Granite Ridge offsets (byte offset / 4 = float index)
OFFSETS = {
    "FCLK": 0x11C // 4,   # 71
    "UCLK": 0x12C // 4,   # 75
    "MCLK": 0x13C // 4,   # 79
    "VSOC": 0x14C // 4,   # 83
    "VDDP": 0x434 // 4,   # 269
    "VDDG_IOD": 0x40C // 4,  # 259
    "VDDG_CCD": 0x414 // 4,  # 261
    "VDD_MISC": 0xE8 // 4,   # 58
}

def main():
    try:
        data = PM_TABLE_PATH.read_bytes()
    except (PermissionError, FileNotFoundError) as e:
        print(f"ERROR:{e}", file=sys.stderr)
        sys.exit(1)

    if len(data) < 4:
        sys.exit(1)

    floats = list(struct.unpack("<%df" % (len(data) // 4), data))

    for key, idx in OFFSETS.items():
        if idx < len(floats):
            print(f"{key}={floats[idx]}")

if __name__ == "__main__":
    main()
