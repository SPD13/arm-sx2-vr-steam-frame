#!/usr/bin/env bash
# ARMSX2 - Steam Frame Edition: what Steam starts. Steam runs a devkit title's command as is, without the compatibility tool
# frame-install.sh names, so this enters the Steam Linux Runtime 4.0 ARM64 the game is built for (its libraries, OpenXR's
# loader among them), as tools/run-frame.sh does, and starts ps2v from this folder with its output in the same log.
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
log_dir=${XDG_DATA_HOME:-$HOME/.local/share}/ps2v/logs
mkdir -p "$log_dir"
exec > "$log_dir/ps2v.log" 2>&1
if [[ -e /run/pressure-vessel ]]; then   # already in a Steam runtime container
   exec "$here/ps2v" "$@"
fi
runtime=$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4-arm64/run
if [[ ! -x $runtime ]]; then
   echo "run.sh: 'Steam Linux Runtime 4.0 ARM64' is missing: install it from the Steam library, then start the game again."
   exit 1
fi
exec "$runtime" -- "$here/ps2v" "$@"
