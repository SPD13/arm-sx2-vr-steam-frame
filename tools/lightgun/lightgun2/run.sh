#!/usr/bin/env bash
# Rebuild the scan-based LightGun 2 (assets/lightgun2-scan.glb, not shipped) from its right shell scan (mirrored for the left shell).
# Usage: tools/lightgun/lightgun2/run.sh <dir with Right.stl> <work dir> [python]
# Python needs: numpy scipy trimesh pymeshlab scikit-image rtree shapely mapbox-earcut pillow
set -euo pipefail
SCANS=$1; WORK=$2; PY=${3:-python3}
HERE=$(cd "$(dirname "$0")" && pwd); LG=$(cd "$HERE/.." && pwd); REPO=$(cd "$LG/../.." && pwd)
mkdir -p "$WORK"
"$PY" "$HERE/load_stl.py" "$SCANS/Right.stl" "$WORK/Right.npz"
"$PY" "$HERE/align.py" "$WORK"
"$PY" "$LG/fields.py" "$WORK" > /dev/null
"$PY" "$LG/solid.py" "$WORK" 0.5 "$HERE/markings.json"
"$PY" "$LG/decimate.py" "$WORK" 2600
"$PY" "$HERE/build_glb.py" "$WORK" "$REPO/assets/lightgun2-scan.glb"
"$PY" "$LG/view_glb.py" "$LG" "$REPO/assets/lightgun2-scan.glb" "$WORK/lightgun2-scan-preview.png"
