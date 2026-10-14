# Sourced by make-release.sh and deploy-to-frame.sh (run from your computer): brings the Frame's build up to date with this
# checkout and gives it Valve's devkit scripts, ready for stage-release.sh.
#
# FRAME_HOST (default steamos@frame.local) and FRAME_DIR (default ps2-volumetric/repo, as tools/sync-frame.sh) select the
# Frame and the repo mirror on it. Needs Developer Mode on the Frame and an ssh key it accepts (see the README).

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
host=${FRAME_HOST:-steamos@frame.local}
dir=${FRAME_DIR:-ps2-volumetric/repo}
devkit_src=$root/build/steamos-devkit
# Beside the repo mirror on the Frame, not in it: sync-frame.sh deletes what this checkout does not have
remote_devkit=$(dirname "$dir")/devkit-utils
remote_release=$(dirname "$dir")/release

# Valve's devkit scripts (gitlab.steamos.cloud/devkit/steamos-devkit, MIT): steamos-prepare-upload, steam-client-create-shortcut
# and steamos-delete register and remove a title in the Frame's Steam library
prepare_frame() {
   if [[ ! -d $devkit_src/client/devkit-utils ]]; then
      echo "==> Fetching Valve's devkit scripts into $devkit_src"
      git clone -q --depth 1 https://gitlab.steamos.cloud/devkit/steamos-devkit.git "$devkit_src"
   fi
   echo "==> Syncing to $host:$dir"
   FRAME_HOST=$host FRAME_DIR=$dir "$root/tools/sync-frame.sh"
   echo "==> Building on the Frame"
   ssh "$host" "cd $dir && tools/build-frame.sh"
   rsync -a --delete --exclude __pycache__ "$devkit_src/client/devkit-utils/" "$host:$remote_devkit/"
   rsync -a "$devkit_src/LICENSE" "$host:$remote_devkit/LICENSE"
}
