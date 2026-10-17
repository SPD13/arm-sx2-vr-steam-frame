#!/usr/bin/env bash
# ARMSX2 VR Steam Frame Edition: the uninstall steps that run on the Frame. The installer app embeds this script (see
# make-installer.sh) and sends it through its ssh command.
#
#   frame-uninstall.sh [--gameid ID] [--settings]
#
#   --settings   also delete the settings, caches and logs in ~/.local/share/ps2v; the games, BIOS files, memory cards, save
#                states and texture packs there are always kept
set -eo pipefail

GAMEID=ARMSX2
SETTINGS=0
SHARE=~/.local/share/armsx2-frame
DATA=~/.local/share/ps2v
while (( $# )); do
   case $1 in
      --gameid) GAMEID=${2:?--gameid needs a name}; shift ;;
      --settings) SETTINGS=1 ;;
      *) echo "frame-uninstall.sh: unknown option $1" >&2; exit 2 ;;
   esac
   shift
done
[[ $GAMEID =~ ^[A-Za-z_][A-Za-z0-9_.]+$ ]] || { echo "uninstall: invalid game id '$GAMEID'" >&2; exit 2; }

if test -d ~/devkit-game/$GAMEID; then
   # Artwork first: it is found through the shortcut, which goes away with the title
   if [ -f $SHARE/install-library-art.py ]; then
      python3 $SHARE/install-library-art.py --remove $GAMEID | sed 's/^/    /' || true
   fi
   # Valve's script deletes the folder, then removes the shortcuts whose folder is gone
   python3 ~/devkit-utils/steamos-delete --delete-title $GAMEID 2>&1 | { grep -E 'deleted shortcut|Not found|failed' || true; } | sed 's/^/    /'
   rm -f ~/devkit-game/$GAMEID-argv.json ~/devkit-game/$GAMEID-settings.json ~/devkit-game/$GAMEID-env.json   # left by Valve's script
else
   echo '    not installed'
fi
# The shared folder serves every installed copy of the game (another --gameid): keep it while one is left
ls ~/devkit-game/*/ps2v > /dev/null 2>&1 || rm -rf $SHARE
if [ $SETTINGS = 1 ]; then
   rm -rf $DATA/inis $DATA/gamesettings $DATA/inputprofiles $DATA/cache $DATA/logs $DATA/debug $DATA/transfer
   echo '    settings, caches and logs deleted'
fi
echo "    done; games, BIOS files, memory cards, save states and texture packs are still in $DATA"
