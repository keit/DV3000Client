#!/bin/bash
# Builds DV3KClient-<arch>.AppImage from a configured and built build
# directory: installs into an AppDir, then lets linuxdeploy and its Qt
# plugin copy in the Qt libraries and plugins the binary needs. The
# linuxdeploy tools are downloaded into the build directory on first use.
#
#   cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build
#   ./scripts/build-appimage.sh [build-dir]      # default: build
#
# libasound is deliberately not bundled (linuxdeploy's excludelist leaves
# it out): it has to be the host's own, so the host's ALSA configuration
# and PipeWire/PulseAudio plugins are what the app talks to.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
build=$(cd "${1:-$repo/build}" && pwd)
arch=$(uname -m)
tools=$build/appimage-tools
appdir=$build/AppDir

# linuxdeploy only publishes rolling "continuous" builds.
base=https://github.com/linuxdeploy
mkdir -p "$tools"
for tool in "linuxdeploy/releases/download/continuous/linuxdeploy-$arch.AppImage" \
            "linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$arch.AppImage"; do
    file=$tools/$(basename "$tool")
    if [[ ! -x $file ]]; then
        curl -fsSL --retry 3 --retry-all-errors -o "$file" "$base/$tool"
        chmod +x "$file"
    fi
done

rm -rf "$appdir"
DESTDIR=$appdir cmake --install "$build" --prefix /usr

# Inside a container (CI) there's no FUSE to mount the tools' own
# AppImages; extracting them first works everywhere.
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake)}
# Wayland support alongside the default xcb, so the app runs natively on
# GNOME/KDE Wayland sessions (it falls back to XWayland without it). The
# plugin names vary by Qt version (libqwayland-generic.so in 6.4,
# libqwayland.so later), so take whichever this Qt has. Plus offscreen,
# so CI can smoke-test the AppImage with no display.
plugins=$("$QMAKE" -query QT_INSTALL_PLUGINS)/platforms
EXTRA_PLATFORM_PLUGINS=$(cd "$plugins" && ls libqwayland*.so libqoffscreen.so 2>/dev/null | paste -sd';' || true)
export EXTRA_PLATFORM_PLUGINS
export LDAI_OUTPUT=$build/DV3KClient-$arch.AppImage

cd "$build"
linuxdeploy=$tools/linuxdeploy-$arch.AppImage
"$linuxdeploy" \
    --appdir "$appdir" \
    --desktop-file "$appdir/usr/share/applications/dv3kclient.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/dv3kclient.png" \
    --plugin qt

# GNOME's Wayland compositor leaves each app to draw its own title bar, and
# Qt 6.4 (Ubuntu 24.04's, which CI builds with) only draws one when a
# client buffer integration loads -- without this plugin, which
# linuxdeploy-plugin-qt doesn't deploy, the window has no title bar at
# all. Added by hand, then a second pass bundles its libraries (EGL/GL
# themselves stay the host's, as they must, per linuxdeploy's excludelist).
egl_plugin=$("$QMAKE" -query QT_INSTALL_PLUGINS)/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so
extra=()
if compgen -G "$appdir/usr/plugins/platforms/libqwayland*.so" >/dev/null && [[ -f $egl_plugin ]]; then
    dir=$appdir/usr/plugins/wayland-graphics-integration-client
    install -D -m 755 "$egl_plugin" "$dir/$(basename "$egl_plugin")"
    extra=(--deploy-deps-only "$dir")
fi
"$linuxdeploy" --appdir "$appdir" "${extra[@]}" --output appimage

echo "Built: $LDAI_OUTPUT"
