#!/usr/bin/env bash
# ARMSX2 - Steam Frame Edition: what Steam starts, inside the Steam Linux Runtime 4.0 ARM64 (the title's compatibility tool,
# set by frame-install.sh). Starts ps2v from this folder, with its output in the log that tools/run-frame.sh also writes.
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
log_dir=${XDG_DATA_HOME:-$HOME/.local/share}/ps2v/logs
mkdir -p "$log_dir"
exec "$here/ps2v" "$@" > "$log_dir/ps2v.log" 2>&1
