#!/usr/bin/env python3
# Runs on the Frame (frame-install.sh copies it there with library-art/, from assets/steam/). Finds the Steam shortcut of a devkit title in
# shortcuts.vdf and copies the artwork into Steam's grid folder under that shortcut's appid, which is how Steam looks up
# custom art for non-Steam games: <appid>p.png (portrait capsule), <appid>.png (wide), <appid>_hero.png, <appid>_logo.png.
# Steam reads that folder only when it starts, so the artwork is also given to the running Steam client
# (SteamClient.Apps.SetCustomArtworkForApp, through set-library-name.py's connection), which shows it at once.
#
# Usage: install-library-art.py <gameid> <art folder>
#        install-library-art.py --remove <gameid>        (before the title is deleted: the appid is read from its shortcut)

import base64
import glob
import importlib.util
import json
import os
import shutil
import struct
import sys
import urllib.request

ART = {'capsule.png': '{}p.png', 'wide.png': '{}.png', 'hero.png': '{}_hero.png', 'logo.png': '{}_logo.png'}
# SetCustomArtworkForApp's asset types
LIVE_TYPES = {'capsule.png': 0, 'hero.png': 1, 'logo.png': 2, 'wide.png': 3}


def parse_binary_vdf(data, pos=0):
   # Steam's binary KeyValues: type byte, NUL-terminated key, value; 0x00 map, 0x01 string, 0x02 int32, 0x08 end of map
   out = {}
   while True:
      t = data[pos]
      pos += 1
      if t == 0x08:
         return out, pos
      end = data.index(b'\0', pos)
      key = data[pos:end].decode('utf-8', 'replace')
      pos = end + 1
      if t == 0x00:
         out[key], pos = parse_binary_vdf(data, pos)
      elif t == 0x01:
         end = data.index(b'\0', pos)
         out[key] = data[pos:end].decode('utf-8', 'replace')
         pos = end + 1
      elif t == 0x02:
         out[key] = struct.unpack_from('<I', data, pos)[0]
         pos += 4
      else:
         raise ValueError(f'unsupported VDF type {t:#x} at {pos - 1}')


def show_now(appids, art):
   # The Steam client's connection lives in set-library-name.py, next to this script
   sys.dont_write_bytecode = True
   try:
      spec = importlib.util.spec_from_file_location('set_library_name', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'set-library-name.py'))
      module = importlib.util.module_from_spec(spec)
      spec.loader.exec_module(module)
      targets = json.load(urllib.request.urlopen(f'http://127.0.0.1:{module.CEF_PORT}/json', timeout=5))
      devtools = module.DevToolsSocket(next(t for t in targets if t.get('title') == 'SharedJSContext')['webSocketDebuggerUrl'])
      for appid in appids:
         for src, kind in LIVE_TYPES.items():
            with open(os.path.join(art, src), 'rb') as f:
               data = base64.b64encode(f.read()).decode()
            devtools.evaluate(f"SteamClient.Apps.SetCustomArtworkForApp({appid}, '{data}', 'png', {kind})")
   except Exception as e:
      return str(e) or type(e).__name__
   return None


def main():
   remove = sys.argv[1] == '--remove'
   gameid = sys.argv[2] if remove else sys.argv[1]
   art = None if remove else sys.argv[2]
   installed = []
   for vdf in glob.glob(os.path.expanduser('~/.local/share/Steam/userdata/*/config/shortcuts.vdf')):
      with open(vdf, 'rb') as f:
         shortcuts, _ = parse_binary_vdf(f.read())
      entries = next((v for k, v in shortcuts.items() if k.lower() == 'shortcuts'), {})   # 'shortcuts' or 'Shortcuts' depending on the client
      for entry in entries.values():
         if entry.get('DevkitGameID') != gameid:
            continue
         appid = entry['appid']
         grid = os.path.join(os.path.dirname(vdf), 'grid')
         os.makedirs(grid, exist_ok=True)
         for src, dst in ART.items():
            target = os.path.join(grid, dst.format(appid))
            if remove:
               if os.path.exists(target):
                  os.remove(target)
            else:
               shutil.copyfile(os.path.join(art, src), target)
         print(f'{entry.get("AppName")}: artwork {"removed" if remove else "installed"} for appid {appid} in {grid}')
         installed.append(appid)
   if not installed:
      sys.exit(f'No Steam shortcut with DevkitGameID {gameid}: register the title first')
   if not remove:
      error = show_now(installed, art)
      print('artwork shown in the library now' if error is None else
            f'it shows once Steam has restarted (restarting the headset does it): the Steam client did not take it ({error})')


if __name__ == '__main__':
   main()
