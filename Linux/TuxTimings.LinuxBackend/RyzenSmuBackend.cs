using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using TuxTimings.Core;

namespace TuxTimings.LinuxBackend;

public sealed class RyzenSmuBackend : IHardwareBackend
{
    private const string BasePath = "/sys/kernel/ryzen_smu_drv";

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

            var coreTemps = new List<float>();
            for (int i = 0; i < 8; i++)
            {
                if (values.TryGetValue("CORE_TEMP_" + i, out var ct))
                    coreTemps.Add(ct);
            }

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
                Vcore = PlausibleVcore(values.GetValueOrDefault("VCORE")),
                CpuPackagePowerWatts = values.GetValueOrDefault("POWER"),
                CpuPptWatts = values.GetValueOrDefault("PPT"),
                CpuPackageCurrentAmps = values.GetValueOrDefault("CURRENT"),
                CpuTempCelsius = values.GetValueOrDefault("TEMP"),
                CoreTempsCelsius = coreTemps,
                CoreClockMHz = values.GetValueOrDefault("CORE_MHZ")
            };
        }
        catch
        {
            return null;
        }
    }
    private static bool _hasDumpedOnce;
    private static bool _hasDumpedDiagnostic;

    /// <summary>Cached dmidecode result; only run on first load, not every sensor refresh.</summary>
    private static bool _dmidecodeCached;
    private static (string ProcessorName, string PartNumbers, string Manufacturers, string MotherboardProductName, string BiosVersion, string BiosReleaseDate, IReadOnlyList<MemoryModule> Modules)? _cachedDmidecode;

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
                $"  Package:  {metrics.CpuPackagePowerWatts:F2} W",
                $"  Current:  {metrics.CpuPackageCurrentAmps:F2} A",
                $"  Core:     {metrics.CoreClockMHz:F0} MHz",
                $"  Temp:     {metrics.CpuTempCelsius:F1} °C",
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
                $"  MEM VPP:  {metrics.MemVpp:F3} V"
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
        var (processorName, partNumbers, manufacturers, motherboardProductName, biosVersion, biosReleaseDate, modules) = ReadDmidecode();
        var codenameIndex = ReadCodenameIndex();

        var cpu = new CpuInfoModel
        {
            Name = "AMD Ryzen (from ryzen_smu)",
            ProcessorName = processorName,
            CodeName = MapCodename(codenameIndex),
            SmuVersion = ReadString("version"),
            Package = string.Empty
        };

        var metrics = ReadMetrics(codenameIndex);

        var isGraniteRidge = codenameIndex == 23;
        var dramTimings = isGraniteRidge
            ? ReadGraniteRidgeDdr5Timings()
            : new DramTimingsModel();

        var memory = new MemoryConfigModel
        {
            Frequency = isGraniteRidge ? dramTimings.FrequencyHintMHz : 0,
            Type = isGraniteRidge ? MemType.DDR5 : MemType.Unknown,
            TotalCapacity = ReadTotalMemory(),
            Manufacturer = manufacturers,
            PartNumber = partNumbers
        };

        var boardInfo = new BoardInfoModel
        {
            MotherboardProductName = motherboardProductName,
            BiosVersion = biosVersion,
            BiosReleaseDate = biosReleaseDate
        };

        var fans = ReadHwmonFans();

        return new SystemSummary
        {
            Cpu = cpu,
            Memory = memory,
            BoardInfo = boardInfo,
            Modules = modules,
            Metrics = metrics,
            DramTimings = dramTimings,
            Fans = fans
        };
    }

    /// <summary>Read fan1–fan7 from hwmon device named nct6799; fan7 = Pump; exclude 0 RPM.</summary>
    private static IReadOnlyList<FanReading> ReadHwmonFans()
    {
        const string hwmonRoot = "/sys/class/hwmon";
        if (!Directory.Exists(hwmonRoot)) return Array.Empty<FanReading>();

        string? basePath = null;
        foreach (var dir in Directory.GetDirectories(hwmonRoot))
        {
            var namePath = Path.Combine(dir, "name");
            if (!File.Exists(namePath)) continue;
            var name = File.ReadAllText(namePath).Trim();
            if (name.Contains("nct6799", StringComparison.OrdinalIgnoreCase))
            {
                basePath = dir;
                break;
            }
        }
        if (string.IsNullOrEmpty(basePath)) return Array.Empty<FanReading>();

        var list = new List<FanReading>();
        for (int i = 1; i <= 7; i++)
        {
            var path = Path.Combine(basePath, $"fan{i}_input");
            if (!File.Exists(path)) continue;
            if (!int.TryParse(File.ReadAllText(path).Trim(), out var rpm) || rpm <= 0) continue;
            var label = i == 7 ? "Pump" : $"Fan{i}";
            list.Add(new FanReading(label, rpm));
        }
        return list;
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
    private static (string ProcessorName, string PartNumbers, string Manufacturers, string MotherboardProductName, string BiosVersion, string BiosReleaseDate, IReadOnlyList<MemoryModule> Modules) ReadDmidecode()
    {
        if (_dmidecodeCached && _cachedDmidecode.HasValue)
            return _cachedDmidecode.Value;

        var processorName = string.Empty;
        var partNumbers = new List<string>();
        var manufacturerStr = string.Empty;
        var motherboardProductName = string.Empty;
        var biosVersion = string.Empty;
        var biosReleaseDate = string.Empty;
        IReadOnlyList<MemoryModule> modules = Array.Empty<MemoryModule>();

        try
        {
            processorName = RunDmidecodeAndParse("processor", ParseProcessor).Trim();
            var memStdout = RunDmidecodeAndParse("memory", s => s);
            var (parsedModules, parsedPartNumbers, manufacturers) = ParseMemoryDevices(memStdout);
            modules = parsedModules;
            partNumbers = parsedPartNumbers;
            manufacturerStr = string.Join(", ", manufacturers);

            motherboardProductName = RunDmidecodeString("baseboard-product-name");
            biosVersion = RunDmidecodeString("bios-version");
            biosReleaseDate = RunDmidecodeString("bios-release-date");
        }
        catch
        {
            // dmidecode may require root; ignore
        }

        var result = (processorName, string.Join(", ", partNumbers), manufacturerStr, motherboardProductName, biosVersion, biosReleaseDate, modules);
        _cachedDmidecode = result;
        _dmidecodeCached = true;
        return result;
    }

    /// <summary>Parses dmidecode -t 17 output; returns list of installed modules and aggregated part numbers/manufacturers.</summary>
    private static (List<MemoryModule> Modules, List<string> PartNumbers, List<string> Manufacturers) ParseMemoryDevices(string stdout)
    {
        var modules = new List<MemoryModule>();
        var partNumbers = new List<string>();
        var manufacturers = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var lines = stdout.Split('\n');
        var inMem = false;
        string? size = null, locator = null, bankLocator = null, manufacturer = null, partNumber = null, serialNumber = null;
        MemRank rank = MemRank.SR;

        void FlushBlock()
        {
            if (string.IsNullOrEmpty(size) || size.Contains("No Module", StringComparison.OrdinalIgnoreCase))
                return;
            ulong capacityBytes = ParseSizeToBytes(size);
            var mod = new MemoryModule
            {
                DeviceLocator = locator ?? string.Empty,
                BankLabel = bankLocator ?? string.Empty,
                Slot = locator ?? string.Empty,
                CapacityBytes = capacityBytes,
                Manufacturer = manufacturer ?? string.Empty,
                PartNumber = partNumber ?? string.Empty,
                SerialNumber = serialNumber ?? string.Empty,
                Rank = rank
            };
            modules.Add(mod);
            if (!string.IsNullOrEmpty(mod.PartNumber) && mod.PartNumber != "Unknown" && mod.PartNumber != "NO DIMM")
                partNumbers.Add(mod.PartNumber);
            if (!string.IsNullOrEmpty(mod.Manufacturer) && mod.Manufacturer != "Unknown" && mod.Manufacturer != "NO DIMM")
                manufacturers.Add(mod.Manufacturer);
        }

        foreach (var line in lines)
        {
            if (line.Contains("Memory Device", StringComparison.OrdinalIgnoreCase))
            {
                FlushBlock();
                size = null; locator = null; bankLocator = null; manufacturer = null; partNumber = null; serialNumber = null;
                rank = MemRank.SR;
                inMem = true;
                continue;
            }
            if (inMem && line.StartsWith("\t", StringComparison.Ordinal) == false && line.Trim().Length > 0)
                inMem = false;
            if (!inMem) continue;

            var trimmed = line.TrimStart();
            if (trimmed.StartsWith("Size:", StringComparison.OrdinalIgnoreCase))
                size = GetValueAfterColon(line);
            else if (trimmed.StartsWith("Locator:", StringComparison.OrdinalIgnoreCase))
                locator = GetValueAfterColon(line);
            else if (trimmed.StartsWith("Bank Locator:", StringComparison.OrdinalIgnoreCase))
                bankLocator = GetValueAfterColon(line);
            else if (trimmed.StartsWith("Manufacturer:", StringComparison.OrdinalIgnoreCase))
                manufacturer = GetValueAfterColon(line);
            else if (trimmed.StartsWith("Part Number:", StringComparison.OrdinalIgnoreCase))
                partNumber = GetValueAfterColon(line);
            else if (trimmed.StartsWith("Serial Number:", StringComparison.OrdinalIgnoreCase))
                serialNumber = GetValueAfterColon(line);
            else if (trimmed.StartsWith("Rank:", StringComparison.OrdinalIgnoreCase))
            {
                var rankStr = GetValueAfterColon(line);
                rank = rankStr switch
                {
                    "1" => MemRank.SR,
                    "2" => MemRank.DR,
                    "4" => MemRank.QR,
                    _ => MemRank.SR
                };
            }
        }
        FlushBlock();

        // Sort by channel (A before B) so module index matches UMC0/UMC1 and PhyRdlPerChannel
        static int ChannelOrder(string? bankLoc)
        {
            if (string.IsNullOrEmpty(bankLoc)) return 0;
            if (bankLoc.Contains("CHANNEL B", StringComparison.OrdinalIgnoreCase)) return 1;
            if (bankLoc.Contains("CHANNEL A", StringComparison.OrdinalIgnoreCase)) return 0;
            return 0;
        }
        modules.Sort((a, b) => ChannelOrder(a.BankLabel).CompareTo(ChannelOrder(b.BankLabel)));

        return (modules, partNumbers, manufacturers.ToList());
    }

    private static string GetValueAfterColon(string line)
    {
        var colon = line.IndexOf(':', StringComparison.Ordinal);
        return colon >= 0 ? line[(colon + 1)..].Trim() : string.Empty;
    }

    private static ulong ParseSizeToBytes(string size)
    {
        if (string.IsNullOrWhiteSpace(size)) return 0;
        var parts = size.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length < 2) return 0;
        if (!double.TryParse(parts[0], out var value)) return 0;
        var unit = parts[1].AsSpan();
        if (unit.StartsWith("GiB", StringComparison.OrdinalIgnoreCase) || unit.StartsWith("GB", StringComparison.OrdinalIgnoreCase))
            return (ulong)(value * 1024 * 1024 * 1024);
        if (unit.StartsWith("MiB", StringComparison.OrdinalIgnoreCase) || unit.StartsWith("MB", StringComparison.OrdinalIgnoreCase))
            return (ulong)(value * 1024 * 1024);
        if (unit.StartsWith("KiB", StringComparison.OrdinalIgnoreCase) || unit.StartsWith("KB", StringComparison.OrdinalIgnoreCase))
            return (ulong)(value * 1024);
        return 0;
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

    private static SmuMetrics ReadMetrics(int codenameIndex)
    {
        // Best-effort: if pm_table is present, read a few floats from it.
        var pmTablePath = Path.Combine(BasePath, "pm_table");
        if (!File.Exists(pmTablePath))
        {
            if (!_hasDumpedDiagnostic)
            {
                DumpDiagnostic("pm_table file does not exist", codenameIndex);
                _hasDumpedDiagnostic = true;
            }
            return new SmuMetrics();
        }

        try
        {
            // Sysfs can occasionally return stale/empty data on first read; retry once if needed.
            byte[] bytes = File.ReadAllBytes(pmTablePath);
            if (bytes.Length < 4)
            {
                return new SmuMetrics();
            }

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
                    // If still zeros, try Python (same logic as dump_pm_voltages.py)
                    if (baseMetrics.FclkMHz == 0 && baseMetrics.Vsoc == 0)
                    {
                        var pyMetrics = TryReadPmTableViaPython();
                        if (pyMetrics is { } m && (m.FclkMHz > 0 || m.Vsoc > 0))
                            baseMetrics = m;
                    }
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
                    MemoryClockMHz = memClock
                };
            }

            // Overlay with zenpower3 hwmon values if available.
            var metrics = ApplyZenpowerOverrides(baseMetrics);
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

            return metrics;
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
                    return ApplyK10TempTctlTccdOverlay(ApplyK10TempCoreTempFallback(ApplyZenpowerOverrides(m)));
            }
            return new SmuMetrics();
        }
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
        if (!File.Exists(path)) return 0;
        var bytes = File.ReadAllBytes(path);
        if (bytes.Length < 4) return 0;
        return BitConverter.ToUInt32(bytes, 0);
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
        // PM table layout is version-dependent. Try known offsets from ryzen_smu monitor_cpu (0x240903) and common alternatives.
        // SOCKET_POWER = index 29, CPU_TELEMETRY_POWER = 42, CPU_TELEMETRY_CURRENT = 41; some tables use 0/1/2 for power/temp/core.
        float packagePower = TryPlausiblePower(pt);
        float coreClock = TryPlausibleCoreClock(pt);
        float packageCurrent = TryPlausibleCurrent(pt);
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

        // PM table index 271 = core voltage (Granite Ridge, from watch_pm_table)
        float vcore = 271 < pt.Length ? PlausibleVcore(pt[271]) : 0f;
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
            VddMisc = vddMisc
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

    /// <summary>Return value if in plausible core voltage range (0.25–2.2 V), else 0.</summary>
    private static float PlausibleVcore(float v)
    {
        return v >= 0.25f && v <= 2.2f ? v : 0f;
    }

    // SVI2 telemetry SMN addresses. Family 17h (Zen1–Zen3) from zenpower/LibreHardwareMonitor.
    // Zen 5 (Family 1Ah / Granite Ridge): no public SVI2 spec; we try the same base in case SMU kept compatibility.
    private const uint Svi2Plane0Addr = 0x0005A00C; // Core for Zen1, SoC for Zen2 Ryzen
    private const uint Svi2Plane1Addr = 0x0005A010; // SoC for Zen1, Core for Zen2 Ryzen

    /// <summary>Read SVI2 telemetry via SMN (voltage/current). Formulas from zenpower; Zen2 current scale. Zen 5: same addresses tried.</summary>
    private static (float CoreV, float CoreA, float SocV, float SocA)? TryReadSvi2Telemetry()
    {
        try
        {
            uint p0 = ReadSmn(Svi2Plane0Addr);
            uint p1 = ReadSmn(Svi2Plane1Addr);

            static float PlaneToVoltageMV(uint plane)
            {
                uint vddCor = (plane >> 16) & 0xFF;
                return 1550f - (625f * vddCor / 100f); // mV
            }

            // Zen2 current scale (658.823 * idd); idd = low 8 bits. Result mA.
            static float PlaneToCurrentMA(uint plane)
            {
                uint idd = plane & 0xFF;
                return (658823f * idd) / 1000f;
            }

            float v0 = PlaneToVoltageMV(p0) / 1000f;
            float a0 = PlaneToCurrentMA(p0) / 1000f;
            float v1 = PlaneToVoltageMV(p1) / 1000f;
            float a1 = PlaneToCurrentMA(p1) / 1000f;

            // Plausible: V 0.3–2 V, A 0.01–200 A
            bool ok0 = v0 >= 0.3f && v0 <= 2f && a0 >= 0.01f && a0 <= 200f;
            bool ok1 = v1 >= 0.3f && v1 <= 2f && a1 >= 0.01f && a1 <= 200f;
            if (!ok0 && !ok1) return null;

            // Report plane0 as Core, plane1 as SoC (Zen1 order); on Zen2 they're swapped but both rails are valid
            return (v0, a0, v1, a1);
        }
        catch
        {
            return null;
        }
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

            float cpuVddio = metrics.CpuVddio;
            float memVdd = metrics.MemVdd;
            float memVddq = metrics.MemVddq;
            float memVpp = metrics.MemVpp;
            float vsoc = metrics.Vsoc;
            float powerW = metrics.CpuPackagePowerWatts;
            float currentA = metrics.CpuPackageCurrentAmps;

            // power1_input = microwatts, curr1_input = milliamps (hwmon convention)
            TryReadHwmonFloat(zpPath, "power1_input", out var powerUw);
            if (powerUw > 0) powerW = powerUw / 1_000_000f;
            TryReadHwmonFloat(zpPath, "curr1_input", out var currentMa);
            if (currentMa > 0) currentA = currentMa / 1000f;

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

                if (label.Contains("vddcr_cpu") || (label.Contains("core") && cpuVddio == 0))
                    cpuVddio = v;
                else if (label.Contains("vddcr_soc") || label.Contains("vsoc"))
                    vsoc = v;
                else if (label.Contains("vddio_mem") || label.Contains("vddmem") || label.Contains("mem vdd"))
                    memVdd = v;
                else if (label.Contains("vddq") && label.Contains("mem"))
                    memVddq = v;
                else if (label.Contains("vpp") && label.Contains("mem"))
                    memVpp = v;
            }

            return new SmuMetrics
            {
                CpuPackagePowerWatts = powerW,
                CpuPptWatts = metrics.CpuPptWatts,
                CpuPackageCurrentAmps = currentA,
                Vcore = metrics.Vcore,
                CpuTempCelsius = metrics.CpuTempCelsius,
                CoreTempsCelsius = metrics.CoreTempsCelsius,
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

    private static bool TryReadHwmonFloat(string hwmonDir, string file, out float value)
    {
        value = 0;
        var path = Path.Combine(hwmonDir, file);
        if (!File.Exists(path)) return false;
        return float.TryParse(File.ReadAllText(path).Trim(), out value);
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

    /// <summary>If PM table core temps are empty or all zero, fill from k10temp CCD temps (alternative source).</summary>
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

        var fromK10 = ReadK10TempCoreTemps();
        if (fromK10.Count == 0) return metrics;

        return new SmuMetrics
        {
            CpuPackagePowerWatts = metrics.CpuPackagePowerWatts,
            CpuPptWatts = metrics.CpuPptWatts,
            CpuPackageCurrentAmps = metrics.CpuPackageCurrentAmps,
            Vcore = metrics.Vcore,
            CpuTempCelsius = metrics.CpuTempCelsius,
            CoreTempsCelsius = fromK10,
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

    private static bool TryReadTempInput(string hwmonDir, int index, out float celsius)
    {
        celsius = 0;
        var path = Path.Combine(hwmonDir, $"temp{index}_input");
        if (!File.Exists(path)) return false;
        if (!int.TryParse(File.ReadAllText(path).Trim(), out var raw)) return false;
        celsius = raw / 1000f;
        return celsius >= 0f && celsius <= 150f;
    }

    /// <summary>Overlay Tctl, Tccd1, Tccd2 from k10temp onto metrics. Each only set when k10temp exposes that channel.</summary>
    private static SmuMetrics ApplyK10TempTctlTccdOverlay(SmuMetrics metrics)
    {
        var (tctl, tccd1, tccd2) = ReadK10TempTctlTccd();
        if (!tctl.HasValue && !tccd1.HasValue && !tccd2.HasValue) return metrics;

        return new SmuMetrics
        {
            CpuPackagePowerWatts = metrics.CpuPackagePowerWatts,
            CpuPptWatts = metrics.CpuPptWatts,
            CpuPackageCurrentAmps = metrics.CpuPackageCurrentAmps,
            Vcore = metrics.Vcore,
            CpuTempCelsius = metrics.CpuTempCelsius,
            CoreTempsCelsius = metrics.CoreTempsCelsius,
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

            // tPHYRDL per UMC for per-DIMM display (UMC0 = offset 0, UMC1 = 0x10000)
            const uint umc1Offset = 0x10000u;
            uint reg50258Umc1 = ReadSmn(umc1Offset | 0x50258);
            uint phyRdlUmc0 = BitSlice(reg50258, 23, 16);
            uint phyRdlUmc1 = BitSlice(reg50258Umc1, 23, 16);
            var phyRdlPerChannel = new uint[] { phyRdlUmc0, phyRdlUmc1 };

            uint tcl = BitSlice(reg50204, 5, 0);
            uint trcdRd = BitSlice(reg50204, 21, 16);
            uint trcdWr = BitSlice(reg50204, 29, 24);
            if (trcdWr == 0) trcdWr = trcdRd; // fallback when not split in register
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
                PhyRdlPerChannel = phyRdlPerChannel,
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

