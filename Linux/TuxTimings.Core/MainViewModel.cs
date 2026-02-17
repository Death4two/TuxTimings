using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Reflection;
using Timer = System.Timers.Timer;

namespace TuxTimings.Core;

public sealed class MainViewModel : INotifyPropertyChanged
{
    private readonly IHardwareBackend _backend;
    private readonly Timer _timer;

    public event PropertyChangedEventHandler? PropertyChanged;

    private SystemSummary _summary = new();
    // Cached Modules list reference — only updated when the actual list changes
    // so the ComboBox doesn't reset its selection on every refresh.
    private IReadOnlyList<MemoryModule> _cachedModules = Array.Empty<MemoryModule>();

    public SystemSummary Summary
    {
        get => _summary;
        private set
        {
            var prevModulesRef = _summary.Modules;
            _summary = value;
            OnPropertyChanged();

            // Only fire Modules change when the list reference actually changed (first read).
            var newModules = value.Modules ?? Array.Empty<MemoryModule>();
            if (!ReferenceEquals(prevModulesRef, newModules) && newModules.Count > 0)
            {
                _cachedModules = newModules;
                OnPropertyChanged(nameof(Modules));
            }

            // Auto-select first module if nothing is selected yet but modules exist.
            if (_selectedModuleIndex < 0 && _cachedModules.Count > 0)
            {
                _selectedModuleIndex = 0;
                OnPropertyChanged(nameof(SelectedModuleIndex));
            }

            OnPropertyChanged(nameof(SelectedModuleCapacity));
            OnPropertyChanged(nameof(SelectedModuleManufacturer));
            OnPropertyChanged(nameof(SelectedModulePartNumber));
            OnPropertyChanged(nameof(SelectedModuleSerialNumber));
            OnPropertyChanged(nameof(SelectedModuleRankDisplay));
            OnPropertyChanged(nameof(SelectedPhyRdl));
            UpdateSelectedPhyRdlDisplay();
            OnPropertyChanged(nameof(CoreTempsDisplay));
            OnPropertyChanged(nameof(TctlTccdDisplay));
            OnPropertyChanged(nameof(SelectedSpdTempDisplay));
            OnPropertyChanged(nameof(FanDisplayLines));
            OnPropertyChanged(nameof(PumpDisplay));
            OnPropertyChanged(nameof(BclkMHz));
        }
    }
    /// <summary>Installed DIMMs from dmidecode -t 17 for dropdown.</summary>
    public IReadOnlyList<MemoryModule> Modules => _cachedModules;

    private int _selectedModuleIndex = -1;
    private string _selectedPhyRdlDisplay = "—";
    /// <summary>Selected DIMM index in Modules; -1 when none. Bound to ComboBox SelectedIndex.</summary>
    public int SelectedModuleIndex
    {
        get => _selectedModuleIndex;
        set
        {
            var clamped = value < 0 ? -1 : (value >= _cachedModules.Count ? (_cachedModules.Count > 0 ? _cachedModules.Count - 1 : -1) : value);
            if (_selectedModuleIndex == clamped) return;
            _selectedModuleIndex = clamped;
            OnPropertyChanged();
            OnPropertyChanged(nameof(SelectedModuleCapacity));
            OnPropertyChanged(nameof(SelectedModuleManufacturer));
            OnPropertyChanged(nameof(SelectedModulePartNumber));
            OnPropertyChanged(nameof(SelectedModuleSerialNumber));
            OnPropertyChanged(nameof(SelectedModuleRankDisplay));
            OnPropertyChanged(nameof(SelectedPhyRdl));
            UpdateSelectedPhyRdlDisplay();
            OnPropertyChanged(nameof(SelectedSpdTempDisplay));
        }
    }

    private void UpdateSelectedPhyRdlDisplay()
    {
        var val = SelectedPhyRdl.ToString();
        if (_selectedPhyRdlDisplay == val) return;
        _selectedPhyRdlDisplay = val;
        OnPropertyChanged(nameof(SelectedPhyRdlDisplay));
    }

    /// <summary>Serial number of the selected DIMM or "—".</summary>
    public string SelectedModuleSerialNumber
    {
        get
        {
            var mod = SelectedModule;
            return mod != null && !string.IsNullOrEmpty(mod.SerialNumber) ? mod.SerialNumber : "—";
        }
    }

    /// <summary>Rank of the selected DIMM from dmidecode: SR, DR, or QR.</summary>
    public string SelectedModuleRankDisplay
    {
        get
        {
            var mod = SelectedModule;
            if (mod == null) return "—";
            return mod.Rank switch
            {
                MemRank.SR => "SR",
                MemRank.DR => "DR",
                MemRank.QR => "QR",
                _ => "SR"
            };
        }
    }

    /// <summary>tPHYRDL for the selected DIMM (per-channel) or global PhyRdl fallback.</summary>
    public uint SelectedPhyRdl
    {
        get
        {
            var perCh = Summary.DramTimings?.PhyRdlPerChannel;
            if (perCh != null && _selectedModuleIndex >= 0 && _selectedModuleIndex < perCh.Count)
                return perCh[_selectedModuleIndex];
            return Summary.DramTimings?.PhyRdl ?? 0;
        }
    }

    /// <summary>tPHYRDL as string for UI binding; updated explicitly when selection or Summary changes.</summary>
    public string SelectedPhyRdlDisplay => _selectedPhyRdlDisplay;

    /// <summary>Capacity of the selected DIMM (e.g. "16.0 GiB") or "—".</summary>
    public string SelectedModuleCapacity
    {
        get
        {
            var mod = SelectedModule;
            return mod != null ? mod.CapacityDisplay : "—";
        }
    }

    /// <summary>Manufacturer of the selected DIMM or "—".</summary>
    public string SelectedModuleManufacturer
    {
        get
        {
            var mod = SelectedModule;
            return mod != null && !string.IsNullOrEmpty(mod.Manufacturer) ? mod.Manufacturer : "—";
        }
    }

    /// <summary>Part number of the selected DIMM or "—".</summary>
    public string SelectedModulePartNumber
    {
        get
        {
            var mod = SelectedModule;
            return mod != null && !string.IsNullOrEmpty(mod.PartNumber) ? mod.PartNumber : "—";
        }
    }

    private MemoryModule? SelectedModule
    {
        get
        {
            if (_selectedModuleIndex < 0 || _selectedModuleIndex >= _cachedModules.Count) return null;
            return _cachedModules[_selectedModuleIndex];
        }
    }

    /// <summary>BCLK in MHz. AM5 Ryzen uses 100 MHz reference clock; derive from FCLK when available.</summary>
    public float BclkMHz
    {
        get
        {
            // BCLK = reference clock. For AM5 Ryzen, 100 MHz is the standard value.
            // If FCLK is available, derive BCLK = FCLK / round(FCLK/100) to detect BCLK OC.
            var fclk = Summary.Metrics.FclkMHz;
            if (fclk > 0)
            {
                float ratio = MathF.Round(fclk / 100f);
                if (ratio >= 1f)
                    return fclk / ratio;
            }
            return 100.0f;
        }
    }

    private bool IsPerCoreTempsSupportedCpu
    {
        get
        {
            var cpu = Summary?.Cpu;
            if (cpu == null) return false;
            var name = cpu.DisplayName ?? cpu.ProcessorName ?? cpu.Name ?? string.Empty;
            if (string.IsNullOrEmpty(name)) return false;
            var lower = name.ToLowerInvariant();
            return lower.Contains("9800x3d") || lower.Contains("9850x3d");
        }
    }

    /// <summary>Per-core temps, usage, and frequency for UI: "C0:45°C (23% @ 4.50 GHz) …". Falls back gracefully when usage/freq missing.</summary>
    public string CoreTempsDisplay
    {
        get
        {
            var temps = Summary.Metrics.CoreTempsCelsius ?? Array.Empty<float>();
            var usage = Summary.Metrics.CoreUsagePercent ?? Array.Empty<float>();
            var freqsMhz = Summary.Metrics.CoreFreqMHz ?? Array.Empty<float>();

            // Determine how many cores we can show based on any of the three sources.
            var coreCount = Math.Max(Math.Max(temps.Count, usage.Count), freqsMhz.Count);
            if (coreCount == 0) return "—";

            bool showTemps = IsPerCoreTempsSupportedCpu;

            var parts = new List<string>(coreCount);
            for (int i = 0; i < coreCount; i++)
            {
                bool hasTemp = showTemps && i < temps.Count;
                float temp = hasTemp ? temps[i] : 0f;
                string part;
                bool hasUsage = i < usage.Count;
                bool hasFreq = i < freqsMhz.Count && freqsMhz[i] > 0;
                if (hasTemp && hasUsage && hasFreq)
                {
                    var u = usage[i];
                    var ghz = freqsMhz[i] / 1000f;
                    part = $"C{i}:{temp:F0}°C ({u:F0}% @ {ghz:F2} GHz)";
                }
                else if (hasTemp && hasUsage)
                {
                    var u = usage[i];
                    part = $"C{i}:{temp:F0}°C ({u:F0}%)";
                }
                else if (hasTemp && hasFreq)
                {
                    var ghz = freqsMhz[i] / 1000f;
                    part = $"C{i}:{temp:F0}°C @ {ghz:F2} GHz";
                }
                else if (hasUsage && hasFreq)
                {
                    var u = usage[i];
                    var ghz = freqsMhz[i] / 1000f;
                    part = $"C{i}:{u:F0}% @ {ghz:F2} GHz";
                }
                else if (hasUsage)
                {
                    var u = usage[i];
                    part = $"C{i}:{u:F0}%";
                }
                else if (hasFreq)
                {
                    var ghz = freqsMhz[i] / 1000f;
                    part = $"C{i}:{ghz:F2} GHz";
                }
                else
                {
                    // Nothing meaningful for this core; skip.
                    continue;
                }
                parts.Add(part);
            }
            // Show each core on its own row for clearer readability.
            return parts.Count == 0 ? "—" : string.Join(Environment.NewLine, parts);
        }
    }

    /// <summary>SPD temp for the currently selected DIMM, or "—".</summary>
    public string SelectedSpdTempDisplay
    {
        get
        {
            var temps = Summary.Metrics.SpdTempsCelsius ?? Array.Empty<float>();
            if (temps.Count == 0 || _cachedModules.Count == 0) return "—";
            var idx = _selectedModuleIndex;
            if (idx < 0 || idx >= temps.Count) return "—";
            return $"{temps[idx]:F0}°C";
        }
    }

    /// <summary>CCD1 / Die temps from Tccd1/Tdie (PM table / k10temp / zenpower) plus optional CCD2. "CCD1/Die: 42/45  CCD2:40 °C" or similar, or "—".</summary>
    public string TctlTccdDisplay
    {
        get
        {
            var m = Summary.Metrics;
            if (!m.Tccd1Celsius.HasValue && !m.TdieCelsius.HasValue && !m.Tccd2Celsius.HasValue) return "—";
            var parts = new List<string>();
            // First part: CCD1/Die combined when both present.
            if (m.Tccd1Celsius.HasValue && m.TdieCelsius.HasValue)
                parts.Add($"CCD1/Die:{m.Tccd1Celsius.Value:F0}/{m.TdieCelsius.Value:F0}");
            else if (m.Tccd1Celsius.HasValue)
                parts.Add($"CCD1:{m.Tccd1Celsius.Value:F0}");
            else if (m.TdieCelsius.HasValue)
                parts.Add($"Die:{m.TdieCelsius.Value:F0}");

            if (m.Tccd2Celsius.HasValue) parts.Add($"CCD2:{m.Tccd2Celsius.Value:F0}");
            return string.Join("  ", parts) + " °C";
        }
    }

    /// <summary>Fans only (excludes Pump), one line per fan for UI.</summary>
    public IReadOnlyList<string> FanDisplayLines =>
        Summary.Fans?.Where(f => f.Label != "Pump").Select(f => $"{f.Label}: {f.Rpm} RPM").ToList() ?? new List<string>();

    /// <summary>Pump RPM or "—" if no pump.</summary>
    public string PumpDisplay =>
        Summary.Fans?.FirstOrDefault(f => f.Label == "Pump") is { } p ? $"{p.Rpm} RPM" : "—";
    public string AppVersion { get; } =
        Assembly.GetExecutingAssembly()
            .GetName()
            .Version?
            .ToString(3) // e.g. 1.0.1 instead of 1.0.1.0
        ?? string.Empty;

    public MainViewModel(IHardwareBackend backend, int refreshIntervalMs = 1000)
    {
        _backend = backend;
        if (_backend.IsSupported())
        {
            Summary = _backend.ReadSummary();
        }

        _timer = new Timer(refreshIntervalMs);
        _timer.Elapsed += (_, _) => Refresh();
        _timer.AutoReset = true;
        _timer.Start();
    }

    public void Refresh()
    {
        if (_backend.IsSupported())
        {
            try
            {
                Summary = _backend.ReadSummary();
            }
            catch
            {
                // ignore refresh errors for now
            }
        }
    }

    private void OnPropertyChanged([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

