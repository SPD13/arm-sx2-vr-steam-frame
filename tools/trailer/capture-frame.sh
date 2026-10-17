#!/usr/bin/env bash
# Records a trailer take on the Steam Frame: starts the game with --capture (both eyes' pictures and
# depth, the head, the screen and the guns) and records the headset's sound beside it. In the
# headset, start the game, calibrate the light gun and play to the scene to show; then the red record
# button under the passthrough button starts recording and stops it (as many times as you like).
# Quit the game (the power button) to end the take. Then fetch it with fetch-capture.sh.
#
# Usage: tools/trailer/capture-frame.sh [--dev] [--name NAME] [--width N] [--max-gb G] [-- ps2v arguments]
#   --dev       run the development build (~/ps2-volumetric/repo, tools/run-frame.sh) instead of the
#               game installed in the Steam library (~/devkit-game/ARMSX2)
#   --name      the take's folder name (default: take-YYYYmmdd-HHMMSS)
#   --width     scale the captured pictures down to N pixels wide (fewer dropped frames at 3x/4x)
#   --max-gb    stop capturing at G GB (default 40)
# FRAME_HOST (default steamos@frame.local) selects the headset. Takes go to ~/ps2-volumetric/captures.
set -euo pipefail
host=${FRAME_HOST:-steamos@frame.local}
dev=0
name=take-$(date +%Y%m%d-%H%M%S)
extra=()
while [[ $# -gt 0 ]]; do
  case $1 in
    --dev) dev=1 ;;
    --name) name=$2; shift ;;
    --width) extra+=(--capture-width "$2"); shift ;;
    --max-gb) extra+=(--capture-max-gb "$2"); shift ;;
    --) shift; extra+=("$@"); break ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
  shift
done

dir=ps2-volumetric/captures/$name
if [[ $dev == 1 ]]; then
  launch="ps2-volumetric/repo/tools/run-frame.sh"
else
  launch="devkit-game/ARMSX2/run.sh"
fi
quoted=
[[ ${#extra[@]} -gt 0 ]] && quoted=$(printf ' %q' "${extra[@]}")

echo "Take $name on $host: put the headset on, calibrate, play to the scene, then the record button"
echo "(under passthrough, beside the screen) starts and stops recording. Quit the game to end the take."
ssh -t "$host" bash -s <<REMOTE
set -euo pipefail
cd ~
mkdir -p "$dir"
export XDG_RUNTIME_DIR=\${XDG_RUNTIME_DIR:-/run/user/\$(id -u)}
# The sound the headset plays (the default output's monitor), timestamped for tools/trailer.
audio_pid=
if command -v pw-record >/dev/null; then
  echo "{\"started_realtime_ns\": \$(date +%s%N)}" > "$dir/audio.json"
  pw-record -P '{ stream.capture.sink = true }' --rate 48000 --channels 2 "$dir/audio.wav" &
  audio_pid=\$!
elif command -v parecord >/dev/null; then
  echo "{\"started_realtime_ns\": \$(date +%s%N)}" > "$dir/audio.json"
  parecord --device=@DEFAULT_MONITOR@ --file-format=wav "$dir/audio.wav" &
  audio_pid=\$!
else
  echo "No pw-record or parecord: the take has no sound."
fi
status=0
"\$HOME/$launch" --capture "\$HOME/$dir"$quoted || status=\$?
if [[ -n \$audio_pid ]]; then kill -INT \$audio_pid 2>/dev/null || true; wait \$audio_pid 2>/dev/null || true; fi
du -sh "$dir" | sed 's/^/Take size: /'
grep -h "Capture:" ~/.local/share/ps2v/logs/ps2v.log 2>/dev/null | tail -3 || true
exit \$status
REMOTE
echo "Fetch it with: tools/trailer/fetch-capture.sh $name"
