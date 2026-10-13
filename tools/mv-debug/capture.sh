#!/bin/bash
# Capture a step-out run of Time Crisis II with gfxreconstruct (layer copy in ~/vklayers on the Frame).
# Usage: tools/mv-debug/capture.sh <name>  -> Frame: ~/gfxr/<name>.gfxr; Mac: captures/mv/<name>/ dumps + verdicts
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
name=$1
ssh -o ConnectTimeout=10 steamos@frame.local "pkill -f '^python3 tools/m0/vpad'" >/dev/null 2>&1
timeout 280 ssh -o ConnectTimeout=10 steamos@frame.local "pkill -x ps2v; for i in \$(seq 20); do pgrep -x ps2v >/dev/null || break; sleep 0.5; done; D=~/.local/share/ps2v/debug; rm -f \$D/present*; cd ~/ps2-volumetric/repo; (nohup python3 tools/m0/vpad.py --hold 150 26:lb:0.5 29:lb:0.5 32:lb:0.5 35:lb:0.5 38:lb:80 > /tmp/vpad.log 2>&1 < /dev/null &); sleep 1; (VK_LAYER_PATH=/home/steamos/vklayers VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_gfxreconstruct GFXRECON_CAPTURE_FILE=/home/steamos/gfxr/$name.gfxr GFXRECON_CAPTURE_FILE_TIMESTAMP=false GFXRECON_CAPTURE_COMPRESSION_TYPE=ZSTD GFXRECON_LOG_LEVEL=warning nohup timeout 300 tools/run-frame.sh --sdl-pad --stereo --stereo-z --stereo-conv 0.12 --stereo-depth 3 --load-state \$D/state-room2.p2s /home/steamos/ps2-volumetric/m0/games/TimeCrisisIIDVD-dvdfix.iso > /tmp/ps2v-gfxr.log 2>&1 < /dev/null &); sleep 41; P=\$(pgrep -x ps2v); for t in \$(seq 7); do kill -USR2 \$P; sleep 3; done; pkill -x ps2v; sleep 8; cp ~/.local/share/ps2v/logs/emulog.txt ~/gfxr/$name.emulog.txt" >/dev/null 2>&1
d=$root/captures/mv/$name; rm -rf "$d"; mkdir -p "$d"
scp -q "steamos@frame.local:.local/share/ps2v/debug/present00?-left.png" "$d/" 2>/dev/null
for f in "$d"/present00?-left.png; do sips -s format jpeg -Z 320 "$f" --out "${f%.png}.jpg" >/dev/null; done
swift "$root/tools/mv-debug/classify.swift" "$d"/present00?-left.jpg
