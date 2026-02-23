# Maintainer: Death4two <https://github.com/Death4two>
pkgname=tuxtimings
pkgver=1.0.2
pkgrel=1
pkgdesc="AMD Ryzen DRAM timings and CPU telemetry viewer (GTK4)"
arch=('x86_64')
url="https://github.com/Death4two/TuxTimings"
license=('GPL3')
depends=('gtk4')
makedepends=('gcc' 'pkgconf')
optdepends=(
    'clang: required if your kernel was built with Clang (CachyOS, etc)'
    'dkms: required to build aod-voltages kernel module for memory voltage readings'
    'linux-headers: required to build aod-voltages kernel module'
    'ryzen_smu-dkms-git: kernel module for reading AMD SMN/PM tables'
    'nct6775-dkms-git: fan readings on boards with Nuvoton Super I/O (NCT6775F through NCT6799D)'
)
source=("$pkgname-$pkgver.tar.gz::https://github.com/Death4two/TuxTimings/archive/refs/heads/main.tar.gz")
sha256sums=('SKIP')

build() {
    cd "$srcdir/TuxTimings-main/Linux"
    make clean all
}

package() {
    cd "$srcdir/TuxTimings-main/Linux"

    # Binary
    install -Dm755 tuxtimings "$pkgdir/opt/TuxTimings/bin/tuxtimings"

    # Icon
    install -Dm644 tuxtimings.png "$pkgdir/usr/share/icons/hicolor/256x256/apps/tuxtimings.png"

    # aod-voltages DKMS module source
    local aod_ver
    aod_ver=$(grep '^PACKAGE_VERSION=' src/aod-voltages/dkms.conf | cut -d= -f2 | tr -d '"')
    install -dm755 "$pkgdir/usr/src/aod-voltages-$aod_ver"
    install -Dm644 src/aod-voltages/aod_voltages.c  "$pkgdir/usr/src/aod-voltages-$aod_ver/aod_voltages.c"
    install -Dm644 src/aod-voltages/Makefile         "$pkgdir/usr/src/aod-voltages-$aod_ver/Makefile"
    install -Dm644 src/aod-voltages/dkms.conf        "$pkgdir/usr/src/aod-voltages-$aod_ver/dkms.conf"

    # Polkit policy
    install -Dm644 /dev/stdin "$pkgdir/usr/share/polkit-1/actions/com.tuxtimings.policy" << 'EOF'
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
EOF

    # Desktop file
    install -Dm644 /dev/stdin "$pkgdir/usr/share/applications/tuxtimings.desktop" << 'DESKTOP'
[Desktop Entry]
Name=TuxTimings
Comment=AMD Ryzen DRAM timings viewer
Exec=tuxtimings
Icon=tuxtimings
Terminal=false
Type=Application
Categories=Utility;
DESKTOP

    # Launcher script (handles env forwarding through pkexec)
    install -Dm755 /dev/stdin "$pkgdir/usr/bin/tuxtimings" << 'LAUNCHER'
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
}

post_install() {
    local aod_ver
    aod_ver=$(grep '^PACKAGE_VERSION=' /usr/src/aod-voltages-*/dkms.conf 2>/dev/null | head -1 | cut -d= -f2 | tr -d '"')
    if [ -n "$aod_ver" ] && command -v dkms &>/dev/null; then
        echo "==> Building aod-voltages $aod_ver DKMS module..."
        dkms add aod-voltages/"$aod_ver" 2>/dev/null || true
        dkms build aod-voltages/"$aod_ver" && dkms install aod-voltages/"$aod_ver" || \
            echo "  WARNING: aod-voltages DKMS build failed — memory voltages will be unavailable"
    fi
}

post_upgrade() {
    post_install
}

pre_remove() {
    if command -v dkms &>/dev/null; then
        local aod_ver
        aod_ver=$(dkms status aod-voltages 2>/dev/null | grep -oP '(?<=aod-voltages, )[0-9.]+' | head -1)
        if [ -n "$aod_ver" ]; then
            rmmod aod_voltages 2>/dev/null || true
            dkms remove aod-voltages/"$aod_ver" --all 2>/dev/null || true
        fi
    fi
}
