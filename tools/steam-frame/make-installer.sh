#!/bin/zsh
# Builds the installer app (installer/, in Go) for Windows, macOS and Linux, for the GitHub release beside the game archive
# (make-release.sh). The app downloads the latest release's .tar.gz itself, so the same build serves later releases: attach
# these files to each new release (their names stay the same, so
# https://github.com/SPD13/arm-sx2-vr-steam-frame/releases/latest/download/<name> always gives the newest).
#
# Usage: tools/steam-frame/make-installer.sh [version]      (default: <date>-<short commit>)
# Output: build/installer/
#
#   ARMSX2-Frame-Installer-Windows.exe          Windows 10/11 x64 (WebView2 window; runs on Windows on Arm too)
#   ARMSX2-Frame-Installer-macOS.zip            "ARMSX2 Installer.app", universal (Apple silicon and Intel), macOS 12+ (the minimum of Go)
#   ARMSX2-Frame-Installer-Linux-x86_64.tar.gz  a static binary that opens in the browser (any distribution, SteamOS desktop)
#   ARMSX2-Frame-Installer-Linux-arm64.tar.gz
#
# Needs Go (brew install go) and Xcode's command line tools, so it runs on a Mac; the Windows icon uses go-winres (installed in
# the Go bin folder on first use). Not signed: Windows SmartScreen and macOS Gatekeeper warn on first launch (see the README).

set -e

HERE="${0:A:h}"
ROOT="${HERE:h:h}"
SRC="$HERE/installer"
ART="$ROOT/assets/steam"
OUT="$ROOT/build/installer"
GO=${GO:-$(command -v go || echo /opt/homebrew/bin/go)}
[[ -x $GO ]] || { echo "Go is missing: brew install go"; exit 1 }
VERSION="${1:-$(date +%Y%m%d)-$(git -C "$HERE" rev-parse --short HEAD)}"
LDFLAGS="-s -w -X main.version=$VERSION"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
rm -rf "$OUT"
mkdir -p "$OUT"

# The Frame-side uninstall steps travel inside the app (the install steps come with each release); the copy is not committed
mkdir -p "$SRC/embedded"
cp "$HERE/frame-uninstall.sh" "$SRC/embedded/frame-uninstall.sh"
cd "$SRC"

echo "==> Windows"
WINRES="$($GO env GOPATH)/bin/go-winres"
[[ -x $WINRES ]] || $GO install github.com/tc-hib/go-winres@v0.3.3
# The icon and the manifest (per-monitor DPI, modern controls) as a resource the Go linker picks up: rsrc_windows_amd64.syso
"$WINRES" simply --arch amd64 --out rsrc --icon "$ART/icon.png" --manifest gui \
   --product-name "ARMSX2 - Steam Frame Edition Installer" --file-description "ARMSX2 - Steam Frame Edition Installer" \
   --product-version "0.0.0.0" --file-version "0.0.0.0" --original-filename ARMSX2-Frame-Installer-Windows.exe
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 $GO build -trimpath -ldflags "$LDFLAGS -H windowsgui" -o "$OUT/ARMSX2-Frame-Installer-Windows.exe" .
rm -f rsrc_windows_*.syso

echo "==> macOS"
APP="$WORK/ARMSX2 Installer.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
for arch in arm64 amd64; do
   clang_arch=$arch; [[ $arch == amd64 ]] && clang_arch=x86_64
   GOOS=darwin GOARCH=$arch CGO_ENABLED=1 CC="clang -arch $clang_arch" MACOSX_DEPLOYMENT_TARGET=12.0 \
      $GO build -trimpath -ldflags "$LDFLAGS" -o "$WORK/installer-$arch" . 2> "$WORK/build-$arch.log" \
      || { cat "$WORK/build-$arch.log"; exit 1 }
done
lipo -create "$WORK/installer-arm64" "$WORK/installer-amd64" -output "$APP/Contents/MacOS/armsx2-frame-installer"
ICONSET="$WORK/icon.iconset"
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
   sips -z $size $size "$ART/icon.png" --out "$ICONSET/icon_${size}x${size}.png" > /dev/null
   sips -z $((size * 2)) $((size * 2)) "$ART/icon.png" --out "$ICONSET/icon_${size}x${size}@2x.png" > /dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/icon.icns"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$APP/Contents/Resources/"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
   <key>CFBundleExecutable</key><string>armsx2-frame-installer</string>
   <key>CFBundleIdentifier</key><string>io.github.spd13.armsx2-frame-installer</string>
   <key>CFBundleName</key><string>ARMSX2 Installer</string>
   <key>CFBundleDisplayName</key><string>ARMSX2 Installer</string>
   <key>CFBundleIconFile</key><string>icon</string>
   <key>CFBundlePackageType</key><string>APPL</string>
   <key>CFBundleShortVersionString</key><string>${VERSION%%-*}</string>
   <key>CFBundleVersion</key><string>$VERSION</string>
   <key>LSMinimumSystemVersion</key><string>12.0</string>
   <key>NSHighResolutionCapable</key><true/>
   <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
   <key>NSLocalNetworkUsageDescription</key><string>The installer finds your Steam Frame on the network and installs the game on it.</string>
   <key>NSBonjourServices</key><array><string>_steamos-devkit._tcp</string></array>
   <key>NSAppTransportSecurity</key><dict><key>NSAllowsLocalNetworking</key><true/></dict>
</dict>
</plist>
EOF
codesign --force --deep --sign - "$APP"   # ad-hoc: Apple silicon runs only signed code
ditto -c -k --keepParent "$APP" "$OUT/ARMSX2-Frame-Installer-macOS.zip"

echo "==> Linux"
for arch in amd64 arm64; do
   name=x86_64; [[ $arch == arm64 ]] && name=arm64
   mkdir -p "$WORK/linux-$arch"
   GOOS=linux GOARCH=$arch CGO_ENABLED=0 $GO build -trimpath -ldflags "$LDFLAGS" -o "$WORK/linux-$arch/ARMSX2-Frame-Installer" .
   cp "$ROOT/THIRD_PARTY_NOTICES.md" "$WORK/linux-$arch/"
   tar -C "$WORK/linux-$arch" -czf "$OUT/ARMSX2-Frame-Installer-Linux-$name.tar.gz" ARMSX2-Frame-Installer THIRD_PARTY_NOTICES.md
done

# The Windows installer is a single .exe: the notices go on the release beside it
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$OUT/"

ls -lh "$OUT"
