#!/usr/bin/env bash
# Rebuild a light gun drawn from photos (no scans needed): lightgun1 -> assets/lightgun.glb, lightgun2 -> assets/lightgun2.glb.
# Usage: tools/lightgun/photo/run.sh <lightgun1|lightgun2> <work dir> [python] [out.glb] [preview.png]
# Python needs: numpy scipy trimesh pymeshlab scikit-image rtree mapbox-earcut pillow
set -euo pipefail
GUN=$1; WORK=$2; PY=${3:-python3}
HERE=$(cd "$(dirname "$0")" && pwd); LG=$(cd "$HERE/.." && pwd); REPO=$(cd "$LG/../.." && pwd)
case $GUN in
	lightgun1) NAME=lightgun ;;
	lightgun2) NAME=lightgun2 ;;
	*) echo "unknown gun: $GUN" >&2; exit 1 ;;
esac
OUT=${4:-$REPO/assets/$NAME.glb}; PREVIEW=${5:-$REPO/docs/images/$NAME-preview.png}
mkdir -p "$WORK"
"$PY" "$HERE/build_body.py" "$HERE/$GUN.json" "$WORK" 0.5
"$PY" "$LG/decimate.py" "$WORK" 2600
"$PY" "$HERE/${GUN}_glb.py" "$WORK" "$OUT"
"$PY" "$LG/view_glb.py" "$LG" "$OUT" "$PREVIEW"
