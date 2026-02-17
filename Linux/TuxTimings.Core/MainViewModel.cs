using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
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
    public SystemSummary Summary
    {
        get => _summary;
        private set
        {
            _summary = value;
            var modules = _summary.Modules ?? Array.Empty<MemoryModule>();
            if (modules.Count == 0)
                _selectedModuleIndex = -1;
            else if (_selectedModuleIndex < 0 || _selectedModuleIndex >= modules.Count)
                _selectedModuleIndex = 0;
            OnPropertyChanged();
            OnPropertyChanged(nameof(Modules));
            OnPropertyChanged(nameof(SelectedModuleIndex));
            OnPropertyChanged(nameof(SelectedModuleCapacity));
            OnPropertyChanged(nameof(SelectedModuleManufacturer));
            OnPropertyChanged(nameof(SelectedModulePartNumber));
            OnPropertyChanged(nameof(SelectedModuleSerialNumber));
            OnPropertyChanged(nameof(SelectedModuleRankDisplay));
            OnPropertyChanged(nameof(SelectedPhyRdl));
            UpdateSelectedPhyRdlDisplay();
            OnPropertyChanged(nameof(CoreTempsDisplay));
            OnPropertyChanged(nameof(TctlTccdDisplay));
            OnPropertyChanged(nameof(FanDisplayLines));
            OnPropertyChanged(nameof(PumpDisplay));
            OnPropertyChanged(nameof(BclkMHz));
        }
    }

    /// <summary>Installed DIMMs from dmidecode -t 17 for dropdown.</summary>
    public IReadOnlyList<MemoryModule> Modules => Summary.Modules ?? Array.Empty<MemoryModule>();

    private int _selectedModuleIndex = -1;
    private string _selectedPhyRdlDisplay = "—";
    /// <summary>Selected DIMM index in Modules; -1 when none. Bound to ComboBox SelectedIndex.</summary>
    public int SelectedModuleIndex
    {
        get => _selectedModuleIndex;
        set
        {
            var modules = Summary.Modules ?? Array.Empty<MemoryModule>();
            var clamped = value < 0 ? -1 : (value >= modules.Count ? (modules.Count > 0 ? modules.Count - 1 : -1) : value);
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
            var modules = Summary.Modules ?? Array.Empty<MemoryModule>();
            if (_selectedModuleIndex < 0 || _selectedModuleIndex >= modules.Count) return null;
            return modules[_selectedModuleIndex];
        }
    }

    /// <summary>BCLK in MHz (CoreClockMHz / 10 for display).</summary>
    public float BclkMHz => Summary.Metrics.CoreClockMHz / 10f;

    /// <summary>Per-core temps for UI: "C0:17 C1:15 … °C".</summary>
    public string CoreTempsDisplay
    {
        get
        {
            var list = Summary.Metrics.CoreTempsCelsius;
            if (list == null || list.Count == 0) return "—";
            return string.Join(" ", list.Select((t, i) => $"C{i}:{t:F0}")) + " °C";
        }
    }

    /// <summary>Tctl, Tccd1, Tccd2 from k10temp. Tccd2 only when k10temp exposes it. "Tctl: 45  Tccd1: 42  Tccd2: 40 °C" or "—".</summary>
    public string TctlTccdDisplay
    {
        get
        {
            var m = Summary.Metrics;
            if (!m.TctlCelsius.HasValue && !m.Tccd1Celsius.HasValue && !m.Tccd2Celsius.HasValue) return "—";
            var parts = new List<string>();
            if (m.TctlCelsius.HasValue) parts.Add($"Tctl:{m.TctlCelsius.Value:F0}");
            if (m.Tccd1Celsius.HasValue) parts.Add($"Tccd1:{m.Tccd1Celsius.Value:F0}");
            if (m.Tccd2Celsius.HasValue) parts.Add($"Tccd2:{m.Tccd2Celsius.Value:F0}");
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
        Assembly.GetExecutingAssembly().GetName().Version?.ToString() ?? string.Empty;

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

