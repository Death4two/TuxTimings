# Maintainer: Death4two <https://github.com/Death4two>
pkgbase=tuxtimings
pkgname=('tuxtimings' 'tuxtimings-dkms')
pkgver=1.0.5
pkgrel=2
pkgdesc="AMD Ryzen DRAM timings and CPU telemetry viewer (GTK4)"
arch=('x86_64')
url="https://github.com/Death4two/TuxTimings"
license=('GPL3')
makedepends=('gcc' 'pkgconf')
source=("$pkgbase-$pkgver.tar.gz::https://github.com/Death4two/TuxTimings/archive/refs/heads/main.tar.gz")
sha256sums=('SKIP')

build() {
    cd "$srcdir/TuxTimings-main/Linux"
    # Run clean and build in separate make invocations to avoid
    # parallel builds (`MAKEFLAGS=-j…`) deleting objects while linking.
    make clean
    make all
}

package_tuxtimings() {
    pkgdesc="AMD Ryzen DRAM timings and CPU telemetry viewer (GTK4)"
    depends=('gtk4')
    optdepends=(
        'tuxtimings-dkms: kernel modules for accurate benchmarking and memory voltages'
        'ryzen_smu-dkms-git: kernel module for reading AMD SMN/PM tables'
        'nct6775-dkms-git: fan readings on boards with Nuvoton Super I/O (NCT6775F through NCT6799D)'
    )

    cd "$srcdir/TuxTimings-main/Linux"

    # Binary
    install -Dm755 tuxtimings "$pkgdir/opt/TuxTimings/bin/tuxtimings"

    # Icon
    install -Dm644 tuxtimings.png "$pkgdir/usr/share/icons/hicolor/256x256/apps/tuxtimings.png"

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

package_tuxtimings-dkms() {
    pkgdesc="DKMS kernel modules for TuxTimings (aod-voltages, tuxbench)"
    depends=('dkms' 'linux-headers')
    optdepends=('clang: required if your kernel was built with Clang (CachyOS, etc)')
    install=tuxtimings-dkms.install

    cd "$srcdir/TuxTimings-main/Linux"

    # aod-voltages DKMS module source
    local aod_ver
    aod_ver=$(grep '^PACKAGE_VERSION=' src/aod-voltages/dkms.conf | cut -d= -f2 | tr -d '"')
    install -dm755 "$pkgdir/usr/src/aod-voltages-$aod_ver"
    install -Dm644 src/aod-voltages/aod_voltages.c  "$pkgdir/usr/src/aod-voltages-$aod_ver/aod_voltages.c"
    install -Dm644 src/aod-voltages/Makefile         "$pkgdir/usr/src/aod-voltages-$aod_ver/Makefile"
    install -Dm644 src/aod-voltages/dkms.conf        "$pkgdir/usr/src/aod-voltages-$aod_ver/dkms.conf"

    # tuxbench DKMS module source
    local tb_ver
    tb_ver=$(grep '^PACKAGE_VERSION=' src/tuxbench/dkms.conf | cut -d= -f2 | tr -d '"')
    install -dm755 "$pkgdir/usr/src/tuxbench-$tb_ver"
    install -Dm644 src/tuxbench/tuxbench.c   "$pkgdir/usr/src/tuxbench-$tb_ver/tuxbench.c"
    install -Dm644 src/tuxbench/tuxbench.h   "$pkgdir/usr/src/tuxbench-$tb_ver/tuxbench.h"
    install -Dm644 src/tuxbench/Makefile     "$pkgdir/usr/src/tuxbench-$tb_ver/Makefile"
    install -Dm644 src/tuxbench/dkms.conf    "$pkgdir/usr/src/tuxbench-$tb_ver/dkms.conf"
}
