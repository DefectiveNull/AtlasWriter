#!/bin/sh
# Installs AtlasWriter for the current user, without root.
#
#   sh install.sh [source-folder]
#
# The source is this script's folder in a release archive, or ../build when run
# from a repository checkout.

set -eu

die() {
    echo "install.sh: $1" >&2
    exit 1
}

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ $# -ge 1 ]; then
    source=$1
elif [ -x "$here/AtlasWriter" ]; then
    source=$here
else
    source=$here/../build
fi
[ -x "$source/AtlasWriter" ] || die "AtlasWriter was not found in '$source'. Build the project or pass a folder."
source=$(CDPATH= cd -- "$source" && pwd)

data=${XDG_DATA_HOME:-$HOME/.local/share}
target=$data/AtlasWriter
[ "$source" != "$target" ] || die "the source is the install folder itself: '$target'."

for link in /proc/[0-9]*/exe; do
    if [ "$(readlink "$link" 2>/dev/null || true)" = "$target/AtlasWriter" ]; then
        die "AtlasWriter is running from the install folder. Close it and try again."
    fi
done

# Only these go in: a build folder also holds CMake's output.
for name in AtlasWriter icon.svg settings.example.ccl assets; do
    [ -e "$source/$name" ] || die "'$name' is missing from '$source'."
done

mkdir -p "$target"
for name in AtlasWriter icon.svg settings.example.ccl assets LICENSE README.md licenses; do
    rm -rf "${target:?}/$name"
    if [ -e "$source/$name" ]; then
        cp -R "$source/$name" "$target/$name"
    fi
done
cp "$here/uninstall.sh" "$target/uninstall.sh"
chmod +x "$target/AtlasWriter" "$target/uninstall.sh"
echo AtlasWriter > "$target/installed.marker"

# The launcher entry is what makes it searchable.
icons=$data/icons/hicolor/scalable/apps
mkdir -p "$icons" "$data/applications"
cp "$source/icon.svg" "$icons/atlaswriter.svg"
cat > "$data/applications/atlaswriter.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=AtlasWriter
Comment=A minimalist text editor for the thinking mind
Exec="$target/AtlasWriter" %F
Icon=atlaswriter
Terminal=false
Categories=Utility;TextEditor;Development;
StartupWMClass=atlaswriter
EOF

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$data/applications" || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q "$data/icons/hicolor" 2>/dev/null || true
fi

echo "AtlasWriter $("$target/AtlasWriter" --version) installed to $target"
