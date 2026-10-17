#!/usr/bin/env bash
# Runs on the Frame, in the repo mirror that tools/sync-frame.sh makes (make-release.sh and deploy-to-frame.sh start it over
# ssh). Puts the build of tools/build-frame.sh and everything the install needs in a release folder:
#
#   game/          ps2v (debug info stripped, symbols kept for crash backtraces), resources/, assets/, profiles/, run.sh, VERSION,
#                  LICENSE and THIRD_PARTY_NOTICES.md (so they stay beside the installed game)
#   tools/         frame-install.sh, install-library-art.py, set-library-name.py
#   library-art/   assets/steam/ (capsule, wide, hero, logo, icon)
#   devkit-utils/  Valve's devkit scripts (MIT), sent by make-release.sh to DEVKIT
#   LICENSE  THIRD_PARTY_NOTICES.md  SOURCE.md  VERSION
#
#   stage-release.sh OUT VERSION DEVKIT SOURCE_MD
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
out=${1:?output folder}; version=${2:?version}; devkit=${3:?devkit-utils folder}; source_md=${4:?SOURCE.md}
bin=$root/build/frame/bin
[[ -x $bin/ps2v ]] || { echo "No build at $bin/ps2v: run tools/build-frame.sh first" >&2; exit 1; }
[[ -f $devkit/steam-client-create-shortcut ]] || { echo "No devkit scripts in $devkit" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out/game" "$out/tools"
out=$(cd "$out" && pwd)
# llvm-strip from the SDK container of tools/build-frame.sh, which sees the repo only: SteamOS itself has no binutils
container=ps2v-build
podman start "$container" > /dev/null
stripped=$(realpath "$root/build/frame")/ps2v.stripped
podman exec "$container" llvm-strip --strip-debug -o "$stripped" "$(realpath "$bin/ps2v")"
mv "$stripped" "$out/game/ps2v"
cp -R "$bin/resources" "$bin/assets" "$bin/profiles" "$out/game/"
cp "$root/tools/steam-frame/run.sh" "$out/game/run.sh"
chmod +x "$out/game/ps2v" "$out/game/run.sh"
cp "$root/tools/steam-frame/frame-install.sh" "$root/tools/steam-frame/install-library-art.py" \
   "$root/tools/steam-frame/set-library-name.py" "$out/tools/"
cp -R "$root/assets/steam" "$out/library-art"
cp -R "$devkit" "$out/devkit-utils"
find "$out/devkit-utils" -name __pycache__ -prune -exec rm -rf {} +
cp "$root/external/armsx2/COPYING.GPLv3" "$out/LICENSE"
cp "$root/THIRD_PARTY_NOTICES.md" "$out/THIRD_PARTY_NOTICES.md"
cp "$out/LICENSE" "$out/THIRD_PARTY_NOTICES.md" "$out/game/"
cp "$source_md" "$out/SOURCE.md"
echo "$version" > "$out/VERSION"
echo "$version" > "$out/game/VERSION"   # the installed version, which the installer app compares with the latest release
echo "staged $(du -sh "$out" | cut -f1) in $out"
