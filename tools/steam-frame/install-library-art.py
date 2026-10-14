#!/usr/bin/env python3
# Runs on the Frame (frame-install.sh copies it there with library-art/, from assets/steam/). Finds the Steam shortcut of a devkit title in
# shortcuts.vdf and copies the artwork into Steam's grid folder under that shortcut's appid, which is how Steam looks up
# custom art for non-Steam games: <appid>p.png (portrait capsule), <appid>.png (wide), <appid>_hero.png, <appid>_logo.png.
#
# Usage: install-library-art.py <gameid> <art folder>
#        install-library-art.py --remove <gameid>        (before the title is deleted: the appid is read from its shortcut)

import glob
import os
import shutil
import struct
import sys

ART = {'capsule.png': '{}p.png', 'wide.png': '{}.png', 'hero.png': '{}_hero.png', 'logo.png': '{}_logo.png'}


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


def main():
   remove = sys.argv[1] == '--remove'
   gameid = sys.argv[2] if remove else sys.argv[1]
   art = None if remove else sys.argv[2]
   installed = 0
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
         installed += 1
   if not installed:
      sys.exit(f'No Steam shortcut with DevkitGameID {gameid}: register the title first')


if __name__ == '__main__':
   main()
