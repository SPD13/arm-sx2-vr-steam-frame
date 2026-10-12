#!/usr/bin/env bash
# Rebuild assets/lightgun.glb from the two shell scans.
# Usage: tools/lightgun/run.sh <dir with LeftScan.obj + RightScan.obj> <work dir> [python]
# Python needs: numpy scipy trimesh pymeshlab scikit-image rtree shapely mapbox-earcut pillow
set -euo pipefail
SCANS=$1; WORK=$2; PY=${3:-python3}
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../.." && pwd)
mkdir -p "$WORK"
"$PY" "$HERE/load.py" "$SCANS/LeftScan.obj" "$WORK/Left.npz"
"$PY" "$HERE/load.py" "$SCANS/RightScan.obj" "$WORK/Right.npz"
"$PY" "$HERE/align.py" "$WORK"
"$PY" "$HERE/fields.py" "$WORK" > /dev/null
"$PY" "$HERE/solid.py" "$WORK" 0.5
"$PY" "$HERE/decimate.py" "$WORK" 2600
"$PY" "$HERE/build_glb.py" "$WORK" "$REPO/assets/lightgun.glb"
"$PY" "$HERE/view_glb.py" "$HERE" "$REPO/assets/lightgun.glb" "$REPO/docs/images/lightgun-preview.png"
