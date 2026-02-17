using System;
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
            OnPropertyChanged();
            OnPropertyChanged(nameof(CoreTempsDisplay));
            OnPropertyChanged(nameof(TctlTccdDisplay));
            OnPropertyChanged(nameof(FanDisplayLines));
            OnPropertyChanged(nameof(PumpDisplay));
            OnPropertyChanged(nameof(BclkMHz));
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

