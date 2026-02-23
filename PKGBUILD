# Maintainer: Death4two <https://github.com/Death4two>
pkgname=tuxtimings
pkgver=1.0.0
pkgrel=1
pkgdesc="AMD Ryzen DRAM timings and CPU telemetry viewer (GTK4)"
arch=('x86_64')
url="https://github.com/Death4two/TuxTimings"
license=('GPL3')
depends=('gtk4' 'dmidecode')
makedepends=('gcc' 'pkgconf')
optdepends=('ryzen_smu-dkms-git: kernel module for reading AMD SMN/PM tables')
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

    # Icon
    install -Dm644 tuxtimings.png "$pkgdir/usr/share/icons/hicolor/256x256/apps/tuxtimings.png"

    # Launcher script (handles env forwarding through pkexec)
    install -Dm755 /dev/stdin "$pkgdir/usr/bin/tuxtimings" << 'LAUNCHER'
#!/bin/bash
# If already root, run directly
if [ "$(id -u)" -eq 0 ]; then
    exec /opt/TuxTimings/bin/tuxtimings "$@"
fi

# Build --env-VAR=VALUE args to restore after pkexec
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
