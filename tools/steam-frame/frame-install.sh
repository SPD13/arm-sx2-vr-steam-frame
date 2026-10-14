#!/usr/bin/env bash
# ARMSX2 - Steam Frame Edition: the install steps that run on the Frame. The installer app sends the release archive over ssh,
# unpacked into a folder next to this script, then runs it; deploy-to-frame.sh runs it on a folder staged from the Frame's own
# build, so every developer deploy also tests the user's install. It installs Valve's devkit scripts and the game folder, registers the game in the Steam library (a devkit title that
# Steam starts inside the Steam Linux Runtime 4.0 ARM64), then names it and installs its artwork and icon.
#
#   frame-install.sh --bundle DIR [--gameid ID] [--name-base64 NAME] [--no-art]
#
#   --bundle DIR          the unpacked release folder (game/, devkit-utils/, library-art/, tools/); deleted when done
#   --gameid ID           identifier of the title for Steam's devkit tools, and its folder in ~/devkit-game (default: ARMSX2)
#   --name-base64 NAME    name of the game in the library, base64 encoded (no quoting through the ssh command line); '-'
#                         keeps Steam's name, "Devkit Game: <gameid>"
#   --no-art              do not install the library artwork
set -euo pipefail

BUNDLE=
GAMEID=ARMSX2
NAME='ARMSX2 - Steam Frame Edition'
ART=1
RUNTIME=SteamLinuxRuntime_4-arm64   # "Steam Linux Runtime 4.0 ARM64", which the game is built for (tools/build-frame.sh)
SHARE=~/.local/share/armsx2-frame   # the tools and artwork shared by every installed copy (another --gameid)
while (( $# )); do
   case $1 in
      --bundle) BUNDLE=${2:?--bundle needs a folder}; shift ;;
      --gameid) GAMEID=${2:?--gameid needs a name}; shift ;;
      --name-base64) if [[ ${2:?--name-base64 needs a value} == - ]]; then NAME=; else NAME=$(printf '%s' "$2" | base64 -d); fi; shift ;;
      --no-art) ART=0 ;;
      *) echo "frame-install.sh: unknown option $1" >&2; exit 2 ;;
   esac
   shift
done

step() { printf '\n==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die() { printf '\ninstall: %s\n' "$*" >&2; exit 1; }

[[ -d $BUNDLE/game && -d $BUNDLE/devkit-utils ]] || die "the game files did not arrive on the Frame"
trap 'rm -rf "$BUNDLE"' EXIT
[[ $GAMEID =~ ^[A-Za-z_][A-Za-z0-9_.]+$ ]] || die "invalid game id '$GAMEID': letters, digits, '_' and '.' only"
[[ $NAME != *[\'\"\\\`\$]* ]] || die "invalid name '$NAME': no quotes, backslashes, backquotes or dollar signs"
[[ $(uname -m) == aarch64 ]] || die "this is a $(uname -m) machine, not a Steam Frame"
note "Steam Frame found"
test -d ~/.local/share/Steam/steamapps/common/$RUNTIME \
   || note "WARNING: 'Steam Linux Runtime 4.0 ARM64' is not installed yet; Steam downloads it the first time the game starts"

step "Valve's devkit scripts"
rm -rf ~/devkit-utils
mv "$BUNDLE/devkit-utils" ~/devkit-utils

step "Game files"
DIRECTORY=$(python3 ~/devkit-utils/steamos-prepare-upload --gameid "$GAMEID" | tail -1 | python3 -c 'import json,sys; print(json.load(sys.stdin)["directory"])')
[[ -n $DIRECTORY ]] || die "the Frame did not prepare the game folder"
note "-> $DIRECTORY"
# The new files replace the folder, so that files removed from the game do not stay behind
rm -rf "$DIRECTORY.new"
mv "$BUNDLE/game" "$DIRECTORY.new"
chmod -R u+rwX,go+rX "$DIRECTORY.new"
chmod +x "$DIRECTORY.new/run.sh" "$DIRECTORY.new/ps2v"
rm -rf "$DIRECTORY"
mv "$DIRECTORY.new" "$DIRECTORY"
note "$(du -sh "$DIRECTORY" | cut -f1) installed"

step "Steam library"
PARMS='{"gameid": "'$GAMEID'", "directory": "'$DIRECTORY'", "argv": ["run.sh"], "env": {}, "settings": {"steam_play": "0", "compat_tool": "'$RUNTIME'"}, "force_appid": "", "lepton_args": ""}'
RESULT=$(python3 ~/devkit-utils/steam-client-create-shortcut --parms "$PARMS" | tail -1)
case $RESULT in
   *'"error"'*) die "Steam did not register the game: $RESULT
    Steam must be running on the Frame: put the headset on so it is in its normal mode, then install again." ;;
esac
mkdir -p "$SHARE"
cp "$BUNDLE/tools/install-library-art.py" "$BUNDLE/tools/set-library-name.py" "$SHARE/"
ICON=
if (( ART )) && [[ -f $BUNDLE/library-art/icon.png ]]; then
   cp "$BUNDLE/library-art/icon.png" "$SHARE/icon.png"
   ICON=$SHARE/icon.png
fi
LIBRARY_NAME="Devkit Game: $GAMEID"
if [[ -n $NAME ]]; then
   if RENAME=$(python3 "$SHARE/set-library-name.py" "$GAMEID" "$NAME" $ICON); then
      LIBRARY_NAME=$NAME
      [[ -z $RENAME ]] || note "$RENAME"
   else
      note "WARNING: the game keeps the name \"$LIBRARY_NAME\": $RENAME"
   fi
fi
note "registered as \"$LIBRARY_NAME\""

if (( ART )) && [[ -d $BUNDLE/library-art ]]; then
   step "Library artwork"
   rm -rf "$SHARE/library-art"
   cp -R "$BUNDLE/library-art" "$SHARE/library-art"
   python3 "$SHARE/install-library-art.py" "$GAMEID" "$SHARE/library-art" | sed 's/^/    /' \
      || note "WARNING: the artwork could not be installed; the game works without it"
fi

step "Done"
note "On the Frame: Library > Non-Steam > \"$LIBRARY_NAME\"."
note "BIOS and games: in the game, open Settings > Wi-Fi Transfer, switch it on, and open the address it shows from a"
note "phone or computer. A PS2 BIOS dump of your own is needed to play."
