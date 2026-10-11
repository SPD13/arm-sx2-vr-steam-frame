#!/usr/bin/env bash
# Build PS2 Volumetric on a Steam Frame (or any ARM64 Linux host) inside a rootless Podman
# container running Valve's ARM64 Steam Runtime 4 SDK, the same runtime version as the headset.
# Usage: tools/build-frame.sh [--clean]
#   Static dependencies are built once into build/deps (ARMSX2's own script); delete that
#   directory to rebuild them. The output is build/frame/bin/ps2v (with resources/ beside it).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
[[ $(uname -m) == aarch64 ]] || { echo "Run this build on an ARM64 Linux host (the Steam Frame)." >&2; exit 1; }

sdk=registry.gitlab.steamos.cloud/steamrt/steamrt4/sdk/arm64:4.0.20260805.254769
container=ps2v-build
if [[ ${1:-} != --in-container ]]; then
  if ! podman container exists "$container"; then
    podman create --name "$container" --network host -v "$root:$root" -w "$root" "$sdk" sleep infinity >/dev/null
  fi
  [[ $(podman inspect --format '{{.Config.Image}}' "$container") == "$sdk" ]] || {
    echo "Container $container uses a different SDK image; remove it with: podman rm -f $container" >&2; exit 1;
  }
  podman start "$container" >/dev/null
  exec podman exec "$container" bash "$root/tools/build-frame.sh" --in-container "$@"
fi
shift
[[ ${1:-} == --clean ]] && rm -rf "$root/build/frame"

export DEBIAN_FRONTEND=noninteractive
stamp=/var/lib/ps2v-packages-installed
if [[ ! -f $stamp ]]; then
  apt-get update
  # The package list of ARMSX2's SDL handheld CI build, plus OpenXR.
  apt-get install -y --no-install-recommends \
    build-essential clang lld llvm cmake ninja-build nasm patchelf pkg-config zstd zlib1g-dev git curl \
    libasound2-dev libaio-dev libcurl4-openssl-dev libdbus-1-dev libdrm-dev libegl-dev libevdev-dev libgbm-dev \
    libgudev-1.0-dev libinput-dev libopengl-dev libpcap-dev libpipewire-0.3-dev libpulse-dev libssl-dev libudev-dev \
    libwayland-dev libx11-dev libvulkan-dev libopenxr-dev libopenxr-loader1
  touch "$stamp"
fi

deps=$root/build/deps
if [[ ! -f $deps/.done ]]; then
  ARMSX2_DEPS_STATIC=1 bash external/armsx2/.github/workflows/scripts/linux/build-dependencies-runner.sh "$deps"
  touch "$deps/.done"
fi

# ARMSX2 has to be the top-level CMake project; it pulls the host in through PS2V_HOST_DIR.
cmake -S "$root/external/armsx2" -B "$root/build/frame" -G Ninja \
  -DPS2V_HOST_DIR="$root/host" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$deps" \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXE_LINKER_FLAGS_INIT="-fuse-ld=lld" -DCMAKE_MODULE_LINKER_FLAGS_INIT="-fuse-ld=lld" \
  -DENABLE_QT_UI=OFF -DENABLE_SDL_FRONTEND=OFF -DENABLE_GSRUNNER=OFF -DENABLE_TESTS=OFF \
  -DENABLE_RECOMPILER_TEST_HOOKS=OFF -DENABLE_SETCAP=OFF -DUSE_BACKTRACE=ON \
  -DX11_API=OFF -DWAYLAND_API=OFF -DDISABLE_ADVANCE_SIMD=TRUE -DOVERRIDE_HOST_PAGE_SIZE=4096 \
  -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build "$root/build/frame" --target ps2v
