using System;
using System.Collections.Generic;
using System.ComponentModel;

namespace TuxTimings.Core;

public enum MemRank
{
    SR = 0,
    DR = 1,
    QR = 2,
}

public sealed class MemoryModule
{
    public string BankLabel { get; init; } = string.Empty;
    public string PartNumber { get; init; } = string.Empty;
    public string Manufacturer { get; init; } = string.Empty;
    public string DeviceLocator { get; init; } = string.Empty;
    public ulong CapacityBytes { get; init; }
    public uint ClockSpeedMHz { get; init; }
    public MemRank Rank { get; init; }
    public string Slot { get; init; } = string.Empty;
}

public enum MemType
{
    Unknown = 0,
    DDR4 = 1,
    DDR5 = 2,
    LPDDR4 = 3,
    LPDDR5 = 4,
}

public sealed class MemoryConfigModel : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    private float _frequency;
    public float Frequency
    {
        get => _frequency;
        set
        {
            if (Math.Abs(_frequency - value) < 0.0001f) return;
            _frequency = value;
            OnPropertyChanged(nameof(Frequency));
            OnPropertyChanged(nameof(FrequencyString));
        }
    }

    public string FrequencyString => $"{Math.Floor(Frequency)} MT/s";

    public MemType Type { get; set; } = MemType.Unknown;

    public string TotalCapacity { get; set; } = string.Empty;

    /// <summary>RAM part number(s) from dmidecode -t memory (e.g. F5-7600J3646G16G).</summary>
    public string PartNumber { get; set; } = string.Empty;

    private void OnPropertyChanged(string propertyName) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}

public sealed class CpuInfoModel
{
    public string Name { get; init; } = string.Empty;
    /// <summary>Actual processor name from dmidecode (e.g. AMD Ryzen 9 9900X 12-Core Processor).</summary>
    public string ProcessorName { get; init; } = string.Empty;
    /// <summary>ProcessorName when available, otherwise Name.</summary>
    public string DisplayName => !string.IsNullOrEmpty(ProcessorName) ? ProcessorName : Name;
    public string CodeName { get; init; } = string.Empty;
    public string Package { get; init; } = string.Empty;
    public string SmuVersion { get; init; } = string.Empty;
}

public sealed class SmuMetrics
{
    public float CpuPackagePowerWatts { get; init; }
    /// <summary>CPU PPT (Package Power Tracking) in watts from PM table indices 3, 26.</summary>
    public float CpuPptWatts { get; init; }
    /// <summary>Package current in amps (from zenpower or PM table when available).</summary>
    public float CpuPackageCurrentAmps { get; init; }
    /// <summary>Core voltage (V) from PM table index 308 (Granite Ridge). 0 if unavailable.</summary>
    public float Vcore { get; init; }
    public float CpuTempCelsius { get; init; }
    /// <summary>Per-core temperatures °C from PM table indices 317–324 (up to 8 cores).</summary>
    public IReadOnlyList<float> CoreTempsCelsius { get; init; } = Array.Empty<float>();
    public float CoreClockMHz { get; init; }
    /// <summary>Per-core clocks in GHz from PM table indices 325–340 (up to 16 cores).</summary>
    public IReadOnlyList<float> CoreClocksGhz { get; init; } = Array.Empty<float>();
    public float MemoryClockMHz { get; init; }
    public float FclkMHz { get; init; }
    public float UclkMHz { get; init; }
    public float MclkMHz { get; init; }
    public float Vsoc { get; init; }
    public float Vddp { get; init; }
    public float VddgCcd { get; init; }
    public float VddgIod { get; init; }
    public float VddMisc { get; init; }
    public float CpuVddio { get; init; }
    public float MemVdd { get; init; }
    public float MemVddq { get; init; }
    public float MemVpp { get; init; }
}

public sealed class DramTimingsModel
{
    // Primary timings
    public uint Tcl { get; init; }
    public uint Trcd { get; init; }
    public uint Trp { get; init; }
    public uint Tras { get; init; }
    public uint Trc { get; init; }

    // Common secondary timings
    public uint Trrds { get; init; }
    public uint Trrdl { get; init; }
    public uint Tfaw { get; init; }
    public uint Twr { get; init; }
    public uint Tcwl { get; init; }

    // Additional secondary / tertiary timings (subset)
    public uint Rtp { get; init; }
    public uint Wtrs { get; init; }
    public uint Wtrl { get; init; }
    public uint Rdwr { get; init; }
    public uint Wrrd { get; init; }
    public uint RdrdScl { get; init; }
    public uint WrwrScl { get; init; }
    public uint RdrdSc { get; init; }
    public uint RdrdSd { get; init; }
    public uint RdrdDd { get; init; }
    public uint WrwrSc { get; init; }
    public uint WrwrSd { get; init; }
    public uint WrwrDd { get; init; }
    public uint TrcPage { get; init; }
    public uint Mod { get; init; }
    public uint ModPda { get; init; }
    public uint Mrd { get; init; }
    public uint MrdPda { get; init; }
    public uint Stag { get; init; }
    public uint StagSb { get; init; }
    public uint Cke { get; init; }
    public uint Xp { get; init; }
    public uint PhyWrd { get; init; }
    public uint PhyWrl { get; init; }
    public uint PhyRdl { get; init; }
    public uint Refi { get; init; }
    public uint Wrpre { get; init; }
    public uint Rdpre { get; init; }
    public uint Rfc { get; init; }
    public uint Rfc2 { get; init; }
    public uint Rfcsb { get; init; }

    public float TrefiNs { get; init; }
    public float TrfcNs { get; init; }
    public float Trfc2Ns { get; init; }
    public float TrfcsbNs { get; init; }

    public bool GdmEnabled { get; init; }
    public bool PowerDownEnabled { get; init; }
    public string Cmd2T { get; init; } = string.Empty;

    // Optional hint: effective memory clock in MHz used when these
    // timings were derived (useful for DDR5 where ratio is encoded).
    public float FrequencyHintMHz { get; init; }
}

/// <summary>Board and BIOS info from dmidecode -s baseboard-product-name / bios-version / bios-release-date.</summary>
public sealed class BoardInfoModel
{
    public string MotherboardProductName { get; init; } = string.Empty;
    public string BiosVersion { get; init; } = string.Empty;
    public string BiosReleaseDate { get; init; } = string.Empty;

    /// <summary>Single line for UI: "Board | BIOS version (date)" or empty.</summary>
    public string DisplayLine
    {
        get
        {
            var parts = new List<string>();
            if (!string.IsNullOrEmpty(MotherboardProductName))
                parts.Add(MotherboardProductName);
            if (!string.IsNullOrEmpty(BiosVersion) || !string.IsNullOrEmpty(BiosReleaseDate))
            {
                var bios = string.IsNullOrEmpty(BiosReleaseDate)
                    ? $"BIOS {BiosVersion}"
                    : $"BIOS {BiosVersion} ({BiosReleaseDate})";
                parts.Add(bios);
            }
            return parts.Count == 0 ? string.Empty : string.Join(" | ", parts);
        }
    }
}

public sealed class SystemSummary
{
    public CpuInfoModel Cpu { get; init; } = new();
    public MemoryConfigModel Memory { get; init; } = new();
    public BoardInfoModel BoardInfo { get; init; } = new();
    public IReadOnlyList<MemoryModule> Modules { get; init; } = Array.Empty<MemoryModule>();
    public SmuMetrics Metrics { get; init; } = new();
    public DramTimingsModel DramTimings { get; init; } = new();
}

public interface IHardwareBackend
{
    bool IsSupported();

    SystemSummary ReadSummary();
}
