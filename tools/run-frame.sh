#!/usr/bin/env bash
# Run the PS2 Volumetric build on the Steam Frame (on the headset, e.g. over ssh), inside the
# Steam Linux Runtime 4 ARM64 container the build targets.
# Usage: tools/run-frame.sh [ps2v arguments]   e.g. tools/run-frame.sh --upscale 3 game.iso
# The log goes to stdout and to ~/.local/share/ps2v/logs/ps2v.log.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
bin=$root/build/frame/bin/ps2v
[[ -x $bin ]] || { echo "No build at $bin; run tools/build-frame.sh first." >&2; exit 1; }
runtime=$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4-arm64/run
[[ -x $runtime ]] || { echo "Install Steam Linux Runtime 4.0 ARM64 from the Steam library." >&2; exit 1; }

export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
log_dir=${XDG_DATA_HOME:-$HOME/.local/share}/ps2v/logs
mkdir -p "$log_dir"
"$runtime" -- "$bin" "$@" 2>&1 | tee "$log_dir/ps2v.log"
