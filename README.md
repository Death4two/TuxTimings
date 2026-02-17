# TuxTimings

![TuxTimings](screenshot.png "TuxTimings")

### Supported CPUs

At the moment, **only Zen 5 Granite Ridge desktop CPUs** (e.g. Ryzen 9000‑series AM5) are fully supported for PM‑table based telemetry. Other AMD families may start, but sensors and timings are not guaranteed to be correct yet.

### Building

See [Linux/README.md](Linux/README.md). Requires .NET 8 SDK. The app reads data via the **ryzen_smu** kernel module (build it from [GitHub](https://github.com/amkillam/ryzen_smu/) and load at runtime).

### License

This project is licensed under the **GNU General Public License v3.0**. See [LICENSE](LICENSE) for the full text.

### References and projects used  

- **[ryzen_smu](https://github.com/amkillam/ryzen_smu/)** — Kernel module for reading AMD SMN and PM table; build and load separately at runtime.
- **[ZenStates-Core](https://github.com/irusanov/ZenStates-Core)** — PM table offsets and timing formulas (reimplemented in our Linux backend; used with permission).
- **[ZenTimings](https://github.com/nickspacewalker/ZenTimings)** — Windows version; TuxTimings is based on this concept.
- **[Avalonia](https://avaloniaui.net/)** — Cross-platform .NET UI framework (Avalonia 11, Fluent theme, ReactiveUI).
- **[.NET](https://dotnet.microsoft.com/)** — Runtime and SDK (net8.0).
- **[Linux kernel](https://github.com/torvalds/linux)** — SMN/sysfs interface and platform support.
- **[AMD's public documentation](https://www.amd.com/en/support/tech-docs)** — SMN/PM table and DRAM timing references.
- **[AppImageKit](https://github.com/AppImage/AppImageKit)** — Used to build the AppImage (see [Linux/README.md](Linux/README.md)).  
- **Tux icon** — Tux the penguin originally by Larry Ewing, created with GIMP (`lewing@isc.tamu.edu`), used and/or modified under the terms of the original image permission.
