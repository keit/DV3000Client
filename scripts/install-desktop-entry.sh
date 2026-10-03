#!/bin/bash
# Adds "DV3K Client" to the desktop's application launcher (GNOME, KDE,
# ...) for the current user, with its icon -- for running from a source
# build; the .deb and AppImage set up their own. Points at the binary in this
# repo's build directory -- run it again if you move the repo. No sudo
# needed; everything goes under ~/.local/share.
#
#   ./scripts/install-desktop-entry.sh             # install
#   ./scripts/install-desktop-entry.sh --uninstall # remove
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
desktop_file=$data/applications/dv3kclient.desktop
sizes=(16 24 32 48 64 128 256)

if [[ ${1:-} == --uninstall ]]; then
    rm -f "$desktop_file"
    for size in "${sizes[@]}"; do
        rm -f "$data/icons/hicolor/${size}x${size}/apps/dv3kclient.png"
    done
    echo "Removed DV3K Client from the application launcher."
    exit 0
fi

binary=$repo/build/dv3kclient
if [[ ! -x $binary ]]; then
    echo "$binary not found -- build it first (see README.md)." >&2
    exit 1
fi

for size in "${sizes[@]}"; do
    install -D -m 644 "$repo/resources/icons/dv3kclient-$size.png" \
        "$data/icons/hicolor/${size}x${size}/apps/dv3kclient.png"
done

mkdir -p "$(dirname "$desktop_file")"
# Same entry the packages install (resources/dv3kclient.desktop), pointed
# at this build instead of a dv3kclient on PATH. Its file name must match
# the app's QGuiApplication::setDesktopFileName() so the running window is
# matched to this entry and gets its icon in the dock/Alt-Tab.
sed -e "s|^Exec=.*|Exec=\"$binary\"\nPath=$repo/build|" \
    "$repo/resources/dv3kclient.desktop" > "$desktop_file"

# Refresh the caches if the tools are there; desktops also pick changes up
# on their own, just not always immediately.
gtk-update-icon-cache -q -t "$data/icons/hicolor" 2>/dev/null || true
update-desktop-database -q "$data/applications" 2>/dev/null || true

echo "Installed: $desktop_file"
echo "DV3K Client should now appear in the application launcher."
