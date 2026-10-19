#!/usr/bin/env bash
# Rebuild assets/guncon2.glb from the GunCon2 right shell scan (mirrored for the left shell).
# Usage: tools/lightgun/guncon2/run.sh <dir with Right.stl> <work dir> [python]
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
"$PY" "$HERE/build_glb.py" "$WORK" "$REPO/assets/guncon2.glb"
"$PY" "$LG/view_glb.py" "$LG" "$REPO/assets/guncon2.glb" "$REPO/docs/images/guncon2-preview.png"
