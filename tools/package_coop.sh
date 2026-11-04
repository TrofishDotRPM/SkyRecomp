#!/bin/bash
# Package the current build/skyrecomp for players (Bazzite / Fedora, x86-64) and put it, alone, in the shared folder:
#   tools/package_coop.sh            -> build/dist/coop/SkyRecomp-coop-<commit>-bazzite-x86_64.tar.gz
#                                       and "../Co-op build (latest)/" with only that package and VERSION.txt
set -eu
src="$(cd "$(dirname "$0")/.." && pwd)"
cd "$src"
h=$(git rev-parse --short HEAD)
dirty=$(git status --porcelain runtime docs | head -1)
[ -n "$dirty" ] && h="$h-dirty"
dist=build/dist/coop
tmpl="$dist/Sky Recomp coop 0e749ee"  # (the readme, icon, font and licences every package carries)
name="Sky Recomp coop $h"
rm -rf "$dist/$name"
cp -a "$tmpl" "$dist/$name"
cp build/skyrecomp "$dist/$name/skyrecomp"
cp docs/coop.md "$dist/$name/COOP.md"
tar czf "$dist/SkyRecomp-coop-$h-bazzite-x86_64.tar.gz" -C "$dist" "$name"
rm -rf "$dist/$name"
latest="$src/../Co-op build (latest)"
mkdir -p "$latest"
rm -f "$latest"/SkyRecomp-coop-*.tar.gz
cp "$dist/SkyRecomp-coop-$h-bazzite-x86_64.tar.gz" "$latest/"
{
    echo "Newest co-op build: $h ($(date '+%Y-%m-%d %H:%M'))"
    echo "Both players need this same build (an older one is refused as a different version)."
    echo
    echo "Changes in it, newest first:"
    git log --format='  %h %s' -15
} > "$latest/VERSION.txt"
echo "$latest/SkyRecomp-coop-$h-bazzite-x86_64.tar.gz"
