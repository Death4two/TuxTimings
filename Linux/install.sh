#!/bin/bash
# Build, package, and install TuxTimings (C/GTK4) as an AppImage.
# Requires: gcc, pkg-config, gtk4-devel, appimagetool (auto-downloaded if missing)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APPIMAGE_NAME="TuxTimings-x86_64.AppImage"
APPIMAGE="$SCRIPT_DIR/$APPIMAGE_NAME"
INSTALL_DIR="/opt/TuxTimings"
APPIMAGE_DIR="$SCRIPT_DIR/appimage"
APPDIR="$SCRIPT_DIR/AppDir"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
MAKE=/usr/bin/make

REAL_USER="${SUDO_USER:-$(whoami)}"
REAL_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)

# When re-invoked with sudo for installation, skip straight to install
INSTALL_ONLY=0
if [ "$1" = "--install-only" ]; then
    INSTALL_ONLY=1
    shift
fi

if [ "$INSTALL_ONLY" -eq 0 ]; then

# ── Download appimagetool if missing ────────────────────────────────────

if ! command -v appimagetool &>/dev/null && \
   [ ! -f "$SCRIPT_DIR/appimagetool-x86_64.AppImage" ]; then
    echo "==> Downloading appimagetool..."
    curl -L -o "$SCRIPT_DIR/appimagetool-x86_64.AppImage" "$APPIMAGETOOL_URL"
    chmod +x "$SCRIPT_DIR/appimagetool-x86_64.AppImage"
    if [ "$(id -u)" -eq 0 ]; then
        chown "$REAL_USER":"$REAL_USER" "$SCRIPT_DIR/appimagetool-x86_64.AppImage"
    fi
fi

# ── Generate appimage assets ────────────────────────────────────────────

mkdir -p "$APPIMAGE_DIR"

cat > "$APPIMAGE_DIR/AppRun" << 'APPRUN_EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# If already root, just run directly
if [ "$(id -u)" -eq 0 ]; then
    exec "$HERE/usr/bin/tuxtimings" "$@"
fi

# Build --env-VAR=VALUE args for the binary to restore after pkexec
ENV_ARGS=""
for VAR in DISPLAY WAYLAND_DISPLAY XDG_RUNTIME_DIR XAUTHORITY \
           DBUS_SESSION_BUS_ADDRESS XDG_CONFIG_HOME HOME; do
    eval VAL=\$$VAR
    [ -n "$VAL" ] && ENV_ARGS="$ENV_ARGS --env-$VAR=$VAL"
done

# If on Wayland, tell GTK4 to use it natively
if [ -n "$WAYLAND_DISPLAY" ]; then
    ENV_ARGS="$ENV_ARGS --env-GDK_BACKEND=wayland"
fi

# Use installed binary (matches polkit policy exec.path)
if [ -x /opt/TuxTimings/bin/tuxtimings ]; then
    exec pkexec /opt/TuxTimings/bin/tuxtimings $ENV_ARGS "$@"
else
    exec pkexec "$HERE/usr/bin/tuxtimings" $ENV_ARGS "$@"
fi
APPRUN_EOF
chmod +x "$APPIMAGE_DIR/AppRun"

if [ ! -f "$APPIMAGE_DIR/tuxtimings.desktop" ]; then
    cat > "$APPIMAGE_DIR/tuxtimings.desktop" << 'EOF'
[Desktop Entry]
Name=TuxTimings
Comment=AMD Ryzen DRAM timings viewer
Exec=/opt/TuxTimings/TuxTimings-x86_64.AppImage
Icon=tuxtimings
Terminal=false
Type=Application
Categories=Utility;
EOF
fi

if [ -f "$SCRIPT_DIR/tuxtimings.png" ] && [ ! -f "$APPIMAGE_DIR/tuxtimings.png" ]; then
    cp "$SCRIPT_DIR/tuxtimings.png" "$APPIMAGE_DIR/tuxtimings.png"
fi

# ── Build the C binary ──────────────────────────────────────────────────

echo "==> Building tuxtimings (C/GTK4)..."
if [ -d "$APPDIR" ] && ! rm -rf "$APPDIR" 2>/dev/null; then
    sudo rm -rf "$APPDIR"
fi
rm -f "$APPIMAGE"

# Build as real user if running as root (avoid root-owned object files)
if [ "$(id -u)" -eq 0 ]; then
    su "$REAL_USER" -c "'$MAKE' -C '$SCRIPT_DIR' clean all"
else
    "$MAKE" -C "$SCRIPT_DIR" clean all
fi

# ── Pack AppImage ───────────────────────────────────────────────────────

echo "==> Preparing AppDir..."
mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/share/polkit-1/actions"

cp "$SCRIPT_DIR/tuxtimings" "$APPDIR/usr/bin/"
chmod +x "$APPDIR/usr/bin/tuxtimings"
if [ -f "$APPIMAGE_DIR/com.tuxtimings.policy" ]; then
    cp "$APPIMAGE_DIR/com.tuxtimings.policy" "$APPDIR/usr/share/polkit-1/actions/"
fi
cp "$APPIMAGE_DIR/AppRun" "$APPDIR/"
cp "$APPIMAGE_DIR/tuxtimings.desktop" "$APPDIR/"
if [ -f "$APPIMAGE_DIR/tuxtimings.png" ]; then
    cp "$APPIMAGE_DIR/tuxtimings.png" "$APPDIR/tuxtimings.png"
fi
chmod +x "$APPDIR/AppRun"

echo "==> Creating AppImage..."
BUILD_DIR="/tmp/tuxtimings-appimage-build.$$"
mkdir -p "$BUILD_DIR"
cp -a "$APPDIR" "$BUILD_DIR/AppDir"
set +e
if command -v appimagetool &>/dev/null; then
    (cd "$BUILD_DIR" && ARCH=x86_64 appimagetool AppDir "$APPIMAGE_NAME"); APPIMAGETOOL_EXIT=$?
elif [ -x "$SCRIPT_DIR/appimagetool-x86_64.AppImage" ]; then
    (cd "$BUILD_DIR" && ARCH=x86_64 "$SCRIPT_DIR/appimagetool-x86_64.AppImage" AppDir "$APPIMAGE_NAME"); APPIMAGETOOL_EXIT=$?
else
    set -e
    rm -rf "$BUILD_DIR"
    echo "ERROR: appimagetool not found. Install it or download from:"
    echo "  https://github.com/AppImage/AppImageKit/releases"
    exit 1
fi
set -e
[ "${APPIMAGETOOL_EXIT-0}" -ne 0 ] && echo "==> appimagetool exited with $APPIMAGETOOL_EXIT (checking for created AppImage anyway)..."

CREATED=""
[ -f "$BUILD_DIR/$APPIMAGE_NAME" ] && CREATED="$BUILD_DIR/$APPIMAGE_NAME"
if [ -z "$CREATED" ]; then
    for f in "$BUILD_DIR"/*.AppImage; do
        [ -e "$f" ] && CREATED="$f" && break
    done
fi
if [ -n "$CREATED" ]; then
    mv -f "$CREATED" "$APPIMAGE"
    rm -rf "$BUILD_DIR"
    echo "==> AppImage created: $APPIMAGE"
else
    echo "ERROR: appimagetool did not produce an AppImage."
    ls -la "$BUILD_DIR"
    rm -rf "$BUILD_DIR"
    exit 1
fi

fi # end INSTALL_ONLY check

# ── Install to system (needs root) ──────────────────────────────────────

if [ "$(id -u)" -ne 0 ]; then
    echo "==> Elevating to root for system installation..."
    exec sudo "$0" --install-only "$@"
fi

echo "==> Installing TuxTimings to system..."

# Binary
mkdir -p "$INSTALL_DIR/bin"
cp "$SCRIPT_DIR/tuxtimings" "$INSTALL_DIR/bin/"
chmod +x "$INSTALL_DIR/bin/tuxtimings"

# AppImage
mkdir -p "$INSTALL_DIR"
cp "$APPIMAGE" "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/$APPIMAGE_NAME"

# Polkit policy
if [ -f "$APPIMAGE_DIR/com.tuxtimings.policy" ]; then
    cp "$APPIMAGE_DIR/com.tuxtimings.policy" /usr/share/polkit-1/actions/
fi

# Desktop file
if [ -f "$APPIMAGE_DIR/tuxtimings.desktop" ]; then
    cp "$APPIMAGE_DIR/tuxtimings.desktop" /usr/share/applications/
fi

# Desktop shortcut
DESKTOP_DIR="${REAL_HOME}/Desktop"
[ ! -d "$DESKTOP_DIR" ] && \
    DESKTOP_DIR=$(su "$REAL_USER" -c 'xdg-user-dir DESKTOP 2>/dev/null' || true)
if [ -d "$DESKTOP_DIR" ] && [ -f "$APPIMAGE_DIR/tuxtimings.desktop" ]; then
    cp "$APPIMAGE_DIR/tuxtimings.desktop" "$DESKTOP_DIR/"
    chown "$REAL_USER":"$REAL_USER" "$DESKTOP_DIR/tuxtimings.desktop"
    chmod +x "$DESKTOP_DIR/tuxtimings.desktop"
fi

# Icon
if [ -f "$SCRIPT_DIR/tuxtimings.png" ]; then
    mkdir -p /usr/share/icons/hicolor/256x256/apps
    cp "$SCRIPT_DIR/tuxtimings.png" /usr/share/icons/hicolor/256x256/apps/
    gtk-update-icon-cache /usr/share/icons/hicolor/ 2>/dev/null || true
fi

echo "==> Installation complete!"
echo "    AppImage:  $INSTALL_DIR/$APPIMAGE_NAME"
echo "    Binary:    $INSTALL_DIR/bin/tuxtimings"
echo "    Policy:    /usr/share/polkit-1/actions/com.tuxtimings.policy"
echo "    Desktop:   /usr/share/applications/tuxtimings.desktop"
