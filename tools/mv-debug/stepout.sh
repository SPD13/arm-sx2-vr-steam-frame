#!/bin/bash
# Reproduce the multiview wall corruption in Time Crisis II without a person in the headset.
# Usage: tools/mv-debug/stepout.sh <name> ["first last"]...
#   Runs ps2v on the Frame from the debug save state (state-room2.p2s, in cover in the first shooting
#   room), presses the pedal (LB on the virtual pad) so the player steps out, then for each range
#   writes it to the flush-range file (see VOLUMETRIC_FLUSH_FILE) and takes a present dump (SIGUSR2).
#   "0 0" = no flushing. Extra environment for ps2v goes in $EXTRA, e.g. EXTRA="TU_DEBUG=sysmem".
#   Results land in captures/mv/<name>/: NN-<range>.jpg per dump, sheet-s.jpg, and the verdicts of
#   classify.swift (ok / BROKEN / COVER) on stdout.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
name=$1; shift
[[ $# -eq 0 ]] && set -- "0 0" "0 0"
ranges=""; for r in "$@"; do ranges="$ranges '$r'"; done
ssh -o ConnectTimeout=10 steamos@frame.local "pkill -f '^python3 tools/m0/vpad'" >/dev/null 2>&1
timeout 200 ssh -o ConnectTimeout=10 steamos@frame.local "pkill -x ps2v; for i in \$(seq 20); do pgrep -x ps2v >/dev/null || break; sleep 0.5; done; D=~/.local/share/ps2v/debug; F=\$D/flush; echo '0 0' > \$F; rm -f \$D/present*; cd ~/ps2-volumetric/repo; (nohup python3 tools/m0/vpad.py --hold 90 18:lb:1 21:lb:1 24:lb:60 > /tmp/vpad.log 2>&1 < /dev/null &); sleep 1; (VOLUMETRIC_FLUSH_FILE=\$F ${EXTRA:-} nohup timeout 300 tools/run-frame.sh --sdl-pad --stereo --stereo-z --stereo-conv 0.12 --stereo-depth 3 --load-state \$D/state-room2.p2s /home/steamos/ps2-volumetric/m0/games/TimeCrisisIIDVD-dvdfix.iso > /tmp/ps2v-run.log 2>&1 < /dev/null &); sleep 26.5; P=\$(pgrep -x ps2v); for r in $ranges; do echo \"\$r\" > \$F; sleep 1.5; kill -USR2 \$P; sleep 1.5; done; sleep 2; pkill -x ps2v" >/dev/null 2>&1
d=$root/captures/mv/$name; rm -rf "$d"; mkdir -p "$d"
scp -q "steamos@frame.local:.local/share/ps2v/debug/present0*-*.png" "$d/" 2>/dev/null
scp -q steamos@frame.local:.local/share/ps2v/logs/emulog.txt "$d/" 2>/dev/null
i=0; for r in "$@"; do f=$(printf "%s/present%03d-left.png" "$d" $i); [[ -f $f ]] && sips -s format jpeg -Z 320 "$f" --out "$d/$(printf %02d $i)-${r// /-}.jpg" >/dev/null; i=$((i+1)); done
swift "$root/tools/mv-debug/sheet.swift" "$d/sheet.jpg" 3 "$d"/[0-9]*.jpg && sips -Z 1000 "$d/sheet.jpg" --out "$d/sheet-s.jpg" >/dev/null
swift "$root/tools/mv-debug/classify.swift" "$d"/[0-9]*.jpg
