#!/usr/bin/env bash
# M0 feasibility spike: run an ARMSX2 nightly on the Steam Frame for a fixed time and log
# ARMSX2's own performance OSD (FPS, speed, EE/GS/GPU load) through periodic gamescope screenshots,
# plus GPU/CPU clocks. Runs on the headset.
# Usage: run-armsx2.sh <seconds> <label> [boot file] [upscale multiplier]
#   With no boot file it boots the BIOS. Results go to ~/ps2-volumetric/m0/results/<label>/.
#   BIOS: BIOS file name in the bios folder (default SCPH-39001.bin). SLOWBOOT=1: full BIOS boot.
#   VPAD: timed button taps on a uinput virtual gamepad (vpad.py), e.g. "15:a 18:start", counted from
#   emulator launch. Simulated X11 keys don't reach apps under gamescope, so this is the input path.
#   EXTRA_INI: extra ini text appended to the generated PCSX2.ini (e.g. to switch off a recompiler).
set -euo pipefail
secs=${1:?seconds}; label=${2:?label}; boot=${3:-}; upscale=${4:-1}
root=~/ps2-volumetric/m0
bin=$root/squashfs-root/AppRun
out=$root/results/$label
cfg=$root/config
rm -rf "$out"; mkdir -p "$out"

# Isolated config: Vulkan, chosen upscale, OSD stats on, setup wizard skipped.
for d in PCSX2 ARMSX2; do
  mkdir -p "$cfg/$d/inis"
  cat > "$cfg/$d/inis/PCSX2.ini" <<EOF
[UI]
SettingsVersion = 1
SetupWizardIncomplete = false
ConfirmShutdown = false
StartFullscreen = true
[Folders]
Bios = $root/bios
[Filenames]
BIOS = ${BIOS:-SCPH-39001.bin}
[EmuCore/GS]
Renderer = 14
upscale_multiplier = $upscale
OsdShowFPS = true
OsdShowSpeed = true
OsdShowCPU = true
OsdShowGPU = true
OsdShowResolution = true
[Logging]
EnableFileLogging = true
[Pad1]
Type = DualShock2
Up = SDL-0/DPadUp
Down = SDL-0/DPadDown
Left = SDL-0/DPadLeft
Right = SDL-0/DPadRight
Cross = SDL-0/FaceSouth
Circle = SDL-0/FaceEast
Square = SDL-0/FaceWest
Triangle = SDL-0/FaceNorth
Start = SDL-0/Start
Select = SDL-0/Back
${EXTRA_INI:-}
EOF
done

export DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 QT_QPA_PLATFORM=xcb LANG=C.UTF-8
export XDG_CONFIG_HOME=$cfg

args=(-batch -fullscreen -logfile "$out/emulog.txt")
[[ -n ${SLOWBOOT:-} ]] && args+=(-slowboot)
if [[ -n $boot ]]; then args+=(-- "$boot"); else args+=(-bios); fi

# Create the virtual pad first so SDL sees it at startup; its tap times then line up with launch.
[[ -n ${VPAD:-} ]] && { python3 "$root/vpad.py" --hold $((secs + 5)) $VPAD >"$out/vpad.txt" 2>&1 & sleep 1; }
"$bin" "${args[@]}" >"$out/stdout.txt" 2>&1 &
pid=$!
# Every 10 s: screenshot of the OSD and the current GPU and big-core clocks.
gpu=/sys/class/devfreq/3d00000.gpu
for ((t = 10; t <= secs; t += 10)); do
  sleep 10
  gamescopectl screenshot "$out/screen-$(printf %03d $t).png" >/dev/null 2>&1 || true
  echo "t=$t gpu_mhz=$(($(cat $gpu/cur_freq) / 1000000)) gpu_max_mhz=$(($(cat $gpu/max_freq) / 1000000)) cpu_khz=$(cat /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq | tr '\n' ' ')" >>"$out/clocks.txt"
done
sleep 2
kill "$pid" 2>/dev/null || true
sleep 3
kill -9 "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
ls -la "$out"
