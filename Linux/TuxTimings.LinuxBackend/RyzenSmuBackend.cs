using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using TuxTimings.Core;
using System.Text;

namespace TuxTimings.LinuxBackend;

public sealed class RyzenSmuBackend : IHardwareBackend
{
    private const string BasePath = "/sys/kernel/ryzen_smu_drv";
    // Previous per-CPU times from /proc/stat for usage deltas (key = logical CPU index).
    // Values are (IdleJiffies, TotalJiffies).
    private readonly Dictionary<int, (ulong Idle, ulong Total)> _prevCpuTimes = new();

    // Cached static data: these don't change between refreshes so we only read them once.
    private IReadOnlyList<MemoryModule>? _cachedModules;
    private (string ProcessorName, string PartNumbers, string MotherboardProductName, string BiosVersion, string BiosReleaseDate)? _cachedDmidecode;
    private string? _cachedAgesaVersion;

    /// <summary>
    /// Fallback: use Python script (same logic as dump_pm_voltages.py) to parse PM table.
    /// Python reads sysfs successfully when C# may throw; script outputs key=value.
    /// </summary>
    private static SmuMetrics? TryReadPmTableViaPython()
    {
        var scriptPaths = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "scripts", "parse_pm_table.py"),
            Path.Combine(AppContext.BaseDirectory, "..", "scripts", "parse_pm_table.py"),
            Path.Combine(Directory.GetCurrentDirectory(), "scripts", "parse_pm_table.py"),
            Path.Combine(Directory.GetCurrentDirectory(), "scripts", "..", "Linux", "scripts", "parse_pm_table.py"),
        };

        string? scriptPath = null;
        foreach (var p in scriptPaths)
        {
            var full = Path.GetFullPath(p);
            if (File.Exists(full))
            {
                scriptPath = full;
                break;
            }
        }
        if (scriptPath is null) return null;

        try
        {
            using var proc = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = "python3",
                    Arguments = $"\"{scriptPath}\"",
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                    CreateNoWindow = true
                }
            };
            proc.Start();
            var stdout = proc.StandardOutput.ReadToEnd();
            proc.WaitForExit(3000);
            if (proc.ExitCode != 0) return null;

            var values = new Dictionary<string, float>(StringComparer.OrdinalIgnoreCase);
            foreach (var line in stdout.Split('\n', StringSplitOptions.RemoveEmptyEntries))
            {
                var eq = line.IndexOf('=');
                if (eq <= 0) continue;
                var key = line[..eq].Trim();
                if (float.TryParse(line[(eq + 1)..].Trim(), out var v))
                    values[key] = v;
            }

            if (values.Count == 0) return null;

            // This Python fallback is only used when direct PM-table parsing fails.
            // We currently only surface aggregate clocks/voltages/temps here.
            return new SmuMetrics
            {
                FclkMHz = values.GetValueOrDefault("FCLK"),
                UclkMHz = values.GetValueOrDefault("UCLK"),
                MclkMHz = values.GetValueOrDefault("MCLK"),
                Vsoc = values.GetValueOrDefault("VSOC"),
                Vddp = values.GetValueOrDefault("VDDP"),
                VddgIod = values.GetValueOrDefault("VDDG_IOD"),
                VddgCcd = values.GetValueOrDefault("VDDG_CCD"),
                VddMisc = values.GetValueOrDefault("VDD_MISC"),
                Vcore = values.GetValueOrDefault("VCORE"),
                CpuPackagePowerWatts = values.GetValueOrDefault("POWER"),
                CpuPptWatts = values.GetValueOrDefault("PPT"),
                CpuPackageCurrentAmps = values.GetValueOrDefault("CURRENT"),
                CpuTempCelsius = values.GetValueOrDefault("TEMP"),
                CoreTempsCelsius = Array.Empty<float>(),
                CoreClockMHz = values.GetValueOrDefault("CORE_MHZ"),
                // Tdie/Tctl/Tccd will be overlaid later from k10temp/zenpower.
                TdieCelsius = null,
                TctlCelsius = null,
                Tccd1Celsius = null,
                Tccd2Celsius = null
            };
        }
        catch
        {
            return null;
        }
    }
    private static bool _hasDumpedOnce;
    private static bool _hasDumpedDiagnostic;

    private static string GetDumpDirectory()
    {
        // When running with sudo, use /tmp so the real user can find the dump.
        var sudoUser = Environment.GetEnvironmentVariable("SUDO_USER");
        if (!string.IsNullOrEmpty(sudoUser))
        {
            return "/tmp/zentimings";
        }
        var cacheHome = Environment.GetEnvironmentVariable("XDG_CACHE_HOME");
        if (string.IsNullOrEmpty(cacheHome))
        {
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            cacheHome = Path.Combine(home, ".cache");
        }
        return Path.Combine(cacheHome, "zentimings");
    }

    private static void DumpPmTable(byte[] rawBytes, SmuMetrics metrics, int codenameIndex)
    {
        try
        {
            var dir = GetDumpDirectory();
            Directory.CreateDirectory(dir);

            var binPath = Path.Combine(dir, "pm_table_latest.bin");
            File.WriteAllBytes(binPath, rawBytes);

            var summaryPath = Path.Combine(dir, "pm_table_summary.txt");
            var lines = new[]
            {
                $"Dumped at: {DateTime.Now:yyyy-MM-dd HH:mm:ss}",
                $"Dump path: {dir}",
                $"Codename index: {codenameIndex}",
                $"Raw size: {rawBytes.Length} bytes ({rawBytes.Length / 4} floats)",
                "",
                "Parsed metrics (these are what the UI displays):",
                $"  FCLK:     {metrics.FclkMHz:F0} MHz",
                $"  UCLK:     {metrics.UclkMHz:F0} MHz",
                $"  MCLK:     {metrics.MclkMHz:F0} MHz",
                $"  VSOC:     {metrics.Vsoc:F3} V",
                $"  VDDP:     {metrics.Vddp:F3} V",
                $"  VDDG IOD: {metrics.VddgIod:F3} V",
                $"  VDDG CCD: {metrics.VddgCcd:F3} V",
                $"  VDD MISC: {metrics.VddMisc:F3} V",
                $"  CPU VDDIO: {metrics.CpuVddio:F3} V",
                $"  MEM VDD:  {metrics.MemVdd:F3} V",
                $"  MEM VDDQ: {metrics.MemVddq:F3} V",
                $"  MEM VPP:  {metrics.MemVpp:F3} V",
                $"  VCORE:    {metrics.Vcore:F4} V",
                $"  PPT:      {metrics.CpuPptWatts:F1} W",
                $"  Raw[271]: {(rawBytes.Length >= 272 * 4 ? BitConverter.ToSingle(rawBytes, 271 * 4):0):F6}"
            };
            File.WriteAllText(summaryPath, string.Join(Environment.NewLine, lines));
        }
        catch
        {
            // ignore dump failures
        }
    }

    /// <summary>Dumps a diagnostic file when the PM table is missing or read fails.</summary>
    private static void DumpDiagnostic(string reason, int codenameIndex)
    {
        try
        {
            var dir = GetDumpDirectory();
            Directory.CreateDirectory(dir);
            var path = Path.Combine(dir, "pm_table_diagnostic.txt");
            var pmTablePath = Path.Combine(BasePath, "pm_table");
            var pmVersionPath = Path.Combine(BasePath, "pm_table_version");
            var content = $@"Dumped at: {DateTime.Now:yyyy-MM-dd HH:mm:ss}
Dump path: {dir}
Reason: {reason}
Codename index: {codenameIndex}
pm_table exists: {File.Exists(pmTablePath)}
pm_table_version exists: {File.Exists(pmVersionPath)}

If pm_table is missing, the ryzen_smu driver may not support your PM table version.
Check dmesg for 'Unknown PM table version' or 'Failed to probe the PM table'.
";
            File.WriteAllText(path, content);
        }
        catch { }
    }

    public bool IsSupported()
    {
        try
        {
            return Directory.Exists(BasePath)
                   && File.Exists(Path.Combine(BasePath, "version"));
        }
        catch
        {
            return false;
        }
    }

    public SystemSummary ReadSummary()
    {
        // Cache static data (dmidecode, modules, AGESA) — only read once.
        _cachedDmidecode ??= ReadDmidecode();
        var (processorName, partNumbers, motherboardProductName, biosVersion, biosReleaseDate) = _cachedDmidecode.Value;

        var codenameIndex = ReadCodenameIndex();
        var pmVersion = ReadUInt32("pm_table_version");
        var pmVersionDisplay = pmVersion == 0 ? string.Empty : $"PM table 0x{pmVersion:X8}";

        var cpu = new CpuInfoModel
        {
            Name = "AMD Ryzen (from ryzen_smu)",
            ProcessorName = processorName,
            CodeName = MapCodename(codenameIndex),
            SmuVersion = ReadString("version"),
            PmTableVersion = pmVersionDisplay
        };

        // Dynamic data: re-read every refresh.
        var metrics = ReadMetrics(codenameIndex);
        var dramTimings = ReadDramTimingsForCodename(codenameIndex);
        var (memFreqMHz, memType) = GetMemoryConfigForCodename(codenameIndex, dramTimings);

        var memory = new MemoryConfigModel
        {
            Frequency = memFreqMHz,
            Type = memType,
            TotalCapacity = ReadTotalMemory(),
            PartNumber = partNumbers
        };

        _cachedAgesaVersion ??= ReadAgesaVersion();
        var boardInfo = new BoardInfoModel
        {
            MotherboardProductName = motherboardProductName,
            BiosVersion = biosVersion,
            BiosReleaseDate = biosReleaseDate,
            AgesaVersion = _cachedAgesaVersion
        };

        // Modules are static — only read once.
        _cachedModules ??= ReadMemoryModules();
        var fans = ReadHwmonFans();

        return new SystemSummary
        {
            Cpu = cpu,
            Memory = memory,
            BoardInfo = boardInfo,
            Modules = _cachedModules,
            Metrics = metrics,
            DramTimings = dramTimings,
            Fans = fans
        };
    }

    /// <summary>
    /// Read fan1–fan7 from Nuvoton Super I/O hwmon devices (e.g. nct6799, nct6798, nct6775, etc.); fan7 = Pump; exclude 0 RPM.
    /// Matches any hwmon "name" that looks like an NCT67xx/NCT67xx-family or contains "nuvoton".
    /// </summary>
    private static IReadOnlyList<FanReading> ReadHwmonFans()
    {
        const string hwmonRoot = "/sys/class/hwmon";
        if (!Directory.Exists(hwmonRoot)) return Array.Empty<FanReading>();

        // Try each Nuvoton hwmon device until we find one with non-zero fan speeds.
        var result = new List<FanReading>();

        foreach (var dir in Directory.GetDirectories(hwmonRoot))
        {
            try
            {
                var namePath = Path.Combine(dir, "name");
                if (!File.Exists(namePath)) continue;
                var name = File.ReadAllText(namePath).Trim();

                // Typical names: "nct6799", "nct6798", "nct6775", etc., sometimes with suffixes.
                bool isNuvoton = name.StartsWith("nct6", StringComparison.OrdinalIgnoreCase)
                                 || name.Contains("nuvoton", StringComparison.OrdinalIgnoreCase);
                if (!isNuvoton) continue;

                var list = new List<FanReading>();
                for (int i = 1; i <= 7; i++)
                {
                    var path = Path.Combine(dir, $"fan{i}_input");
                    if (!File.Exists(path)) continue;
                    if (!int.TryParse(File.ReadAllText(path).Trim(), out var rpm) || rpm <= 0) continue;
                    var label = i == 7 ? "Pump" : $"Fan{i}";
                    list.Add(new FanReading(label, rpm));
                }

                if (list.Count > 0)
                {
                    result = list;
                    break;
                }
            }
            catch
            {
                // Ignore individual hwmon errors and keep searching.
            }
        }

        return result;
    }
    private static string ReadString(string fileName)
    {
        var path = Path.Combine(BasePath, fileName);
        if (!File.Exists(path)) return string.Empty;
        return File.ReadAllText(path).Trim();
    }

    /// <summary>
    /// Runs dmidecode to get processor name, RAM part numbers, motherboard, and BIOS info.
    /// Called before displaying the main window. Requires root for full access.
    /// </summary>
    private static (string ProcessorName, string PartNumbers, string MotherboardProductName, string BiosVersion, string BiosReleaseDate) ReadDmidecode()
    {
        var processorName = string.Empty;
        var partNumbers = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var motherboardProductName = string.Empty;
        var biosVersion = string.Empty;
        var biosReleaseDate = string.Empty;

        try
        {
            processorName = RunDmidecodeAndParse("processor", ParseProcessor).Trim();
            var memResult = RunDmidecodeAndParse("memory", ParseMemory);
            foreach (var pn in memResult.Split('\n', StringSplitOptions.RemoveEmptyEntries))
            {
                if (!string.IsNullOrWhiteSpace(pn))
                    partNumbers.Add(pn.Trim());
            }

            motherboardProductName = RunDmidecodeString("baseboard-product-name");
            biosVersion = RunDmidecodeString("bios-version");
            biosReleaseDate = RunDmidecodeString("bios-release-date");
        }
        catch
        {
            // dmidecode may require root; ignore
        }

        return (processorName, string.Join(", ", partNumbers), motherboardProductName, biosVersion, biosReleaseDate);
    }

    /// <summary>Runs dmidecode -s &lt;keyword&gt; and returns trimmed stdout (e.g. baseboard-product-name, bios-version).</summary>
    private static string RunDmidecodeString(string keyword)
    {
        try
        {
            using var proc = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = "dmidecode",
                    Arguments = $"-s {keyword}",
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                    CreateNoWindow = true
                }
            };
            proc.Start();
            var stdout = proc.StandardOutput.ReadToEnd();
            proc.WaitForExit(5000);
            return proc.ExitCode == 0 ? stdout.Trim() : string.Empty;
        }
        catch
        {
            return string.Empty;
        }
    }

    private static string RunDmidecodeAndParse(string type, Func<string, string> parser)
    {
        try
        {
            using var proc = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = "dmidecode",
                    Arguments = $"-t {type}",
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                    CreateNoWindow = true
                }
            };
            proc.Start();
            var stdout = proc.StandardOutput.ReadToEnd();
            proc.WaitForExit(5000);
            return proc.ExitCode == 0 ? parser(stdout) : string.Empty;
        }
        catch
        {
            return string.Empty;
        }
    }

    private static string ParseProcessor(string stdout)
    {
        var inProc = false;
        foreach (var line in stdout.Split('\n'))
        {
            if (line.Contains("Processor Information", StringComparison.OrdinalIgnoreCase))
            {
                inProc = true;
                continue;
            }
            if (inProc && line.TrimStart().StartsWith("Version:", StringComparison.OrdinalIgnoreCase))
            {
                var colon = line.IndexOf(':', StringComparison.Ordinal);
                return colon >= 0 ? line[(colon + 1)..].Trim() : string.Empty;
            }
            if (inProc && line.StartsWith("\t", StringComparison.Ordinal) == false && line.Trim().Length > 0)
                break;
        }
        return string.Empty;
    }

    private static string ParseMemory(string stdout)
    {
        var partNumbers = new List<string>();
        var inMem = false;
        foreach (var line in stdout.Split('\n'))
        {
            if (line.Contains("Memory Device", StringComparison.OrdinalIgnoreCase))
            {
                inMem = true;
                continue;
            }
            if (inMem && line.TrimStart().StartsWith("Part Number:", StringComparison.OrdinalIgnoreCase))
            {
                var colon = line.IndexOf(':', StringComparison.Ordinal);
                var pn = colon >= 0 ? line[(colon + 1)..].Trim() : string.Empty;
                if (!string.IsNullOrEmpty(pn) && pn != "Unknown" && pn != "NO DIMM")
                    partNumbers.Add(pn);
            }
            if (inMem && line.StartsWith("\t", StringComparison.Ordinal) == false && line.Trim().Length > 0)
                inMem = false;
        }
        return string.Join("\n", partNumbers);
    }

    /// <summary>
    /// Parse dmidecode -t memory to build a list of populated MemoryModule objects.
    /// Each "Memory Device" block that has a non-zero Size is considered a populated DIMM slot.
    /// </summary>
    private static IReadOnlyList<MemoryModule> ReadMemoryModules()
    {
        try
        {
            using var proc = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = "dmidecode",
                    Arguments = "-t memory",
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                    CreateNoWindow = true
                }
            };
            proc.Start();
            var stdout = proc.StandardOutput.ReadToEnd();
            proc.WaitForExit(5000);
            if (proc.ExitCode != 0) return Array.Empty<MemoryModule>();

            var modules = new List<MemoryModule>();
            string bankLabel = "", locator = "", manufacturer = "", partNumber = "", serialNumber = "";
            ulong capacityBytes = 0;
            uint clockMHz = 0;
            int rank = 0;
            bool inDevice = false;

            foreach (var rawLine in stdout.Split('\n'))
            {
                var line = rawLine.TrimEnd();

                if (line.Contains("Memory Device", StringComparison.OrdinalIgnoreCase) && !line.Contains("Mapped", StringComparison.OrdinalIgnoreCase))
                {
                    // Save previous device if populated
                    if (inDevice && capacityBytes > 0)
                    {
                        modules.Add(BuildModule(bankLabel, locator, manufacturer, partNumber, serialNumber, capacityBytes, clockMHz, rank));
                    }
                    // Reset for new device
                    inDevice = true;
                    bankLabel = locator = manufacturer = partNumber = serialNumber = "";
                    capacityBytes = 0;
                    clockMHz = 0;
                    rank = 0;
                    continue;
                }

                if (!inDevice) continue;

                var trimmed = line.TrimStart();
                var colonIdx = trimmed.IndexOf(':', StringComparison.Ordinal);
                if (colonIdx < 0) continue;
                var key = trimmed[..colonIdx].Trim();
                var val = trimmed[(colonIdx + 1)..].Trim();

                switch (key)
                {
                    case "Size":
                        capacityBytes = ParseCapacity(val);
                        break;
                    case "Locator":
                        locator = val;
                        break;
                    case "Bank Locator":
                        bankLabel = val;
                        break;
                    case "Manufacturer":
                        if (val != "Unknown" && val != "Not Specified")
                            manufacturer = val;
                        break;
                    case "Part Number":
                        if (val != "Unknown" && val != "NO DIMM" && val != "Not Specified")
                            partNumber = val;
                        break;
                    case "Serial Number":
                        if (val != "Unknown" && val != "Not Specified" && val != "00000000")
                            serialNumber = val;
                        break;
                    case "Rank":
                        int.TryParse(val, out rank);
                        break;
                    case "Configured Memory Speed" or "Configured Clock Speed":
                        // e.g. "3300 MT/s" or "3300 MHz"
                        var parts = val.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                        if (parts.Length > 0 && uint.TryParse(parts[0], out var mhz))
                            clockMHz = mhz;
                        break;
                }
            }

            // Don't forget the last device
            if (inDevice && capacityBytes > 0)
            {
                modules.Add(BuildModule(bankLabel, locator, manufacturer, partNumber, serialNumber, capacityBytes, clockMHz, rank));
            }

            return modules;
        }
        catch
        {
            return Array.Empty<MemoryModule>();
        }
    }

    private static MemoryModule BuildModule(string bankLabel, string locator, string manufacturer,
        string partNumber, string serialNumber, ulong capacityBytes, uint clockMHz, int rank)
    {
        return new MemoryModule
        {
            BankLabel = bankLabel,
            DeviceLocator = locator,
            Manufacturer = manufacturer,
            PartNumber = partNumber,
            SerialNumber = serialNumber,
            CapacityBytes = capacityBytes,
            ClockSpeedMHz = clockMHz,
            Rank = rank switch
            {
                4 => MemRank.QR,
                2 => MemRank.DR,
                _ => MemRank.SR
            }
        };
    }

    /// <summary>Parse dmidecode Size field: "16 GB", "8192 MB", "No Module Installed", etc.</summary>
    private static ulong ParseCapacity(string val)
    {
        if (string.IsNullOrEmpty(val)) return 0;
        if (val.Contains("No Module", StringComparison.OrdinalIgnoreCase)
            || val.Contains("Not Installed", StringComparison.OrdinalIgnoreCase))
            return 0;

        var parts = val.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length < 2 || !ulong.TryParse(parts[0], out var size)) return 0;

        var unit = parts[1].ToUpperInvariant();
        return unit switch
        {
            "GB" or "GIB" => size * 1024UL * 1024UL * 1024UL,
            "MB" or "MIB" => size * 1024UL * 1024UL,
            "KB" or "KIB" => size * 1024UL,
            _ => size
        };
    }

    private SmuMetrics ReadMetrics(int codenameIndex)
    {
        // Best-effort: if pm_table is present, read a few floats from it.
        var pmTablePath = Path.Combine(BasePath, "pm_table");
        SmuMetrics metrics;

        if (!File.Exists(pmTablePath))
        {
            if (!_hasDumpedDiagnostic)
            {
                DumpDiagnostic("pm_table file does not exist", codenameIndex);
                _hasDumpedDiagnostic = true;
            }
            metrics = new SmuMetrics();
        }
        else
        {
            try
            {
                // Sysfs can occasionally return stale/empty data on first read; retry once if needed.
                byte[] bytes = File.ReadAllBytes(pmTablePath);
                if (bytes.Length < 4)
                {
                    metrics = new SmuMetrics();
                }
                else
                {
                    var count = bytes.Length / 4;
                    var floats = new float[count];
                    Buffer.BlockCopy(bytes, 0, floats, 0, bytes.Length);

                    SmuMetrics baseMetrics;

                    if (codenameIndex == 23)
                    {
                        var pmVersion = ReadUInt32("pm_table_version");
                        baseMetrics = ReadGraniteRidgeMetrics(floats, pmVersion);

                        // Retry once if we got zeros but table has data (sysfs timing)
                        if (baseMetrics.FclkMHz == 0 && baseMetrics.Vsoc == 0 && count >= 84)
                        {
                            bytes = File.ReadAllBytes(pmTablePath);
                            if (bytes.Length >= 4)
                            {
                                count = bytes.Length / 4;
                                Buffer.BlockCopy(bytes, 0, floats, 0, Math.Min(bytes.Length, floats.Length * 4));
                                baseMetrics = ReadGraniteRidgeMetrics(floats, pmVersion);
                            }
                        }

                        // Backup path: if Vcore is still 0 after direct PM-table parsing,
                        // overlay Vcore from parse_pm_table.py (VCORE key) while preserving other fields.
                        if (baseMetrics.Vcore == 0f)
                        {
                            var pyMetrics = TryReadPmTableViaPython();
                            if (pyMetrics is { } m && m.Vcore != 0f)
                                baseMetrics = WithVcore(baseMetrics, m.Vcore);
                        }
                    }
                    else
                    {
                        // Generic fallback: try plausible indices for power/core/current/temp (same logic as parse_pm_table.py).
                        float cpuPower = TryPlausiblePower(floats);
                        float coreClock = TryPlausibleCoreClock(floats);
                        float packageCurrent = TryPlausibleCurrent(floats);
                        float cpuTemp = TryPlausibleTemp(floats);
                        float memClock = count > 3 ? floats[3] : 0;
                        if (coreClock == 0 && count > 2) { float v = floats[2]; if (v >= 0.5f && v <= 6.5f) coreClock = v * 1000f; else if (v >= 500f && v <= 6500f) coreClock = v; }
                        if (coreClock == 0) coreClock = TryReadCpufreqMHz();

                        var (pptW, coreTemps, tdieC, coreClocksGhz) = ReadKnownPmIndices(floats);
                        if (coreClocksGhz.Length > 0)
                        {
                            float maxGhz = coreClocksGhz[0];
                            for (int i = 1; i < coreClocksGhz.Length; i++)
                                if (coreClocksGhz[i] > maxGhz) maxGhz = coreClocksGhz[i];
                            if (maxGhz >= 0.5f && maxGhz <= 6.5f) coreClock = maxGhz * 1000f;
                        }
                        if (tdieC > 0) cpuTemp = tdieC;
                        else if (cpuTemp == 0) cpuTemp = TryPlausibleTemp(floats);

                        baseMetrics = new SmuMetrics
                        {
                            CpuPackagePowerWatts = cpuPower,
                            CpuPptWatts = pptW,
                            CpuPackageCurrentAmps = packageCurrent,
                            Vcore = 0f,
                            CpuTempCelsius = cpuTemp,
                            CoreTempsCelsius = coreTemps,
                            CoreClockMHz = coreClock,
                            CoreClocksGhz = coreClocksGhz,
                            MemoryClockMHz = memClock,
                            // Tdie from PM table if available.
                            TdieCelsius = tdieC > 0 ? tdieC : null,
                            TctlCelsius = null,
                            Tccd1Celsius = null,
                            Tccd2Celsius = null
                        };
                    }

                    // Overlay with zenpower3 hwmon values if available.
                    metrics = ApplyZenpowerOverrides(baseMetrics);
                    // If PM table did not provide per-core temps, use k10temp CCD temps (temp3–temp10 = Tccd1–Tccd8).
                    metrics = ApplyK10TempCoreTempFallback(metrics);
                    // Tctl, Tccd1, Tccd2 from k10temp (temp1, temp3, temp4); Tccd2 only when exposed.
                    metrics = ApplyK10TempTctlTccdOverlay(metrics);

                    // Dump PM table and parsed values on first successful read (at startup).
                    if (!_hasDumpedOnce)
                    {
                        DumpPmTable(bytes, metrics, codenameIndex);
                        _hasDumpedOnce = true;
                    }
                }
            }
            catch
            {
                if (!_hasDumpedDiagnostic)
                {
                    DumpDiagnostic("Exception while reading pm_table", codenameIndex);
                    _hasDumpedDiagnostic = true;
                }
                // Fallback: Python script (same as dump_pm_voltages.py) can read sysfs when C# throws
                if (codenameIndex == 23)
                {
                    var pyMetrics = TryReadPmTableViaPython();
                    if (pyMetrics is { } m && (m.FclkMHz > 0 || m.Vsoc > 0))
                    {
                        metrics = ApplyZenpowerOverrides(m);
                        metrics = ApplyK10TempCoreTempFallback(metrics);
                        metrics = ApplyK10TempTctlTccdOverlay(metrics);
                    }
                    else
                    {
                        metrics = new SmuMetrics();
                    }
                }
                else
                {
                    metrics = new SmuMetrics();
                }
            }
        }

        // Overlay SPD (DIMM) temperatures from spd5118 hwmon.
        metrics = ApplySpdTempsOverlay(metrics);
        // Always overlay per-core usage/frequency from /proc/stat + cpufreq on top of whatever metrics we have.
        metrics = ApplyProcStatCoreUsage(metrics);
        return metrics;
    }

    /// <summary>
    /// Select DRAM timings reader based on codename index (from ryzen_smu codename file).
    /// Granite Ridge (23) uses DDR5 reader; common desktop DDR4 families use generic DDR4 reader.
    /// </summary>
    private static DramTimingsModel ReadDramTimingsForCodename(int codenameIndex)
    {
        return codenameIndex switch
        {
            23 => ReadGraniteRidgeDdr5Timings(),   // Granite Ridge DDR5
            4 or 9 or 10 or 12 or 18 or 19 => ReadGenericDdr4Timings(), // Matisse, Summit, Pinnacle, Vermeer, Naples, Chagall (DDR4 desktop/HEDT)
            _ => new DramTimingsModel()
        };
    }

    /// <summary>
    /// Decide MemoryConfig frequency and type from codename + timing hint.
    /// </summary>
    private static (float FrequencyMHz, MemType Type) GetMemoryConfigForCodename(int codenameIndex, DramTimingsModel dram)
    {
        return codenameIndex switch
        {
            23 => (dram.FrequencyHintMHz, MemType.DDR5),
            4 or 9 or 10 or 12 or 18 or 19 => (dram.FrequencyHintMHz, MemType.DDR4),
            _ => (0f, MemType.Unknown)
        };
    }

    private static uint ReadSmn(uint address)
    {
        var path = Path.Combine(BasePath, "smn");
        if (!File.Exists(path))
        {
            throw new IOException("ryzen_smu smn interface not found.");
        }

        using var fs = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite);
        Span<byte> buf = stackalloc byte[4];

        // write address (little endian)
        BitConverter.TryWriteBytes(buf, address);
        fs.Write(buf);

        // read back value
        fs.Position = 0;
        fs.Read(buf);
        return BitConverter.ToUInt32(buf);
    }

    private static uint ReadUInt32(string fileName)
    {
        var path = Path.Combine(BasePath, fileName);
        try
        {
            if (!File.Exists(path)) return 0;
            // sysfs entries like pm_table_version can report a logical length
            // but allow fewer bytes to be read, so avoid File.ReadAllBytes
            // and instead read up to 4 bytes safely.
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            Span<byte> buf = stackalloc byte[4];
            var read = fs.Read(buf);
            if (read < 4) return 0;
            return BitConverter.ToUInt32(buf);
        }
        catch
        {
            return 0;
        }
    }

    private static string ReadTotalMemory()
    {
        try
        {
            var meminfo = "/proc/meminfo";
            if (!File.Exists(meminfo)) return string.Empty;

            foreach (var line in File.ReadAllLines(meminfo))
            {
                if (line.StartsWith("MemTotal:", StringComparison.Ordinal))
                {
                    var parts = line.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                    if (parts.Length >= 2 && ulong.TryParse(parts[1], out var kB))
                    {
                        double gib = kB / 1024.0 / 1024.0;
                        return $"{gib:F1} GiB";
                    }
                }
            }
        }
        catch
        {
            // ignore
        }
        return string.Empty;
    }

    private static int ReadCodenameIndex()
    {
        var raw = ReadString("codename");
        if (string.IsNullOrEmpty(raw)) return -1;

        return int.TryParse(raw, out var index) ? index : -1;
    }

    private static string MapCodename(int index)
    {
        return index switch
        {
            1 => "Colfax",
            2 => "Renoir",
            3 => "Picasso",
            4 => "Matisse",
            5 => "Threadripper",
            6 => "Castle Peak",
            7 => "Raven Ridge",
            8 => "Raven Ridge 2",
            9 => "Summit Ridge",
            10 => "Pinnacle Ridge",
            11 => "Rembrandt",
            12 => "Vermeer",
            13 => "Vangogh",
            14 => "Cezanne",
            15 => "Milan",
            16 => "Dali",
            17 => "Luciene",
            18 => "Naples",
            19 => "Chagall",
            20 => "Raphael",
            21 => "Phoenix",
            22 => "Strix Point",
            23 => "Granite Ridge",
            24 => "Hawk Point",
            25 => "Storm Peak",
            _ => index >= 0 ? $"Unknown ({index})" : string.Empty
        };
    }

    private static uint BitSlice(uint value, int hiBit, int loBit)
    {
        var width = hiBit - loBit + 1;
        if (width <= 0 || width > 32) return 0;
        uint mask = width == 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);
        return (value >> loBit) & mask;
    }

    private static SmuMetrics ReadGraniteRidgeMetrics(float[] pt, uint pmVersion)
    {
        // Granite Ridge PM table: use generic Zen5 desktop definition (0x000620)
        // as in ZenStates-Core PowerTable. Offsets are in bytes.
        const int offsetFclk = 0x11C;
        const int offsetUclk = 0x12C;
        const int offsetMclk = 0x13C;
        const int offsetVddcrSoc = 0x14C;
        const int offsetCldoVddp = 0x434;
        const int offsetCldoVddgIod = 0x40C;
        const int offsetCldoVddgCcd = 0x414;
        const int offsetVddMisc = 0xE8;
        const int offsetVcore = 0x43C; 

        static float Get(float[] table, int byteIndex)
        {
            if (byteIndex < 0) return 0;
            int idx = byteIndex / 4;
            return idx >= 0 && idx < table.Length ? table[idx] : 0;
        }

        float fclk = Get(pt, offsetFclk);
        float uclk = Get(pt, offsetUclk);
        float mclk = Get(pt, offsetMclk);
        float vsoc = Get(pt, offsetVddcrSoc);
        float vddp = Get(pt, offsetCldoVddp);
        float vddgIod = Get(pt, offsetCldoVddgIod);
        float vddgCcd = Get(pt, offsetCldoVddgCcd);
        float vddMisc = Get(pt, offsetVddMisc);
        float vcore = Get(pt, offsetVcore);

        // Package-level metrics from PM table (best-effort).
        float packagePower = TryPlausiblePower(pt);
        float packageCurrent = TryPlausibleCurrent(pt);
        float coreClock = TryPlausibleCoreClock(pt);
        if (coreClock == 0) coreClock = TryReadCpufreqMHz();

        var (pptW, coreTemps, tdieC, coreClocksGhz) = ReadKnownPmIndices(pt);
        if (coreClocksGhz.Length > 0)
        {
            float maxGhz = coreClocksGhz[0];
            for (int i = 1; i < coreClocksGhz.Length; i++)
                if (coreClocksGhz[i] > maxGhz) maxGhz = coreClocksGhz[i];
            if (maxGhz >= 0.5f && maxGhz <= 6.5f) coreClock = maxGhz * 1000f;
        }

        float cpuTemp = tdieC > 0 ? tdieC : TryPlausibleTemp(pt);
        return new SmuMetrics
        {
            CpuPackagePowerWatts = packagePower,
            CpuPptWatts = pptW,
            CpuPackageCurrentAmps = packageCurrent,
            Vcore = vcore,
            CpuTempCelsius = cpuTemp,
            CoreTempsCelsius = coreTemps,
            CoreClockMHz = coreClock,
            CoreClocksGhz = coreClocksGhz,
            FclkMHz = fclk,
            UclkMHz = uclk,
            MclkMHz = mclk,
            Vsoc = vsoc,
            Vddp = vddp,
            VddgIod = vddgIod,
            VddgCcd = vddgCcd,
            VddMisc = vddMisc,
            // Tdie will be overlaid later from PM table (ReadKnownPmIndices) or zenpower/k10temp.
            TdieCelsius = null,
            TctlCelsius = null,
            Tccd1Celsius = null,
            Tccd2Celsius = null
        };
    }

    /// <summary>Read current CPU frequency from Linux cpufreq (kHz → MHz). Returns 0 if unavailable.</summary>
    private static float TryReadCpufreqMHz()
    {
        try
        {
            for (int i = 0; i < 64; i++)
            {
                var path = $"/sys/devices/system/cpu/cpu{i}/cpufreq/scaling_cur_freq";
                if (!File.Exists(path)) continue;
                var s = File.ReadAllText(path).Trim();
                if (int.TryParse(s, out var khz) && khz > 0) return khz / 1000f;
            }
        }
        catch { }
        return 0;
    }

    /// <summary>Read known PM table indices: 3/26 = CPU PPT (W), 317–324 = core temps (°C), 448/449 = tdie (°C), 325–340 = core clocks (GHz).</summary>
    private static (float PptWatts, float[] CoreTemps, float TdieCelsius, float[] CoreClocksGhz) ReadKnownPmIndices(float[] pt)
    {
        float ppt = 0;
        if (pt != null && pt.Length > 26)
        {
            float v3 = pt[3], v26 = pt[26];
            if (v3 >= 1f && v3 <= 400f) ppt = v3;
            else if (v26 >= 1f && v26 <= 400f) ppt = v26;
        }

        float[] coreTemps = Array.Empty<float>();
        if (pt != null && pt.Length > 324)
        {
            coreTemps = new float[8];
            for (int i = 0; i < 8; i++)
                coreTemps[i] = pt[317 + i];
        }

        float tdie = 0;
        if (pt != null && pt.Length > 449)
        {
            float a = pt[448], b = pt[449];
            if (a >= 1f && a <= 150f) tdie = a;
            else if (b >= 1f && b <= 150f) tdie = b;
            else if (a > 0 && b > 0) tdie = (a + b) * 0.5f;
        }

        float[] coreClocksGhz = Array.Empty<float>();
        if (pt != null && pt.Length > 340)
        {
            coreClocksGhz = new float[16];
            for (int i = 0; i < 16; i++)
                coreClocksGhz[i] = pt[325 + i];
        }

        return (ppt, coreTemps, tdie, coreClocksGhz);
    }

    /// <summary>Try candidate float indices for package power (W). Plausible: 0.5–400 W.</summary>
    private static float TryPlausiblePower(float[] pt)
    {
        if (pt == null || pt.Length == 0) return 0;
        int[] candidates = { 220, 187, 29, 42, 0, 1 }; // 220/187 from PM table dumps, then SOCKET_POWER, CPU_TELEMETRY_POWER, early
        foreach (var i in candidates)
        {
            if (i >= pt.Length) continue;
            float v = pt[i];
            if (v >= 0.5f && v <= 400f) return v;
        }
        return 0;
    }

    /// <summary>Try candidate indices for core frequency (MHz). Plausible: 500–6500 MHz.</summary>
    private static float TryPlausibleCoreClock(float[] pt)
    {
        if (pt == null || pt.Length == 0) return 0;
        int[] candidates = { 2, 48, 49 }; // early layout; FCLK_FREQ/FCLK_FREQ_EFF in 0x240903 (kHz? check)
        foreach (var i in candidates)
        {
            if (i >= pt.Length) continue;
            float v = pt[i];
            if (v >= 500f && v <= 6500f) return v;
            if (v >= 0.5f && v <= 6.5f) return v * 1000f; // might be in GHz
        }
        return 0;
    }

    /// <summary>Try candidate indices for package current (A). Plausible: 0.5–200 A.</summary>
    private static float TryPlausibleCurrent(float[] pt)
    {
        if (pt == null || pt.Length == 0) return 0;
        int[] candidates = { 41, 46, 3, 10, 11, 4 }; // CPU_TELEMETRY_CURRENT, SOC, TDC/EDC; 10,11 from dumps
        foreach (var i in candidates)
        {
            if (i >= pt.Length) continue;
            float v = pt[i];
            if (v >= 0.5f && v <= 200f) return v;
        }
        return 0;
    }

    /// <summary>Try candidate indices for CPU temp / tdie (°C). Plausible: 1–150 °C.</summary>
    private static float TryPlausibleTemp(float[] pt)
    {
        if (pt == null || pt.Length == 0) return 0;
        int[] candidates = { 1, 448, 449 }; // early temp, tdie
        foreach (var i in candidates)
        {
            if (i >= pt.Length) continue;
            float v = pt[i];
            if (v >= 1f && v <= 150f) return v;
        }
        return 0;
    }

    /// <summary>
    /// Helper to create a copy of SmuMetrics with a different Vcore value, preserving all other fields.
    /// Used to overlay Vcore from parse_pm_table.py when PM-table Vcore is 0.
    /// </summary>
    private static SmuMetrics WithVcore(SmuMetrics metrics, float vcore)
    {
        return new SmuMetrics
        {
            CpuPackagePowerWatts = metrics.CpuPackagePowerWatts,
            CpuPptWatts = metrics.CpuPptWatts,
            CpuPackageCurrentAmps = metrics.CpuPackageCurrentAmps,
            Vcore = vcore,
            CpuTempCelsius = metrics.CpuTempCelsius,
            CoreTempsCelsius = metrics.CoreTempsCelsius,
            CoreUsagePercent = metrics.CoreUsagePercent,
            CoreFreqMHz = metrics.CoreFreqMHz,
            TdieCelsius = metrics.TdieCelsius,
            TctlCelsius = metrics.TctlCelsius,
            Tccd1Celsius = metrics.Tccd1Celsius,
            Tccd2Celsius = metrics.Tccd2Celsius,
            CoreClockMHz = metrics.CoreClockMHz,
            CoreClocksGhz = metrics.CoreClocksGhz,
            MemoryClockMHz = metrics.MemoryClockMHz,
            FclkMHz = metrics.FclkMHz,
            UclkMHz = metrics.UclkMHz,
            MclkMHz = metrics.MclkMHz,
            Vsoc = metrics.Vsoc,
            Vddp = metrics.Vddp,
            VddgCcd = metrics.VddgCcd,
            VddgIod = metrics.VddgIod,
            VddMisc = metrics.VddMisc,
            CpuVddio = metrics.CpuVddio,
            MemVdd = metrics.MemVdd,
            MemVddq = metrics.MemVddq,
            MemVpp = metrics.MemVpp,
            SpdTempsCelsius = metrics.SpdTempsCelsius
        };
    }

    /// <summary>
    /// Best-effort AGESA version reader for Linux.
    /// Mirrors ZenStates-Core: first scan the legacy BIOS region (0xE0000–0xFFFFF) via /dev/mem for the
    /// "AGESA!V9" marker, then fall back to ACPI tables under /sys/firmware/acpi/tables.
    /// Returns empty string when no AGESA marker is found or access is not permitted.
    /// </summary>
    private static string ReadAgesaVersion()
    {
        try
        {
            // 1) Primary: scan legacy BIOS region like ZenStates-Core Cpu.GetAgesaVersion
            const long biosBase = 0xE0000;
            const int biosLength = 0x100000 - 0xE0000; // 0xFFFFF - 0xE0000 + 1
            const string devMem = "/dev/mem";
            if (File.Exists(devMem))
            {
                try
                {
                    using var fs = new FileStream(devMem, FileMode.Open, FileAccess.Read, FileShare.Read);
                    if (fs.Length >= biosBase + biosLength)
                    {
                        fs.Position = biosBase;
                        var buf = new byte[biosLength];
                        int read = fs.Read(buf, 0, buf.Length);
                        if (read > 0)
                        {
                            var v = ParseAgesaVersion(buf);
                            if (!string.IsNullOrEmpty(v))
                                return v;
                        }
                    }
                }
                catch
                {
                    // /dev/mem may be restricted; ignore and fall back to ACPI tables
                }
            }

            // 2) Fallback: search common ACPI tables exposed by the kernel.
            var candidatePaths = new[]
            {
                "/sys/firmware/acpi/tables/DSDT",
                "/sys/firmware/acpi/tables/FACP",
                "/sys/firmware/acpi/tables/XSDT",
                "/sys/firmware/acpi/tables/RSDT"
            };

            foreach (var path in candidatePaths)
            {
                if (!File.Exists(path)) continue;
                var bytes = File.ReadAllBytes(path);
                var v = ParseAgesaVersion(bytes);
                if (!string.IsNullOrEmpty(v))
                    return v;
            }
        }
        catch
        {
            // ignore ACPI/permissions issues
        }

        return string.Empty;
    }

    // ---------- AGESA parsing helpers (adapted from ZenStates-Core AgesaUtils) ----------

    private static readonly bool[] AgesaAllowedChars = BuildAgesaAllowedTable();

    private static string ParseAgesaVersion(byte[] source)
    {
        if (source == null || source.Length == 0) return string.Empty;

        byte[] marker = Encoding.ASCII.GetBytes("AGESA!V9");
        int markerOffset = FindSequence(source, marker);
        if (markerOffset < 0) return string.Empty;

        int versionStart = markerOffset + marker.Length;
        versionStart = FindFirstAllowed(source, versionStart);
        if (versionStart < 0) return string.Empty;
        int versionEnd = FindFirstInvalid(source, versionStart);

        if (versionEnd > versionStart)
        {
            return Encoding.ASCII.GetString(source, versionStart, versionEnd - versionStart)
                .Trim('\0', ' ');
        }

        return string.Empty;
    }

    private static int FindSequence(byte[] data, byte[] pattern)
    {
        if (data.Length == 0 || pattern.Length == 0 || pattern.Length > data.Length)
            return -1;

        for (int i = 0; i <= data.Length - pattern.Length; i++)
        {
            bool match = true;
            for (int j = 0; j < pattern.Length; j++)
            {
                if (data[i + j] != pattern[j])
                {
                    match = false;
                    break;
                }
            }
            if (match) return i;
        }
        return -1;
    }

    private static int FindFirstInvalid(byte[] data, int startIndex = 0)
    {
        for (int i = startIndex; i < data.Length; i++)
        {
            if (!AgesaAllowedChars[data[i]])
                return i;
        }
        return data.Length;
    }

    private static int FindFirstAllowed(byte[] data, int startIndex = 0)
    {
        for (int i = startIndex; i < data.Length; i++)
        {
            if (AgesaAllowedChars[data[i]])
                return i;
        }
        return -1;
    }

    private static bool[] BuildAgesaAllowedTable()
    {
        var table = new bool[256];

        for (int c = '0'; c <= '9'; c++) table[c] = true;
        for (int c = 'A'; c <= 'Z'; c++) table[c] = true;
        for (int c = 'a'; c <= 'z'; c++) table[c] = true;

        table[' '] = true;
        table['.'] = true;
        table['-'] = true;

        return table;
    }

    /// <summary>
    /// Generic DDR4 reader using the same UMC register layout as ZenStates-Core DDR4Dictionary.
    /// Uses UMC0 (offset 0) only, like the existing Granite Ridge DDR5 reader.
    /// </summary>
    private static DramTimingsModel ReadGenericDdr4Timings()
    {
        var model = new DramTimingsModel();

        try
        {
            const uint offset = 0; // UMC0

            // Primary and secondary timings using DDR4Dictionary map (same offsets as DDR5Dictionary, different platform).
            uint reg50204 = ReadSmn(offset | 0x50204);
            uint reg50208 = ReadSmn(offset | 0x50208);
            uint reg5020C = ReadSmn(offset | 0x5020C);
            uint reg50210 = ReadSmn(offset | 0x50210);
            uint reg50214 = ReadSmn(offset | 0x50214);
            uint reg50218 = ReadSmn(offset | 0x50218);
            uint reg5021C = ReadSmn(offset | 0x5021C);
            uint reg50220 = ReadSmn(offset | 0x50220);
            uint reg50224 = ReadSmn(offset | 0x50224);
            uint reg50228 = ReadSmn(offset | 0x50228);
            uint reg50230 = ReadSmn(offset | 0x50230);
            uint reg50234 = ReadSmn(offset | 0x50234);
            uint reg50250 = ReadSmn(offset | 0x50250);
            uint reg50254 = ReadSmn(offset | 0x50254);
            uint reg50258 = ReadSmn(offset | 0x50258);
            uint reg502A4 = ReadSmn(offset | 0x502A4);

            uint tcl = BitSlice(reg50204, 5, 0);
            uint trcdRd = BitSlice(reg50204, 21, 16);
            uint trcdWr = BitSlice(reg50204, 29, 24);
            if (trcdWr == 0) trcdWr = trcdRd;
            uint tras = BitSlice(reg50204, 14, 8);
            uint trp = BitSlice(reg50208, 21, 16);
            uint trc = BitSlice(reg50208, 7, 0);

            uint trrds = BitSlice(reg5020C, 4, 0);
            uint trrdl = BitSlice(reg5020C, 12, 8);
            uint tfaw = BitSlice(reg50210, 7, 0);
            uint rtp = BitSlice(reg5020C, 28, 24);
            uint twrs = BitSlice(reg50214, 12, 8);
            uint twrl = BitSlice(reg50214, 22, 16);
            uint tcwl = BitSlice(reg50214, 5, 0);
            uint twr = BitSlice(reg50218, 7, 0);

            uint trcPage = BitSlice(reg5021C, 31, 20);

            uint rdrdScl = BitSlice(reg50220, 29, 24);
            uint rdrdSc = BitSlice(reg50220, 19, 16);
            uint rdrdSd = BitSlice(reg50220, 11, 8);
            uint rdrdDd = BitSlice(reg50220, 3, 0);

            uint wrwrScl = BitSlice(reg50224, 29, 24);
            uint wrwrSc = BitSlice(reg50224, 19, 16);
            uint wrwrSd = BitSlice(reg50224, 11, 8);
            uint wrwrDd = BitSlice(reg50224, 3, 0);

            uint rdwr = BitSlice(reg50228, 13, 8);
            uint wrrd = BitSlice(reg50228, 3, 0);

            uint refi = BitSlice(reg50230, 15, 0);

            uint modPda = BitSlice(reg50234, 29, 24);
            uint mrdPda = BitSlice(reg50234, 21, 16);
            uint mod = BitSlice(reg50234, 13, 8);
            uint mrd = BitSlice(reg50234, 5, 0);

            uint stag = BitSlice(reg50250, 26, 16);
            uint stagSb = BitSlice(reg50250, 8, 0);

            uint cke = BitSlice(reg50254, 28, 24);
            uint xp = BitSlice(reg50254, 5, 0);

            uint phyWrd = BitSlice(reg50258, 26, 24);
            uint phyRdl = BitSlice(reg50258, 23, 16);
            uint phyWrl = BitSlice(reg50258, 15, 8);

            uint wrpre = BitSlice(reg502A4, 10, 8);
            uint rdpre = BitSlice(reg502A4, 2, 0);

            // TRFC / TRFC2 – as in ZenStates-Core DDR4: first non-default value from 0x50260/0x50264.
            uint trfc0 = ReadSmn(offset | 0x50260);
            uint trfc1 = ReadSmn(offset | 0x50264);
            uint trfcReg = trfc0 != trfc1 ? (trfc0 != 0x21060138 ? trfc0 : trfc1) : trfc0;

            uint rfc = 0;
            uint rfc2 = 0;
            if (trfcReg != 0)
            {
                rfc = BitSlice(trfcReg, 10, 0);
                rfc2 = BitSlice(trfcReg, 21, 11);
            }

            // For now we don't compute ns values for DDR4; leave them 0.

            model = new DramTimingsModel
            {
                Tcl = tcl,
                TrcdRd = trcdRd,
                TrcdWr = trcdWr,
                Trp = trp,
                Tras = tras,
                Trc = trc,
                Trrds = trrds,
                Trrdl = trrdl,
                Tfaw = tfaw,
                Twr = twr != 0 ? twr : twrs,
                Tcwl = tcwl,
                Rtp = rtp,
                Wtrs = twrs,
                Wtrl = twrl,
                Rdwr = rdwr,
                Wrrd = wrrd,
                RdrdScl = rdrdScl,
                WrwrScl = wrwrScl,
                RdrdSc = rdrdSc,
                RdrdSd = rdrdSd,
                RdrdDd = rdrdDd,
                WrwrSc = wrwrSc,
                WrwrSd = wrwrSd,
                WrwrDd = wrwrDd,
                TrcPage = trcPage,
                Mod = mod,
                ModPda = modPda,
                Mrd = mrd,
                MrdPda = mrdPda,
                Stag = stag,
                StagSb = stagSb,
                Cke = cke,
                Xp = xp,
                PhyWrd = phyWrd,
                PhyWrl = phyWrl,
                PhyRdl = phyRdl,
                Refi = refi,
                Wrpre = wrpre,
                Rdpre = rdpre,
                Rfc = rfc,
                Rfc2 = rfc2,
                // Ns fields left at 0 for DDR4 for now.
                GdmEnabled = false,
                PowerDownEnabled = false,
                Cmd2T = string.Empty,
                FrequencyHintMHz = 0
            };
        }
        catch
        {
            // leave model at defaults
        }

        return model;
    }

    private static SmuMetrics ApplyZenpowerOverrides(SmuMetrics metrics)
    {
        try
        {
            const string hwmonRoot = "/sys/class/hwmon";
            if (!Directory.Exists(hwmonRoot)) return metrics;

            string? zpPath = null;
            foreach (var dir in Directory.GetDirectories(hwmonRoot))
            {
                var namePath = Path.Combine(dir, "name");
                if (!File.Exists(namePath)) continue;
                var name = File.ReadAllText(namePath).Trim().ToLowerInvariant();
                if (name.Contains("zenpower"))
                {
                    zpPath = dir;
                    break;
                }
            }

            if (zpPath is null) return metrics;

            // Keep Vcore strictly from the PM table; do not override it with zenpower.
            float cpuVddio = metrics.CpuVddio;
            float memVdd = metrics.MemVdd;
            float memVddq = metrics.MemVddq;
            float memVpp = metrics.MemVpp;
            float vsoc = metrics.Vsoc;

            // Read voltage inputs (in*_label / in*_input, millivolts) – only for SoC/MEM rails.
            foreach (var labelPath in Directory.GetFiles(zpPath, "in*_label"))
            {
                var fileName = Path.GetFileName(labelPath);
                var idxStr = fileName.AsSpan(2, fileName.Length - "in".Length - "_label".Length);
                if (!int.TryParse(idxStr, out var idx)) continue;

                var label = File.ReadAllText(labelPath).Trim().ToLowerInvariant();
                var valuePath = Path.Combine(zpPath, $"in{idx}_input");
                if (!File.Exists(valuePath)) continue;

                if (!float.TryParse(File.ReadAllText(valuePath).Trim(), out var mV)) continue;
                float v = mV / 1000.0f;

                if (label.Contains("vddcr_soc") || label.Contains("vsoc") || label.Contains("svi2_soc"))
                    vsoc = v;
                else if (label.Contains("vddio_mem") || label.Contains("vddmem") || label.Contains("mem vdd"))
                    memVdd = v;
                else if (label.Contains("vddq") && label.Contains("mem"))
                    memVddq = v;
                else if (label.Contains("vpp") && label.Contains("mem"))
                    memVpp = v;
            }

            // Read power inputs (power*_label / power*_input, microwatts).
            // zenpower exposes RAPL_P_Package (package power) as a power sensor.
            float packagePower = metrics.CpuPptWatts;
            foreach (var labelPath in Directory.GetFiles(zpPath, "power*_label"))
            {
                var fileName = Path.GetFileName(labelPath);
                // power1_label → extract "1"
                var idxStr = fileName.AsSpan(5, fileName.Length - "power".Length - "_label".Length);
                if (!int.TryParse(idxStr, out var idx)) continue;

                var label = File.ReadAllText(labelPath).Trim().ToLowerInvariant();
                var valuePath = Path.Combine(zpPath, $"power{idx}_input");
                if (!File.Exists(valuePath)) continue;

                if (!float.TryParse(File.ReadAllText(valuePath).Trim(), out var uW)) continue;
                float watts = uW / 1_000_000f;

                // RAPL_P_Package = total package power (best proxy for PPT)
                if (label.Contains("rapl") || label.Contains("package"))
                {
                    if (watts >= 0.1f && watts <= 500f)
                        packagePower = watts;
                }
            }

            // Read current inputs (curr*_label / curr*_input, milliamps).
            float packageCurrent = metrics.CpuPackageCurrentAmps;
            foreach (var labelPath in Directory.GetFiles(zpPath, "curr*_label"))
            {
                var fileName = Path.GetFileName(labelPath);
                var idxStr = fileName.AsSpan(4, fileName.Length - "curr".Length - "_label".Length);
                if (!int.TryParse(idxStr, out var idx)) continue;

                var label = File.ReadAllText(labelPath).Trim().ToLowerInvariant();
                var valuePath = Path.Combine(zpPath, $"curr{idx}_input");
                if (!File.Exists(valuePath)) continue;

                if (!float.TryParse(File.ReadAllText(valuePath).Trim(), out var mA)) continue;
                float amps = mA / 1000f;

                // SVI2_C_Core = core current (TDC-relevant)
                if (label.Contains("core") || label.Contains("svi2_c_core"))
                {
                    if (amps >= 0.01f && amps <= 300f)
                        packageCurrent = amps;
                }
            }

            return new SmuMetrics
            {
                CpuPackagePowerWatts = metrics.CpuPackagePowerWatts,
                CpuPptWatts = packagePower,
                CpuPackageCurrentAmps = packageCurrent,
                Vcore = metrics.Vcore, // never overridden by zenpower
                CpuTempCelsius = metrics.CpuTempCelsius,
                CoreTempsCelsius = metrics.CoreTempsCelsius,
                TdieCelsius = metrics.TdieCelsius,
                TctlCelsius = metrics.TctlCelsius,
                Tccd1Celsius = metrics.Tccd1Celsius,
                Tccd2Celsius = metrics.Tccd2Celsius,
                CoreClockMHz = metrics.CoreClockMHz,
                CoreClocksGhz = metrics.CoreClocksGhz,
                MemoryClockMHz = metrics.MemoryClockMHz,
                FclkMHz = metrics.FclkMHz,
                UclkMHz = metrics.UclkMHz,
                MclkMHz = metrics.MclkMHz,
                Vsoc = vsoc,
                Vddp = metrics.Vddp,
                VddgCcd = metrics.VddgCcd,
                VddgIod = metrics.VddgIod,
                VddMisc = metrics.VddMisc,
                CpuVddio = cpuVddio,
                MemVdd = memVdd,
                MemVddq = memVddq,
                MemVpp = memVpp
            };
        }
        catch
        {
            return metrics;
        }
    }

    /// <summary>
    /// Read per-CCD/per-core temperatures from hwmon. Tries (1) k10temp: temp3–temp10 = Tccd1–Tccd8;
    /// (2) zenpower: temp*_input when k10temp not present. temp*_input is millidegrees; we return °C.
    /// Used as fallback when PM table core temps are missing.
    /// </summary>
    private static IReadOnlyList<float> ReadK10TempCoreTemps()
    {
        const string hwmonRoot = "/sys/class/hwmon";
        if (!Directory.Exists(hwmonRoot)) return Array.Empty<float>();

        string? basePath = null;
        bool isK10 = false;
        foreach (var dir in Directory.GetDirectories(hwmonRoot))
        {
            var namePath = Path.Combine(dir, "name");
            if (!File.Exists(namePath)) continue;
            var name = File.ReadAllText(namePath).Trim();
            if (name.Equals("k10temp", StringComparison.OrdinalIgnoreCase))
            {
                basePath = dir;
                isK10 = true;
                break;
            }
            if (name.Contains("zenpower", StringComparison.OrdinalIgnoreCase) && basePath == null)
                basePath = dir;
        }
        if (string.IsNullOrEmpty(basePath)) return Array.Empty<float>();

        var list = new List<float>();
        if (isK10)
        {
            // k10temp: temp3 = Tccd1 .. temp10 = Tccd8
            for (int i = 3; i <= 10; i++)
                TryAddTempInput(basePath, i, list);
        }
        else
        {
            // zenpower: scan temp1_input, temp2_input, ... and take plausible values
            for (int i = 1; i <= 10; i++)
                TryAddTempInput(basePath, i, list);
        }
        return list;
    }

    private static void TryAddTempInput(string hwmonDir, int index, List<float> list)
    {
        var path = Path.Combine(hwmonDir, $"temp{index}_input");
        if (!File.Exists(path)) return;
        if (!int.TryParse(File.ReadAllText(path).Trim(), out var raw)) return;
        float celsius = raw / 1000f; // millidegrees -> °C
        if (celsius >= 0f && celsius <= 150f)
            list.Add(celsius);
    }

    /// <summary>
    /// If PM table core temps are empty or all zero, build per-core temps from CCD-level
    /// sensors (k10temp / zenpower). Since Linux doesn't expose actual per-core temps on
    /// AMD Zen, we broadcast each CCD's temperature to all cores on that CCD.
    /// </summary>
    private static SmuMetrics ApplyK10TempCoreTempFallback(SmuMetrics metrics)
    {
        var fromPm = metrics.CoreTempsCelsius;
        if (fromPm is { Count: > 0 })
        {
            var hasNonZero = false;
            foreach (var t in fromPm)
            {
                if (t > 0f) { hasNonZero = true; break; }
            }
            if (hasNonZero) return metrics;
        }

        // Determine physical core count from usage/freq arrays (set by ApplyProcStatCoreUsage later,
        // but at this stage those haven't been applied yet). Use cpufreq to count physical cores.
        int physicalCores = CountPhysicalCores();
        if (physicalCores == 0) physicalCores = 8; // sensible default for Zen desktop

        // Get CCD temps from k10temp (temp3=Tccd1..temp10=Tccd8).
        var ccdTemps = ReadK10TempCcdOnly();
        if (ccdTemps.Count == 0)
        {
            // Fall back: use Tccd1 from zenpower or metrics if available.
            float? fallbackTemp = metrics.Tccd1Celsius;
            if (!fallbackTemp.HasValue || fallbackTemp.Value <= 0)
            {
                // Try zenpower Tccd1 directly.
                var raw = ReadK10TempCoreTemps();
                // zenpower returns [Tdie, Tctl, Tccd1, ...] — take the last value as CCD temp.
                if (raw.Count > 0) fallbackTemp = raw[^1];
            }
            if (fallbackTemp.HasValue && fallbackTemp.Value > 0)
                ccdTemps = new[] { fallbackTemp.Value };
        }

        if (ccdTemps.Count == 0) return metrics;

        // Broadcast CCD temps to all cores. Zen 5 desktop: 8 cores per CCD.
        // If 2 CCDs: first half of cores = CCD1, second half = CCD2.
        int coresPerCcd = ccdTemps.Count > 1 ? physicalCores / ccdTemps.Count : physicalCores;
        var perCore = new float[physicalCores];
        for (int i = 0; i < physicalCores; i++)
        {
            int ccdIdx = coresPerCcd > 0 ? Math.Min(i / coresPerCcd, ccdTemps.Count - 1) : 0;
            perCore[i] = ccdTemps[ccdIdx];
        }

        return new SmuMetrics
        {
            CpuPackagePowerWatts = metrics.CpuPackagePowerWatts,
            CpuPptWatts = metrics.CpuPptWatts,
            CpuPackageCurrentAmps = metrics.CpuPackageCurrentAmps,
            Vcore = metrics.Vcore,
            CpuTempCelsius = metrics.CpuTempCelsius,
            CoreTempsCelsius = perCore,
            TdieCelsius = metrics.TdieCelsius,
            TctlCelsius = metrics.TctlCelsius,
            Tccd1Celsius = metrics.Tccd1Celsius,
            Tccd2Celsius = metrics.Tccd2Celsius,
            CoreClockMHz = metrics.CoreClockMHz,
            CoreClocksGhz = metrics.CoreClocksGhz,
            MemoryClockMHz = metrics.MemoryClockMHz,
            FclkMHz = metrics.FclkMHz,
            UclkMHz = metrics.UclkMHz,
            MclkMHz = metrics.MclkMHz,
            Vsoc = metrics.Vsoc,
            Vddp = metrics.Vddp,
            VddgCcd = metrics.VddgCcd,
            VddgIod = metrics.VddgIod,
            VddMisc = metrics.VddMisc,
            CpuVddio = metrics.CpuVddio,
            MemVdd = metrics.MemVdd,
            MemVddq = metrics.MemVddq,
            MemVpp = metrics.MemVpp
        };
    }

    /// <summary>Read only CCD-level temps from k10temp (temp3=Tccd1, temp4=Tccd2, ...).</summary>
    private static IReadOnlyList<float> ReadK10TempCcdOnly()
    {
        const string hwmonRoot = "/sys/class/hwmon";
        if (!Directory.Exists(hwmonRoot)) return Array.Empty<float>();

        foreach (var dir in Directory.GetDirectories(hwmonRoot))
        {
            var namePath = Path.Combine(dir, "name");
            if (!File.Exists(namePath)) continue;
            var name = File.ReadAllText(namePath).Trim();

            if (name.Equals("k10temp", StringComparison.OrdinalIgnoreCase))
            {
                // k10temp: temp3 = Tccd1, temp4 = Tccd2, ...
                var list = new List<float>();
                for (int i = 3; i <= 10; i++)
                    TryAddTempInput(dir, i, list);
                return list;
            }

            if (name.Contains("zenpower", StringComparison.OrdinalIgnoreCase))
            {
                // zenpower: find Tccd* labels specifically.
                var list = new List<float>();
                for (int i = 1; i <= 10; i++)
                {
                    var labelPath = Path.Combine(dir, $"temp{i}_label");
                    if (!File.Exists(labelPath)) continue;
                    var label = File.ReadAllText(labelPath).Trim().ToLowerInvariant();
                    if (label.StartsWith("tccd"))
                    {
                        var inputPath = Path.Combine(dir, $"temp{i}_input");
                        if (!File.Exists(inputPath)) continue;
                        if (int.TryParse(File.ReadAllText(inputPath).Trim(), out var raw))
                        {
                            float c = raw / 1000f;
                            if (c >= 0f && c <= 150f) list.Add(c);
                        }
                    }
                }
                if (list.Count > 0) return list;
            }
        }
        return Array.Empty<float>();
    }

    /// <summary>Count physical CPU cores from cpufreq directories (cpu0..cpuN, dividing by 2 for SMT).</summary>
    private static int CountPhysicalCores()
    {
        try
        {
            int logical = 0;
            for (int i = 0; i < 256; i++)
            {
                if (Directory.Exists($"/sys/devices/system/cpu/cpu{i}/cpufreq"))
                    logical++;
                else if (i > 0)
                    break;
            }
            // SMT: 2 threads per core on Zen.
            return logical > 0 ? logical / 2 : 0;
        }
        catch { return 0; }
    }

    /// <summary>Read Tctl (temp1), Tccd1 (temp3), Tccd2 (temp4) from k10temp only. Null for any channel not exposed.</summary>
    private static (float? Tctl, float? Tccd1, float? Tccd2) ReadK10TempTctlTccd()
    {
        const string hwmonRoot = "/sys/class/hwmon";
        if (!Directory.Exists(hwmonRoot)) return (null, null, null);

        string? basePath = null;
        foreach (var dir in Directory.GetDirectories(hwmonRoot))
        {
            var namePath = Path.Combine(dir, "name");
            if (!File.Exists(namePath)) continue;
            var name = File.ReadAllText(namePath).Trim();
            if (name.Equals("k10temp", StringComparison.OrdinalIgnoreCase))
            {
                basePath = dir;
                break;
            }
        }
        if (string.IsNullOrEmpty(basePath)) return (null, null, null);

        float? tctl = TryReadTempInput(basePath, 1, out var v1) ? v1 : null;
        float? tccd1 = TryReadTempInput(basePath, 3, out var v3) ? v3 : null;
        float? tccd2 = TryReadTempInput(basePath, 4, out var v4) ? v4 : null;
        return (tctl, tccd1, tccd2);
    }

    /// <summary>
    /// Read Tdie, Tctl, Tccd1 from zenpower hwmon (temp*_label / temp*_input) when k10temp is not present.
    /// Returns nulls when zenpower is not loaded or labels are missing.
    /// </summary>
    private static (float? Tdie, float? Tctl, float? Tccd1) ReadZenpowerTdieTctlTccd()
    {
        const string hwmonRoot = "/sys/class/hwmon";
        if (!Directory.Exists(hwmonRoot)) return (null, null, null);

        string? basePath = null;
        foreach (var dir in Directory.GetDirectories(hwmonRoot))
        {
            var namePath = Path.Combine(dir, "name");
            if (!File.Exists(namePath)) continue;
            var name = File.ReadAllText(namePath).Trim();
            if (name.Contains("zenpower", StringComparison.OrdinalIgnoreCase))
            {
                basePath = dir;
                break;
            }
        }
        if (string.IsNullOrEmpty(basePath)) return (null, null, null);

        float? tdie = null, tctl = null, tccd1 = null;

        foreach (var labelPath in Directory.GetFiles(basePath, "temp*_label"))
        {
            var fileName = Path.GetFileName(labelPath);
            // tempN_label -> extract N
            var span = fileName.AsSpan(4, fileName.Length - "temp".Length - "_label".Length);
            if (!int.TryParse(span, out var idx)) continue;

            var label = File.ReadAllText(labelPath).Trim();
            var inputPath = Path.Combine(basePath, $"temp{idx}_input");
            if (!File.Exists(inputPath)) continue;
            if (!int.TryParse(File.ReadAllText(inputPath).Trim(), out var raw)) continue;
            float celsius = raw / 1000f;
            if (celsius < 0f || celsius > 150f) continue;

            if (label.Contains("Tdie", StringComparison.OrdinalIgnoreCase))
                tdie = celsius;
            else if (label.Contains("Tctl", StringComparison.OrdinalIgnoreCase))
                tctl = celsius;
            else if (label.Contains("Tccd1", StringComparison.OrdinalIgnoreCase))
                tccd1 = celsius;
        }

        return (tdie, tctl, tccd1);
    }

    private static bool TryReadTempInput(string hwmonDir, int index, out float celsius)
    {
        celsius = 0;
        var path = Path.Combine(hwmonDir, $"temp{index}_input");
        if (!File.Exists(path)) return false;
        if (!int.TryParse(File.ReadAllText(path).Trim(), out var raw)) return false;
        celsius = raw / 1000f;
        return celsius >= 0f && celsius <= 150f;
    }

    /// <summary>
    /// Overlay Tdie, Tctl, Tccd1, Tccd2 from k10temp or zenpower onto metrics.
    /// Prefers k10temp when available; falls back to zenpower when only zenpower is loaded.
    /// </summary>
    private static SmuMetrics ApplyK10TempTctlTccdOverlay(SmuMetrics metrics)
    {
        var (tctl, tccd1, tccd2) = ReadK10TempTctlTccd();
        float? tdie = metrics.TdieCelsius;

        // If k10temp didn't provide anything, fall back to zenpower temp labels.
        if (!tctl.HasValue && !tccd1.HasValue && !tccd2.HasValue)
        {
            var (zTdie, zTctl, zTccd1) = ReadZenpowerTdieTctlTccd();
            if (zTdie.HasValue) tdie = zTdie;
            if (zTctl.HasValue) tctl = zTctl;
            if (zTccd1.HasValue) tccd1 = zTccd1;
        }

        // If we still don't have an explicit Tdie but we do have Tctl or a plausible CPU temp,
        // use those as a best-effort fallback so the UI doesn't show a missing Die temp.
        if (!tdie.HasValue && tctl.HasValue)
            tdie = tctl;
        if (!tdie.HasValue && metrics.CpuTempCelsius > 0)
            tdie = metrics.CpuTempCelsius;

        if (!tdie.HasValue && !tctl.HasValue && !tccd1.HasValue && !tccd2.HasValue)
            return metrics;

        return new SmuMetrics
        {
            CpuPackagePowerWatts = metrics.CpuPackagePowerWatts,
            CpuPptWatts = metrics.CpuPptWatts,
            CpuPackageCurrentAmps = metrics.CpuPackageCurrentAmps,
            Vcore = metrics.Vcore,
            CpuTempCelsius = metrics.CpuTempCelsius,
            CoreTempsCelsius = metrics.CoreTempsCelsius,
            TdieCelsius = tdie ?? metrics.TdieCelsius,
            TctlCelsius = tctl ?? metrics.TctlCelsius,
            Tccd1Celsius = tccd1 ?? metrics.Tccd1Celsius,
            Tccd2Celsius = tccd2 ?? metrics.Tccd2Celsius,
            CoreClockMHz = metrics.CoreClockMHz,
            CoreClocksGhz = metrics.CoreClocksGhz,
            MemoryClockMHz = metrics.MemoryClockMHz,
            FclkMHz = metrics.FclkMHz,
            UclkMHz = metrics.UclkMHz,
            MclkMHz = metrics.MclkMHz,
            Vsoc = metrics.Vsoc,
            Vddp = metrics.Vddp,
            VddgCcd = metrics.VddgCcd,
            VddgIod = metrics.VddgIod,
            VddMisc = metrics.VddMisc,
            CpuVddio = metrics.CpuVddio,
            MemVdd = metrics.MemVdd,
            MemVddq = metrics.MemVddq,
            MemVpp = metrics.MemVpp
        };
    }

    /// <summary>
    /// Overlay SPD (DIMM) temperatures from spd5118 hwmon devices (temp1_input) onto metrics.
    /// Each spd5118 instance corresponds to one DIMM temperature sensor.
    /// </summary>
    private static SmuMetrics ApplySpdTempsOverlay(SmuMetrics metrics)
    {
        var temps = ReadSpdTempsCelsius();
        if (temps.Count == 0)
            return metrics;

        return new SmuMetrics
        {
            CpuPackagePowerWatts = metrics.CpuPackagePowerWatts,
            CpuPptWatts = metrics.CpuPptWatts,
            CpuPackageCurrentAmps = metrics.CpuPackageCurrentAmps,
            Vcore = metrics.Vcore,
            CpuTempCelsius = metrics.CpuTempCelsius,
            CoreTempsCelsius = metrics.CoreTempsCelsius,
            CoreUsagePercent = metrics.CoreUsagePercent,
            CoreFreqMHz = metrics.CoreFreqMHz,
            TdieCelsius = metrics.TdieCelsius,
            TctlCelsius = metrics.TctlCelsius,
            Tccd1Celsius = metrics.Tccd1Celsius,
            Tccd2Celsius = metrics.Tccd2Celsius,
            CoreClockMHz = metrics.CoreClockMHz,
            CoreClocksGhz = metrics.CoreClocksGhz,
            MemoryClockMHz = metrics.MemoryClockMHz,
            FclkMHz = metrics.FclkMHz,
            UclkMHz = metrics.UclkMHz,
            MclkMHz = metrics.MclkMHz,
            Vsoc = metrics.Vsoc,
            Vddp = metrics.Vddp,
            VddgCcd = metrics.VddgCcd,
            VddgIod = metrics.VddgIod,
            VddMisc = metrics.VddMisc,
            CpuVddio = metrics.CpuVddio,
            MemVdd = metrics.MemVdd,
            MemVddq = metrics.MemVddq,
            MemVpp = metrics.MemVpp,
            SpdTempsCelsius = temps
        };
    }

    /// <summary>
    /// Read SPD5118 DIMM temperatures in a robust way:
    /// 1) Known AMD desktop roots under /sys/devices (paths you provided)
    /// 2) Fallback: /sys/class/hwmon entries whose name contains "spd5118"
    /// 3) Fallback: parse "sensors" output for spd5118 sections and temp1 values.
    /// </summary>
    private static IReadOnlyList<float> ReadSpdTempsCelsius()
    {
        // 1) Board-specific roots under /sys/devices (fast, no recursion)
        var fromDevices = ReadSpdTempsFromDevices();
        if (fromDevices.Count > 0) return fromDevices;

        // 2) Try /sys/class/hwmon for hwmon devices named spd5118-*
        var fromHwmon = ReadSpdTempsFromHwmon();
        if (fromHwmon.Count > 0) return fromHwmon;

        // 3) Last resort: parse "sensors" output
        return ReadSpdTempsFromSensors();
    }

    /// <summary>Read SPD temps from the specific /sys/devices i2c-7/7-005{3,1}/hwmon/... roots.</summary>
    private static IReadOnlyList<float> ReadSpdTempsFromDevices()
    {
        var list = new List<float>();
        try
        {
            string[] deviceRoots =
            {
                "/sys/devices/pci0000:00/0000:00:14.0/i2c-7/7-0053",
                "/sys/devices/pci0000:00/0000:00:14.0/i2c-7/7-0051"
            };

            foreach (var root in deviceRoots)
            {
                if (!Directory.Exists(root)) continue;
                var hwmonRoot = Path.Combine(root, "hwmon");
                if (!Directory.Exists(hwmonRoot)) continue;

                foreach (var hwmonDir in Directory.GetDirectories(hwmonRoot))
                {
                    var tempPath = Path.Combine(hwmonDir, "temp1_input");
                    if (!File.Exists(tempPath)) continue;
                    var s = File.ReadAllText(tempPath).Trim();
                    if (!int.TryParse(s, out var rawInt)) continue;
                    float celsius = rawInt / 1000f;
                    if (celsius < 0f || celsius > 150f) continue;
                    list.Add(celsius);
                }
            }
        }
        catch
        {
            // ignore
        }

        return list.Count == 0 ? Array.Empty<float>() : list;
    }

    /// <summary>Read SPD temps from /sys/class/hwmon entries whose "name" contains spd5118.</summary>
    private static IReadOnlyList<float> ReadSpdTempsFromHwmon()
    {
        const string hwmonRoot = "/sys/class/hwmon";
        if (!Directory.Exists(hwmonRoot)) return Array.Empty<float>();

        var list = new List<float>();
        try
        {
            foreach (var dir in Directory.GetDirectories(hwmonRoot))
            {
                var namePath = Path.Combine(dir, "name");
                if (!File.Exists(namePath)) continue;
                var name = File.ReadAllText(namePath).Trim().ToLowerInvariant();
                if (!name.Contains("spd5118")) continue;

                var tempPath = Path.Combine(dir, "temp1_input");
                if (!File.Exists(tempPath)) continue;
                var s = File.ReadAllText(tempPath).Trim();
                if (!int.TryParse(s, out var rawInt)) continue;
                float celsius = rawInt / 1000f;
                if (celsius < 0f || celsius > 150f) continue;
                list.Add(celsius);
            }
        }
        catch
        {
            // ignore
        }

        return list.Count == 0 ? Array.Empty<float>() : list;
    }

    /// <summary>Fallback: run "sensors" and parse spd5118 sections for temp1 values.</summary>
    private static IReadOnlyList<float> ReadSpdTempsFromSensors()
    {
        var list = new List<float>();
        try
        {
            using var proc = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = "sensors",
                    Arguments = "",
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                    CreateNoWindow = true
                }
            };
            proc.Start();
            var stdout = proc.StandardOutput.ReadToEnd();
            proc.WaitForExit(2000);
            if (proc.ExitCode != 0 || string.IsNullOrWhiteSpace(stdout))
                return Array.Empty<float>();

            bool inSpd = false;
            foreach (var rawLine in stdout.Split('\n'))
            {
                var line = rawLine.TrimEnd();
                if (string.IsNullOrWhiteSpace(line))
                {
                    inSpd = false;
                    continue;
                }

                // Chip header: no leading whitespace and no ':' (e.g. "spd5118-i2c-7-53")
                if (!char.IsWhiteSpace(rawLine, 0) && !line.Contains(':'))
                {
                    inSpd = line.Trim().Contains("spd5118", StringComparison.OrdinalIgnoreCase);
                    continue;
                }

                if (!inSpd) continue;
                var trimmed = line.TrimStart();
                if (!trimmed.StartsWith("temp", StringComparison.OrdinalIgnoreCase)) continue;
                var colon = trimmed.IndexOf(':');
                if (colon <= 0) continue;
                var rest = trimmed[(colon + 1)..];

                // Extract first numeric token (e.g. +33.0°C)
                foreach (var part in rest.Split(' ', StringSplitOptions.RemoveEmptyEntries))
                {
                    var p = part.TrimStart('+');
                    if (float.TryParse(p.TrimEnd('C', 'c', '°'), NumberStyles.Float, CultureInfo.InvariantCulture, out var v))
                    {
                        list.Add(v);
                        break;
                    }
                }
            }
        }
        catch
        {
            return Array.Empty<float>();
        }

        return list.Count == 0 ? Array.Empty<float>() : list;
    }

    /// <summary>
    /// Compute per-physical-core usage (0–100 %) from /proc/stat and per-core frequency (MHz) from cpufreq,
    /// attach both to metrics. Logical CPUs are grouped as SMT pairs (cpu0+cpu1 = core0, cpu2+cpu3 = core1, ...).
    /// </summary>
    private SmuMetrics ApplyProcStatCoreUsage(SmuMetrics metrics)
    {
        var coreUsage = ReadPerCoreUsagePercent();
        var coreFreq = ReadPerCoreFreqMhz();
        if (coreUsage.Count == 0 && coreFreq.Count == 0)
            return metrics;

        return new SmuMetrics
        {
            CpuPackagePowerWatts = metrics.CpuPackagePowerWatts,
            CpuPptWatts = metrics.CpuPptWatts,
            CpuPackageCurrentAmps = metrics.CpuPackageCurrentAmps,
            Vcore = metrics.Vcore,
            CpuTempCelsius = metrics.CpuTempCelsius,
            CoreTempsCelsius = metrics.CoreTempsCelsius,
            CoreUsagePercent = coreUsage,
            CoreFreqMHz = coreFreq,
            TctlCelsius = metrics.TctlCelsius,
            Tccd1Celsius = metrics.Tccd1Celsius,
            Tccd2Celsius = metrics.Tccd2Celsius,
            CoreClockMHz = metrics.CoreClockMHz,
            CoreClocksGhz = metrics.CoreClocksGhz,
            MemoryClockMHz = metrics.MemoryClockMHz,
            FclkMHz = metrics.FclkMHz,
            UclkMHz = metrics.UclkMHz,
            MclkMHz = metrics.MclkMHz,
            Vsoc = metrics.Vsoc,
            Vddp = metrics.Vddp,
            VddgCcd = metrics.VddgCcd,
            VddgIod = metrics.VddgIod,
            VddMisc = metrics.VddMisc,
            CpuVddio = metrics.CpuVddio,
            MemVdd = metrics.MemVdd,
            MemVddq = metrics.MemVddq,
            MemVpp = metrics.MemVpp,
            SpdTempsCelsius = metrics.SpdTempsCelsius
        };
    }

    /// <summary>
    /// Read /proc/stat and compute per-physical-core usage percentage based on deltas since last call.
    /// Uses standard Linux accounting: usage = 1 - deltaIdle / deltaTotal, scaled to 0–100.
    /// </summary>
    private IReadOnlyList<float> ReadPerCoreUsagePercent()
    {
        const string statPath = "/proc/stat";
        if (!File.Exists(statPath))
            return Array.Empty<float>();

        var lines = File.ReadAllLines(statPath);
        if (lines.Length == 0)
            return Array.Empty<float>();

        // First collect per-logical-CPU usage.
        var logicalUsage = new Dictionary<int, float>();

        foreach (var line in lines)
        {
            // Skip summary "cpu " line; only process "cpuN" lines.
            if (!line.StartsWith("cpu", StringComparison.Ordinal))
                continue;
            if (line.Length < 4 || !char.IsDigit(line[3]))
                continue;

            var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 5)
                continue;

            var idStr = parts[0].Substring(3);
            if (!int.TryParse(idStr, out var cpuIndex))
                continue;

            // Standard /proc/stat columns: user nice system idle iowait irq softirq steal guest guest_nice
            // We care about total and idle (idle + iowait).
            ulong ParseField(int idx)
            {
                if (idx >= parts.Length) return 0;
                return ulong.TryParse(parts[idx], out var v) ? v : 0;
            }

            ulong user = ParseField(1);
            ulong nice = ParseField(2);
            ulong system = ParseField(3);
            ulong idle = ParseField(4);
            ulong iowait = ParseField(5);
            ulong irq = ParseField(6);
            ulong softirq = ParseField(7);
            ulong steal = ParseField(8);
            ulong guest = ParseField(9);
            ulong guestNice = ParseField(10);

            ulong idleAll = idle + iowait;
            ulong total = user + nice + system + idle + iowait + irq + softirq + steal + guest + guestNice;

            if (total == 0)
                continue;

            if (!_prevCpuTimes.TryGetValue(cpuIndex, out var prev))
            {
                // First sample – store and report 0 % usage for this CPU.
                _prevCpuTimes[cpuIndex] = (idleAll, total);
                logicalUsage[cpuIndex] = 0f;
                continue;
            }

            ulong deltaIdle = idleAll - prev.Idle;
            ulong deltaTotal = total - prev.Total;
            _prevCpuTimes[cpuIndex] = (idleAll, total);

            if (deltaTotal == 0)
            {
                logicalUsage[cpuIndex] = 0f;
                continue;
            }

            float usage = 1.0f - (float)deltaIdle / deltaTotal;
            if (usage < 0f) usage = 0f;
            if (usage > 1f) usage = 1f;
            logicalUsage[cpuIndex] = usage * 100f;
        }

        if (logicalUsage.Count == 0)
            return Array.Empty<float>();

        // Group logical CPUs into physical cores by assuming 2 threads per core:
        // coreIndex = cpuIndex / 2. Average usage across threads.
        var coreSums = new Dictionary<int, (float Sum, int Count)>();
        foreach (var kvp in logicalUsage)
        {
            int coreIndex = kvp.Key / 2;
            if (!coreSums.TryGetValue(coreIndex, out var agg))
                agg = (0f, 0);
            agg.Sum += kvp.Value;
            agg.Count += 1;
            coreSums[coreIndex] = agg;
        }

        if (coreSums.Count == 0)
            return Array.Empty<float>();

        int maxCore = 0;
        foreach (var coreIndex in coreSums.Keys)
            if (coreIndex > maxCore) maxCore = coreIndex;

        var result = new float[maxCore + 1];
        for (int i = 0; i <= maxCore; i++)
        {
            if (coreSums.TryGetValue(i, out var agg) && agg.Count > 0)
                result[i] = agg.Sum / agg.Count;
            else
                result[i] = 0f;
        }

        return result;
    }

    /// <summary>
    /// Read per-logical-CPU frequencies from cpufreq (scaling_cur_freq, kHz) and aggregate into per-physical-core MHz.
    /// Groups logical CPUs as SMT pairs (cpu0+cpu1 = core0, etc.), averaging their MHz.
    /// </summary>
    private IReadOnlyList<float> ReadPerCoreFreqMhz()
    {
        var logicalFreq = new Dictionary<int, float>();

        try
        {
            const string cpuRoot = "/sys/devices/system/cpu";
            if (!Directory.Exists(cpuRoot))
                return Array.Empty<float>();

            for (int cpu = 0; cpu < 256; cpu++)
            {
                var path = Path.Combine(cpuRoot, $"cpu{cpu}", "cpufreq", "scaling_cur_freq");
                if (!File.Exists(path))
                    continue;
                var s = File.ReadAllText(path).Trim();
                if (!int.TryParse(s, out var khz) || khz <= 0)
                    continue;
                float mhz = khz / 1000f;
                logicalFreq[cpu] = mhz;
            }
        }
        catch
        {
            // ignore cpufreq read errors, just return what we have
        }

        if (logicalFreq.Count == 0)
            return Array.Empty<float>();

        var coreSums = new Dictionary<int, (float Sum, int Count)>();
        foreach (var kvp in logicalFreq)
        {
            int coreIndex = kvp.Key / 2;
            if (!coreSums.TryGetValue(coreIndex, out var agg))
                agg = (0f, 0);
            agg.Sum += kvp.Value;
            agg.Count += 1;
            coreSums[coreIndex] = agg;
        }

        if (coreSums.Count == 0)
            return Array.Empty<float>();

        int maxCore = 0;
        foreach (var coreIndex in coreSums.Keys)
            if (coreIndex > maxCore) maxCore = coreIndex;

        var result = new float[maxCore + 1];
        for (int i = 0; i <= maxCore; i++)
        {
            if (coreSums.TryGetValue(i, out var agg) && agg.Count > 0)
                result[i] = agg.Sum / agg.Count;
            else
                result[i] = 0f;
        }

        return result;
    }

    private static DramTimingsModel ReadGraniteRidgeDdr5Timings()
    {
        var model = new DramTimingsModel();

        try
        {
            const uint offset = 0; // UMC0

            // Ratio -> frequency (like Ddr5Timings.Read)
            uint ratioReg = ReadSmn(offset | 0x50200);
            float ratio = BitSlice(ratioReg, 15, 0) / 100.0f;
            float memFreq = ratio * 200.0f;

            // Gear-down mode, command rate, power-down
            bool gdm = BitSlice(ratioReg, 18, 18) == 1;
            bool cmd2T = BitSlice(ratioReg, 17, 17) == 1;
            uint refreshModeReg = ReadSmn(offset | 0x5012C);
            bool powerDown = BitSlice(refreshModeReg, 28, 28) == 1;

            // Primary and secondary timings using DDR5Dictionary map
            uint reg50204 = ReadSmn(offset | 0x50204);
            uint reg50208 = ReadSmn(offset | 0x50208);
            uint reg5020C = ReadSmn(offset | 0x5020C);
            uint reg50210 = ReadSmn(offset | 0x50210);
            uint reg50214 = ReadSmn(offset | 0x50214);
            uint reg50218 = ReadSmn(offset | 0x50218);
            uint reg5021C = ReadSmn(offset | 0x5021C);
            uint reg50220 = ReadSmn(offset | 0x50220);
            uint reg50224 = ReadSmn(offset | 0x50224);
            uint reg50228 = ReadSmn(offset | 0x50228);
            uint reg50230 = ReadSmn(offset | 0x50230);
            uint reg50234 = ReadSmn(offset | 0x50234);
            uint reg50250 = ReadSmn(offset | 0x50250);
            uint reg50254 = ReadSmn(offset | 0x50254);
            uint reg50258 = ReadSmn(offset | 0x50258);
            uint reg502A4 = ReadSmn(offset | 0x502A4);

            uint tcl = BitSlice(reg50204, 5, 0);
            uint trcd = BitSlice(reg50204, 21, 16);
            uint tras = BitSlice(reg50204, 14, 8);
            uint trp = BitSlice(reg50208, 21, 16);
            uint trc = BitSlice(reg50208, 7, 0);

            uint trrds = BitSlice(reg5020C, 4, 0);
            uint trrdl = BitSlice(reg5020C, 12, 8);
            uint tfaw = BitSlice(reg50210, 7, 0);
            uint rtp = BitSlice(reg5020C, 28, 24);
            uint twrs = BitSlice(reg50214, 12, 8);
            uint twrl = BitSlice(reg50214, 22, 16);
            uint tcwl = BitSlice(reg50214, 5, 0);
            uint twr = BitSlice(reg50218, 7, 0);

            uint trcPage = BitSlice(reg5021C, 31, 20);

            uint rdrdScl = BitSlice(reg50220, 29, 24);
            uint rdrdSc = BitSlice(reg50220, 19, 16);
            uint rdrdSd = BitSlice(reg50220, 11, 8);
            uint rdrdDd = BitSlice(reg50220, 3, 0);

            uint wrwrScl = BitSlice(reg50224, 29, 24);
            uint wrwrSc = BitSlice(reg50224, 19, 16);
            uint wrwrSd = BitSlice(reg50224, 11, 8);
            uint wrwrDd = BitSlice(reg50224, 3, 0);

            uint rdwr = BitSlice(reg50228, 13, 8);
            uint wrrd = BitSlice(reg50228, 3, 0);

            uint refi = BitSlice(reg50230, 15, 0);

            uint modPda = BitSlice(reg50234, 29, 24);
            uint mrdPda = BitSlice(reg50234, 21, 16);
            uint mod = BitSlice(reg50234, 13, 8);
            uint mrd = BitSlice(reg50234, 5, 0);

            uint stag = BitSlice(reg50250, 26, 16);
            uint stagSb = BitSlice(reg50250, 8, 0);

            uint cke = BitSlice(reg50254, 28, 24);
            uint xp = BitSlice(reg50254, 5, 0);

            uint phyWrd = BitSlice(reg50258, 26, 24);
            uint phyRdl = BitSlice(reg50258, 23, 16);
            uint phyWrl = BitSlice(reg50258, 15, 8);

            uint wrpre = BitSlice(reg502A4, 10, 8);
            uint rdpre = BitSlice(reg502A4, 2, 0);

            // TRFC / TRFC2 – choose first register that isn't the default pattern.
            uint trfc0 = ReadSmn(offset | 0x50260);
            uint trfc1 = ReadSmn(offset | 0x50264);
            uint trfc2 = ReadSmn(offset | 0x50268);
            uint trfc3 = ReadSmn(offset | 0x5026C);
            uint trfcReg = 0;
            foreach (var reg in new[] { trfc0, trfc1, trfc2, trfc3 })
            {
                if (reg != 0x00C00138)
                {
                    trfcReg = reg;
                    break;
                }
            }

            uint rfc = 0;
            uint rfc2 = 0;
            if (trfcReg != 0)
            {
                rfc = BitSlice(trfcReg, 15, 0);
                rfc2 = BitSlice(trfcReg, 31, 16);
            }

            // RFCsb – first non-zero short value from the RFCsb registers.
            uint rfcsb0 = BitSlice(ReadSmn(offset | 0x502C0), 10, 0);
            uint rfcsb1 = BitSlice(ReadSmn(offset | 0x502C4), 10, 0);
            uint rfcsb2 = BitSlice(ReadSmn(offset | 0x502C8), 10, 0);
            uint rfcsb3 = BitSlice(ReadSmn(offset | 0x502CC), 10, 0);
            uint rfcsb = 0;
            foreach (var v in new[] { rfcsb0, rfcsb1, rfcsb2, rfcsb3 })
            {
                if (v != 0)
                {
                    rfcsb = v;
                    break;
                }
            }

            // Convert tREFI / tRFC cycles to nanoseconds (as ZenStates-Core Utils.ToNanoseconds)
            static float ToNanoseconds(uint value, float frequency)
            {
                if (frequency <= 0) return 0;
                float v = value;
                float ns = v * 2000f / frequency;
                if (ns > v) ns /= 2f;
                return ns;
            }

            float trefiNs = ToNanoseconds(refi, memFreq);
            float trfcNs = ToNanoseconds(rfc, memFreq);
            float trfc2Ns = ToNanoseconds(rfc2, memFreq);
            float trfcsbNs = ToNanoseconds(rfcsb, memFreq);

            model = new DramTimingsModel
            {
                Tcl = tcl,
                Trcd = trcd,
                Trp = trp,
                Tras = tras,
                Trc = trc,
                Trrds = trrds,
                Trrdl = trrdl,
                Tfaw = tfaw,
                Twr = twr != 0 ? twr : twrs,
                Tcwl = tcwl,
                Rtp = rtp,
                Wtrs = twrs,
                Wtrl = twrl,
                Rdwr = rdwr,
                Wrrd = wrrd,
                RdrdScl = rdrdScl,
                WrwrScl = wrwrScl,
                RdrdSc = rdrdSc,
                RdrdSd = rdrdSd,
                RdrdDd = rdrdDd,
                WrwrSc = wrwrSc,
                WrwrSd = wrwrSd,
                WrwrDd = wrwrDd,
                TrcPage = trcPage,
                Mod = mod,
                ModPda = modPda,
                Mrd = mrd,
                MrdPda = mrdPda,
                Stag = stag,
                StagSb = stagSb,
                Cke = cke,
                Xp = xp,
                PhyWrd = phyWrd,
                PhyWrl = phyWrl,
                PhyRdl = phyRdl,
                Refi = refi,
                Wrpre = wrpre,
                Rdpre = rdpre,
                Rfc = rfc,
                Rfc2 = rfc2,
                Rfcsb = rfcsb,
                TrefiNs = trefiNs,
                TrfcNs = trfcNs,
                Trfc2Ns = trfc2Ns,
                TrfcsbNs = trfcsbNs,
                GdmEnabled = gdm,
                PowerDownEnabled = powerDown,
                Cmd2T = cmd2T ? "2T" : "1T",
                FrequencyHintMHz = memFreq
            };
        }
        catch
        {
            // leave model at defaults
        }

        return model;
    }
}

