#!/usr/bin/env bash
# Packs a release of ARMSX2 - Steam Frame Edition: the game built on the Frame (tools/build-frame.sh), the steps that install
# it there, the library artwork and Valve's devkit scripts, in one .tar.gz for the GitHub release, which the installer app
# (make-installer.sh) downloads and installs. Builds on the Frame first, so the game matches this checkout.
#
# Usage: tools/steam-frame/make-release.sh [version]      (default: <date>-<short commit>)
# Output: build/release/armsx2-steam-frame-<version>.tar.gz, unpacking to a folder of that name (see stage-release.sh)
#
# Attach it to a GitHub release of SPD13/ps2-volumetric with the installer app's files: the installer picks the latest release's
# armsx2-steam-frame-*.tar.gz. Check a release before publishing it with the installer's --archive option.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/frame-common.sh"

commit=$(git -C "$root" rev-parse HEAD)
core_commit=$(git -C "$root/external/armsx2" rev-parse HEAD)
version=${1:-$(date +%Y%m%d)-$(git -C "$root" rev-parse --short HEAD)}
[[ $version =~ ^[A-Za-z0-9._-]+$ ]] || { echo "invalid version '$version'" >&2; exit 1; }
name=armsx2-steam-frame-$version
if [[ -n $(git -C "$root" status --porcelain --untracked-files=no) ]]; then
   echo "WARNING: uncommitted changes: the release may not match commit $commit, which SOURCE.md names"
fi
if [[ -n $(git -C "$root" log --oneline "origin/main..HEAD" 2> /dev/null) ]]; then
   echo "WARNING: commits not pushed to origin/main yet: push them so SOURCE.md points to published source"
fi
if ! git -C "$root/external/armsx2" branch -r --contains "$core_commit" 2> /dev/null | grep -q 'origin/'; then
   echo "WARNING: the ARMSX2 commit $core_commit is not pushed to the fork yet: push it so SOURCE.md points to published source"
fi

prepare_frame

out=$root/build/release
mkdir -p "$out"
cat > "$out/SOURCE.md" <<EOT
# Source code

ARMSX2 - Steam Frame Edition $version was built from:

- https://github.com/SPD13/ps2-volumetric, commit \`$commit\`: the VR app (\`host/\`), the build script
  (\`tools/build-frame.sh\`) and these install scripts (\`tools/steam-frame/\`)
- https://github.com/SPD13/armsx2-steam-frame, branch \`ps2v\`, commit \`$core_commit\`: ARMSX2 with the patches the
  app needs, a fork of https://github.com/ARMSX2/ARMSX2 (itself a fork of PCSX2)

ARMSX2 and this app are free software under the GNU General Public License version 3 (\`LICENSE\`). The libraries built into
\`game/ps2v\` keep their own licences, in the source trees above (\`host/third_party/\`, \`external/armsx2/3rdparty/\`).
\`devkit-utils/\` is Valve's SteamOS Devkit Client scripts, MIT licence (\`devkit-utils/LICENSE\`), from
https://gitlab.steamos.cloud/devkit/steamos-devkit.

No BIOS files or games are included.
EOT
rsync -a "$out/SOURCE.md" "$host:$remote_release-SOURCE.md"

echo "==> Staging the release on the Frame"
ssh "$host" "$dir/tools/steam-frame/stage-release.sh $remote_release/$name $version $remote_devkit $remote_release-SOURCE.md"
echo "==> Packing $name.tar.gz"
ssh "$host" "tar -C $remote_release -czf - $name && rm -rf $remote_release $remote_release-SOURCE.md" > "$out/$name.tar.gz.part"
mv "$out/$name.tar.gz.part" "$out/$name.tar.gz"
rm -f "$out/SOURCE.md"
ls -lh "$out/$name.tar.gz"
