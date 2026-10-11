#!/usr/bin/env bash
# Mirror this repo (including the ARMSX2 submodule) to the Steam Frame for building there.
# FRAME_HOST (default steamos@frame.local) and FRAME_DIR (default ps2-volumetric/repo) select the target.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
host=${FRAME_HOST:-steamos@frame.local}
dir=${FRAME_DIR:-ps2-volumetric/repo}
ssh "$host" "mkdir -p $dir"
rsync -a --delete --exclude /build/ --exclude .DS_Store "$root/" "$host:$dir/"
echo "synced to $host:$dir"
