#!/usr/bin/env bash
# Build ARMSX2 VR on a Steam Frame (or any ARM64 Linux host) inside a rootless Podman
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

# libarchive, for the zip, 7z and RAR archives sent over Wi-Fi: static, as the Steam Runtime has no
# libarchive (it does have the zlib, liblzma, libbz2 and libzstd it uses).
libarchive=3.8.9
libarchive_sha256=888c934f9d95648ecb9163dc8e23ab80a476ecb81a8f1154704a227b5b676dde
if [[ ! -f $deps/.libarchive-$libarchive ]]; then
  src=$root/build/libarchive-$libarchive
  rm -rf "$src"
  curl -fsSL -o "$src.tar.xz" "https://github.com/libarchive/libarchive/releases/download/v$libarchive/libarchive-$libarchive.tar.xz"
  echo "$libarchive_sha256  $src.tar.xz" | sha256sum -c -
  tar xf "$src.tar.xz" -C "$root/build"
  cmake -S "$src" -B "$src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_PREFIX_PATH="$deps" -DCMAKE_C_COMPILER=clang \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF -DENABLE_TEST=OFF \
    -DENABLE_ZLIB=ON -DENABLE_LZMA=ON -DENABLE_BZip2=ON -DENABLE_ZSTD=ON -DENABLE_ICONV=ON \
    -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_LIBB2=OFF -DENABLE_LZ4=OFF \
    -DENABLE_LZO=OFF -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF \
    -DENABLE_LIBGCC=OFF -DENABLE_CNG=OFF -DENABLE_XATTR=OFF -DENABLE_ACL=OFF
  cmake --build "$src/build"
  cmake --install "$src/build"
  rm -rf "$src" "$src.tar.xz"
  touch "$deps/.libarchive-$libarchive"
fi

# ARMSX2 has to be the top-level CMake project; it pulls the host in through EXTERNAL_FRONTEND_DIR.
cmake -S "$root/external/armsx2" -B "$root/build/frame" -G Ninja \
  -DEXTERNAL_FRONTEND_DIR="$root/host" -DENABLE_VOLUMETRIC=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$deps" \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXE_LINKER_FLAGS_INIT="-fuse-ld=lld" -DCMAKE_MODULE_LINKER_FLAGS_INIT="-fuse-ld=lld" \
  -DENABLE_QT_UI=OFF -DENABLE_SDL_FRONTEND=OFF -DENABLE_GSRUNNER=OFF -DENABLE_TESTS=OFF \
  -DENABLE_RECOMPILER_TEST_HOOKS=OFF -DENABLE_SETCAP=OFF -DUSE_BACKTRACE=ON \
  -DX11_API=OFF -DWAYLAND_API=OFF -DDISABLE_ADVANCE_SIMD=TRUE -DOVERRIDE_HOST_PAGE_SIZE=4096 \
  -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
cmake --build "$root/build/frame" --target ps2v
