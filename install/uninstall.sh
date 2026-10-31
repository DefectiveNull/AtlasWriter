#!/bin/sh
# Removes AtlasWriter for the current user. Keeps ~/Documents/AtlasWriter, which
# holds the user's settings.
#
#   sh uninstall.sh

set -eu

data=${XDG_DATA_HOME:-$HOME/.local/share}
target=$data/AtlasWriter

for link in /proc/[0-9]*/exe; do
    if [ "$(readlink "$link" 2>/dev/null || true)" = "$target/AtlasWriter" ]; then
        echo "uninstall.sh: AtlasWriter is running. Close it and try again." >&2
        exit 1
    fi
done

rm -f "$data/applications/atlaswriter.desktop"
rm -f "$data/icons/hicolor/scalable/apps/atlaswriter.svg"
rm -rf "$target"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$data/applications" || true
fi

echo "AtlasWriter was removed. Your settings in Documents/AtlasWriter were kept."
