#!/bin/bash
# Build TuxTimings as a self-contained app and pack it as an AppImage for Arch (and other Linux).
# Requires: dotnet SDK, appimagetool (e.g. from https://github.com/AppImage/AppImageKit/releases)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

OUT_DIR="$SCRIPT_DIR/publish"
APPDIR="$SCRIPT_DIR/AppDir"
RID="${RID:-linux-x64}"

echo "==> Publishing self-contained app (runtime: $RID)..."
dotnet publish TuxTimings.LinuxUI/TuxTimings.LinuxUI.csproj \
  -c Release \
  -r "$RID" \
  --self-contained true \
  -p:PublishSingleFile=false \
  -o "$OUT_DIR"

echo "==> Preparing AppDir..."
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"

cp -a "$OUT_DIR"/* "$APPDIR/usr/bin/"
if [ ! -x "$APPDIR/usr/bin/TuxTimings.LinuxUI" ]; then
  echo "ERROR: TuxTimings.LinuxUI not found or not executable in AppDir/usr/bin/"
  ls -la "$APPDIR/usr/bin/" | head -20
  exit 1
fi
cp "$SCRIPT_DIR/appimage/AppRun" "$APPDIR/"
cp "$SCRIPT_DIR/appimage/tuxtimings.desktop" "$APPDIR/"
chmod +x "$APPDIR/AppRun"

echo "==> Creating AppImage (requires appimagetool in PATH or in current dir)..."
APPIMAGE_NAME="TuxTimings-x86_64.AppImage"
OUTPUT_PATH="$SCRIPT_DIR/$APPIMAGE_NAME"
# Build in /tmp to avoid path-with-spaces issues; appimagetool often fails or misplaces output otherwise
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
  echo "Install appimagetool and run again, or download from:"
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
  mv -f "$CREATED" "$OUTPUT_PATH"
  rm -rf "$BUILD_DIR"
  echo "==> Done: $OUTPUT_PATH"
  ls -la "$OUTPUT_PATH"
  echo "     Run with: ./$APPIMAGE_NAME"
  echo "     (Needs ryzen_smu kernel module and root/sudo for full data.)"
else
  echo "==> appimagetool did not create an AppImage in $BUILD_DIR. Contents:"
  ls -la "$BUILD_DIR"
  rm -rf "$BUILD_DIR"
  exit 1
fi
