#!/usr/bin/env bash
# Copies a take from the Steam Frame to this computer (they are large: keep them out of the repo).
# Usage: tools/trailer/fetch-capture.sh NAME [DEST]   (DEST defaults to ~/ps2v-captures)
#        tools/trailer/fetch-capture.sh --list        the takes on the headset
# FRAME_HOST (default steamos@frame.local) selects the headset.
set -euo pipefail
host=${FRAME_HOST:-steamos@frame.local}
if [[ ${1:-} == --list || -z ${1:-} ]]; then
  ssh "$host" 'cd ~/ps2-volumetric/captures 2>/dev/null && du -sh -- * 2>/dev/null || echo "No takes yet."'
  exit 0
fi
name=$1
dest=${2:-$HOME/ps2v-captures}
mkdir -p "$dest"
rsync -a --info=progress2 "$host:ps2-volumetric/captures/$name" "$dest/"
echo "Fetched to $dest/$name"
