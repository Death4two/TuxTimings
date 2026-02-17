## Requirements

- **Runtime (for users):**
  - **ryzen_smu** kernel module loaded (see [ryzen_smu](https://github.com/amkillam/ryzen_smu/) for building and loading).
  - **Root/sudo** necessary so the app can read SMN, PM table, and dmidecode.

- **Build (for developers):**
  - **.NET 8 SDK** (e.g. on Arch: `sudo pacman -S dotnet-sdk`).
  - For AppImage: **appimagetool** (see [AppImageKit releases](https://github.com/AppImage/AppImageKit/releases); or AUR `appimage-tool`).

---

## Run from source

From the `Linux/` directory:

```bash
sudo dotnet run --project TuxTimings.LinuxUI
```

For a self-contained publish (no system .NET needed to run):

```bash
dotnet publish TuxTimings.LinuxUI -c Release -r linux-x64 --self-contained true -o publish
./publish/TuxTimings.LinuxUI
```

---

## Build AppImage

From the `Linux/` directory:

```bash
chmod +x build-appimage.sh
sudo ./build-appimage.sh
```

You need **appimagetool** in your PATH (or `appimagetool-x86_64.AppImage` in the `Linux/` directory). The build does not require sudo.

This will:

1. Publish the app as a self-contained `linux-x64` build into `publish/`.
2. Prepare `AppDir/` with `AppRun`, desktop file, and binaries under `usr/bin/`.
3. Run appimagetool (in a temporary directory) and place **`TuxTimings-x86_64.AppImage`** in `Linux/`.

**Run the AppImage:**

```bash
sudo ./TuxTimings-x86_64.AppImage
```

Use **sudo** for full data (SMN, PM table, dmidecode): `sudo ./TuxTimings-x86_64.AppImage`. Make it executable if needed: `chmod +x TuxTimings-x86_64.AppImage`. No .NET runtime is required; the AppImage is self-contained.

### If it doesn’t open from the desktop

1. **Make it executable**  
   After copying or downloading, the file may lose execute permission:
   ```bash
   chmod +x TuxTimings-x86_64.AppImage
   ```
   Or in the file manager: right‑click → Properties → Permissions → allow “Execute”.

2. **Run from a terminal first**  
   Double‑click might do nothing. Run from a terminal to see errors:
   ```bash
   sudo ./TuxTimings-x86_64.AppImage
   ```
   If you see “Permission denied”, fix execute permission. If you see FUSE/mount errors, install FUSE (e.g. `sudo pacman -S fuse2`).

3. **Use “Run” in the file manager**  
   Some desktops don’t execute on double‑click; use right‑click → **Run** (or **Execute** / **Run as program**).

---

## Project layout

- **TuxTimings.Core** – Shared domain models and view logic.
- **TuxTimings.LinuxBackend** – ryzen_smu (SMN/PM table) and dmidecode access.
- **TuxTimings.LinuxUI** – Avalonia UI (main window, timings/voltages layout).
- **appimage/** – `AppRun` and `tuxtimings.desktop` for the AppImage.
- **scripts/** – Helper scripts (e.g. `parse_pm_table.py`).
