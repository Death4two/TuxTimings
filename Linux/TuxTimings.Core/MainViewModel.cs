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
        }
    }

    /// <summary>Per-core temps for UI: "C0: 17  C1: 15  … °C".</summary>
    public string CoreTempsDisplay
    {
        get
        {
            var list = Summary.Metrics.CoreTempsCelsius;
            if (list == null || list.Count == 0) return "—";
            return string.Join("  ", list.Select((t, i) => $"C{i}: {t:F0}")) + " °C";
        }
    }

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

