#!/usr/bin/env bash
# Developer deploy: builds on the Frame and installs that build in its Steam library with the same steps the installer app runs
# (frame-install.sh on a release folder), so every deploy also tests the user's install. The game files never leave the Frame.
#
# Usage: tools/steam-frame/deploy-to-frame.sh [gameid]      (default: ARMSX2_dev)
#
# The default gameid differs from the release's (ARMSX2), so a development build and a user install sit side by side in the
# library, as "ARMSX2 - Steam Frame Edition (dev)" and "ARMSX2 - Steam Frame Edition". FRAME_HOST and FRAME_DIR as in
# frame-common.sh. Steam must be running on the Frame (put the headset on).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/frame-common.sh"

gameid=${1:-ARMSX2_dev}
version=$(date +%Y%m%d)-$(git -C "$root" rev-parse --short HEAD)-dev
prepare_frame

bundle=$remote_release/dev
name=$(printf '%s' 'ARMSX2 - Steam Frame Edition (dev)' | base64)
echo "==> Staging on the Frame"
ssh "$host" "printf 'Development build %s, not for distribution.\n' $version > $remote_release-SOURCE.md &&
   $dir/tools/steam-frame/stage-release.sh $bundle $version $remote_devkit $remote_release-SOURCE.md &&
   rm -f $remote_release-SOURCE.md"
ssh "$host" "bash $bundle/tools/frame-install.sh --bundle $bundle --gameid $gameid --name-base64 $name"
