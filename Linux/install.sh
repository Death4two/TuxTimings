#!/bin/bash
# Build and install TuxTimings (C/GTK4) natively.
# Requires: gcc, pkg-config, gtk4 development headers
#
# Usage:
#   ./install.sh              Build and install to system
#   ./install.sh --uninstall  Remove all installed files
#   ./install.sh --deb        Build a .deb package (Ubuntu/Debian)
#
# On Arch-based distros, prefer:  makepkg -si  (from the repo root)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
INSTALL_DIR="/opt/TuxTimings"
MAKE=/usr/bin/make

GITHUB_REPO="Death4two/TuxTimings"
REAL_USER="${SUDO_USER:-$(whoami)}"
REAL_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)

# ── Uninstall ─────────────────────────────────────────────────────────
if [ "$1" = "--uninstall" ]; then
    if [ "$(id -u)" -ne 0 ]; then
        exec sudo "$0" --uninstall
    fi
    echo "==> Uninstalling TuxTimings..."

    rm -rf "$INSTALL_DIR"
    rm -f /usr/bin/tuxtimings
    rm -f /usr/share/polkit-1/actions/com.tuxtimings.policy
    rm -f /usr/share/applications/tuxtimings.desktop
    rm -f /usr/share/icons/hicolor/256x256/apps/tuxtimings.png
    gtk-update-icon-cache /usr/share/icons/hicolor/ 2>/dev/null || true

    # Desktop shortcut
    DESKTOP_DIR="${REAL_HOME}/Desktop"
    [ ! -d "$DESKTOP_DIR" ] && \
        DESKTOP_DIR=$(su "$REAL_USER" -c 'xdg-user-dir DESKTOP 2>/dev/null' || true)
    if [ -d "$DESKTOP_DIR" ]; then
        rm -f "$DESKTOP_DIR/tuxtimings.desktop"
    fi

    echo "==> TuxTimings has been uninstalled."
    exit 0
fi

# ── Check for updates ─────────────────────────────────────────────────
check_for_update() {
    if ! command -v git &>/dev/null; then
        return
    fi
    if ! git -C "$SCRIPT_DIR" rev-parse --is-inside-work-tree &>/dev/null; then
        return
    fi

    echo "==> Checking for updates..."
    local LOCAL_HEAD
    LOCAL_HEAD=$(git -C "$SCRIPT_DIR" rev-parse HEAD 2>/dev/null) || return
    git -C "$SCRIPT_DIR" fetch origin main --quiet 2>/dev/null || return
    local REMOTE_HEAD
    REMOTE_HEAD=$(git -C "$SCRIPT_DIR" rev-parse origin/main 2>/dev/null) || return

    if [ "$LOCAL_HEAD" != "$REMOTE_HEAD" ]; then
        echo "==> A newer version is available."
        read -rp "    Update now? [Y/n] " answer
        case "$answer" in
            [nN]*) echo "    Skipping update." ;;
            *)
                echo "==> Pulling latest changes..."
                if [ "$(id -u)" -eq 0 ]; then
                    su "$REAL_USER" -c "git -C '$SCRIPT_DIR' pull --ff-only origin main"
                else
                    git -C "$SCRIPT_DIR" pull --ff-only origin main
                fi
                echo "==> Updated. Restarting install..."
                exec "$0" "$@"
                ;;
        esac
    else
        echo "    Already up to date."
    fi
}

check_for_update "$@"

# ── Build .deb package ────────────────────────────────────────────────
if [ "$1" = "--deb" ]; then
    echo "==> Building .deb package..."

    # Build binary first
    if [ "$(id -u)" -eq 0 ]; then
        su "$REAL_USER" -c "'$MAKE' -C '$SCRIPT_DIR' clean all"
    else
        "$MAKE" -C "$SCRIPT_DIR" clean all
    fi

    PKG_VERSION="1.0.0"
    DEB_ROOT="$SCRIPT_DIR/deb-build/tuxtimings_${PKG_VERSION}_amd64"
    rm -rf "$SCRIPT_DIR/deb-build"
    mkdir -p "$DEB_ROOT/DEBIAN"
    mkdir -p "$DEB_ROOT/opt/TuxTimings/bin"
    mkdir -p "$DEB_ROOT/usr/bin"
    mkdir -p "$DEB_ROOT/usr/share/polkit-1/actions"
    mkdir -p "$DEB_ROOT/usr/share/applications"
    mkdir -p "$DEB_ROOT/usr/share/icons/hicolor/256x256/apps"

    # Control file
    cat > "$DEB_ROOT/DEBIAN/control" << EOF
Package: tuxtimings
Version: $PKG_VERSION
Section: utils
Priority: optional
Architecture: amd64
Depends: libgtk-4-1, dmidecode
Maintainer: Death4two <https://github.com/Death4two>
Description: AMD Ryzen DRAM timings and CPU telemetry viewer (GTK4)
 Displays real-time DRAM timings, CPU frequencies, temperatures,
 and other telemetry data for AMD Ryzen processors.
EOF

    # Binary
    install -m755 "$SCRIPT_DIR/tuxtimings" "$DEB_ROOT/opt/TuxTimings/bin/tuxtimings"

    # Launcher script
    cat > "$DEB_ROOT/usr/bin/tuxtimings" << 'LAUNCHER'
#!/bin/bash
if [ "$(id -u)" -eq 0 ]; then
    exec /opt/TuxTimings/bin/tuxtimings "$@"
fi
ENV_ARGS=""
for VAR in DISPLAY WAYLAND_DISPLAY XDG_RUNTIME_DIR XAUTHORITY \
           DBUS_SESSION_BUS_ADDRESS XDG_CONFIG_HOME HOME; do
    eval VAL=\$$VAR
    [ -n "$VAL" ] && ENV_ARGS="$ENV_ARGS --env-$VAR=$VAL"
done
[ -n "$WAYLAND_DISPLAY" ] && ENV_ARGS="$ENV_ARGS --env-GDK_BACKEND=wayland"
exec pkexec /opt/TuxTimings/bin/tuxtimings $ENV_ARGS "$@"
LAUNCHER
    chmod 755 "$DEB_ROOT/usr/bin/tuxtimings"

    # Polkit policy
    cat > "$DEB_ROOT/usr/share/polkit-1/actions/com.tuxtimings.policy" << 'POLICY'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE policyconfig PUBLIC
 "-//freedesktop//DTD PolicyKit Policy Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/PolicyKit/1/policyconfig.dtd">
<policyconfig>
  <action id="com.tuxtimings.run">
    <description>Run TuxTimings</description>
    <message>Authentication is required to run TuxTimings</message>
    <defaults>
      <allow_any>auth_admin</allow_any>
      <allow_inactive>auth_admin</allow_inactive>
      <allow_active>auth_admin_keep</allow_active>
    </defaults>
    <annotate key="org.freedesktop.policykit.exec.path">/opt/TuxTimings/bin/tuxtimings</annotate>
    <annotate key="org.freedesktop.policykit.exec.allow_gui">true</annotate>
  </action>
</policyconfig>
POLICY

    # Desktop file
    cat > "$DEB_ROOT/usr/share/applications/tuxtimings.desktop" << 'DESKTOP'
[Desktop Entry]
Name=TuxTimings
Comment=AMD Ryzen DRAM timings viewer
Exec=tuxtimings
Icon=tuxtimings
Terminal=false
Type=Application
Categories=Utility;
DESKTOP

    # Icon
    if [ -f "$SCRIPT_DIR/tuxtimings.png" ]; then
        install -m644 "$SCRIPT_DIR/tuxtimings.png" "$DEB_ROOT/usr/share/icons/hicolor/256x256/apps/tuxtimings.png"
    fi

    dpkg-deb --build "$DEB_ROOT"
    mv "$DEB_ROOT.deb" "$SCRIPT_DIR/"
    rm -rf "$SCRIPT_DIR/deb-build"
    echo "==> .deb package created: $SCRIPT_DIR/tuxtimings_${PKG_VERSION}_amd64.deb"
    echo "    Install with: sudo dpkg -i tuxtimings_${PKG_VERSION}_amd64.deb"
    exit 0
fi

# ── Build ─────────────────────────────────────────────────────────────

# When re-invoked with sudo for installation, skip build
INSTALL_ONLY=0
if [ "$1" = "--install-only" ]; then
    INSTALL_ONLY=1
    shift
fi

if [ "$INSTALL_ONLY" -eq 0 ]; then

echo "==> Building tuxtimings (C/GTK4)..."
if [ "$(id -u)" -eq 0 ]; then
    su "$REAL_USER" -c "'$MAKE' -C '$SCRIPT_DIR' clean all"
else
    "$MAKE" -C "$SCRIPT_DIR" clean all
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

# Launcher script in PATH
cat > /usr/bin/tuxtimings << 'LAUNCHER'
#!/bin/bash
if [ "$(id -u)" -eq 0 ]; then
    exec /opt/TuxTimings/bin/tuxtimings "$@"
fi
ENV_ARGS=""
for VAR in DISPLAY WAYLAND_DISPLAY XDG_RUNTIME_DIR XAUTHORITY \
           DBUS_SESSION_BUS_ADDRESS XDG_CONFIG_HOME HOME; do
    eval VAL=\$$VAR
    [ -n "$VAL" ] && ENV_ARGS="$ENV_ARGS --env-$VAR=$VAL"
done
[ -n "$WAYLAND_DISPLAY" ] && ENV_ARGS="$ENV_ARGS --env-GDK_BACKEND=wayland"
exec pkexec /opt/TuxTimings/bin/tuxtimings $ENV_ARGS "$@"
LAUNCHER
chmod +x /usr/bin/tuxtimings

# Polkit policy
mkdir -p /usr/share/polkit-1/actions
cat > /usr/share/polkit-1/actions/com.tuxtimings.policy << 'POLICY'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE policyconfig PUBLIC
 "-//freedesktop//DTD PolicyKit Policy Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/PolicyKit/1/policyconfig.dtd">
<policyconfig>
  <action id="com.tuxtimings.run">
    <description>Run TuxTimings</description>
    <message>Authentication is required to run TuxTimings</message>
    <defaults>
      <allow_any>auth_admin</allow_any>
      <allow_inactive>auth_admin</allow_inactive>
      <allow_active>auth_admin_keep</allow_active>
    </defaults>
    <annotate key="org.freedesktop.policykit.exec.path">/opt/TuxTimings/bin/tuxtimings</annotate>
    <annotate key="org.freedesktop.policykit.exec.allow_gui">true</annotate>
  </action>
</policyconfig>
POLICY

# Desktop file
cat > /usr/share/applications/tuxtimings.desktop << 'DESKTOP'
[Desktop Entry]
Name=TuxTimings
Comment=AMD Ryzen DRAM timings viewer
Exec=tuxtimings
Icon=tuxtimings
Terminal=false
Type=Application
Categories=Utility;
DESKTOP

# Desktop shortcut
DESKTOP_DIR="${REAL_HOME}/Desktop"
[ ! -d "$DESKTOP_DIR" ] && \
    DESKTOP_DIR=$(su "$REAL_USER" -c 'xdg-user-dir DESKTOP 2>/dev/null' || true)
if [ -d "$DESKTOP_DIR" ]; then
    cp /usr/share/applications/tuxtimings.desktop "$DESKTOP_DIR/"
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
echo "    Binary:    $INSTALL_DIR/bin/tuxtimings"
echo "    Launcher:  /usr/bin/tuxtimings"
echo "    Policy:    /usr/share/polkit-1/actions/com.tuxtimings.policy"
echo "    Desktop:   /usr/share/applications/tuxtimings.desktop"
echo ""
echo "    Run with:  tuxtimings"
