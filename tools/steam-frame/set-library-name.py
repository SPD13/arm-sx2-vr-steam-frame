#!/usr/bin/env python3
# Runs on the Frame (frame-install.sh copies it there). Gives a devkit title its name in the Steam library: Steam's devkit registration
# always names it "Devkit Game: <gameid>". The name is set through the Steam client itself (SteamClient.Apps.SetShortcutName,
# evaluated in its SharedJSContext over the CEF debugging port that Steam opens on 127.0.0.1 in Developer Mode), so it applies at
# once, without restarting Steam, and Steam saves it in shortcuts.vdf. It is kept when the title is registered again (updates).
#
# With an icon (a PNG on the Frame), it also sets the shortcut's icon (SteamClient.Apps.SetShortcutIcon), shown in the library
# list and on the game page; a failure there is only reported, the name counts.
#
# Usage: set-library-name.py <gameid> <name> [icon]
# Exit code 0 when the name is set, 1 otherwise (the title then keeps Steam's name).

import base64
import glob
import json
import os
import socket
import struct
import sys
import time
import urllib.request

CEF_PORT = 8080


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


def find_appids(gameid):
   appids = []
   for vdf in glob.glob(os.path.expanduser('~/.local/share/Steam/userdata/*/config/shortcuts.vdf')):
      with open(vdf, 'rb') as f:
         shortcuts, _ = parse_binary_vdf(f.read())
      entries = next((v for k, v in shortcuts.items() if k.lower() == 'shortcuts'), {})
      appids += [entry['appid'] for entry in entries.values() if entry.get('DevkitGameID') == gameid]
   return appids


class DevToolsSocket:
   # Minimal WebSocket client (text frames only), as the Frame's Python has no websocket module

   def __init__(self, url):
      host_port, path = url[len('ws://'):].split('/', 1)
      host, port = host_port.rsplit(':', 1)
      self.sock = socket.create_connection((host, int(port)), timeout=10)
      key = base64.b64encode(os.urandom(16)).decode()
      self.sock.sendall((f'GET /{path} HTTP/1.1\r\nHost: {host_port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                         f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
      response = b''
      while b'\r\n\r\n' not in response:
         chunk = self.sock.recv(4096)
         if not chunk:
            raise ConnectionError('connection closed during the handshake')
         response += chunk
      if b' 101 ' not in response.split(b'\r\n', 1)[0]:
         raise ConnectionError('handshake refused')
      self.next_id = 0

   def _send(self, text):
      payload = text.encode()
      n = len(payload)
      header = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n) if n < 65536
                                else bytes([0x80 | 127]) + struct.pack('>Q', n))
      mask = os.urandom(4)
      self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

   def _recv_exact(self, n):
      data = b''
      while len(data) < n:
         chunk = self.sock.recv(n - len(data))
         if not chunk:
            raise ConnectionError('connection closed')
         data += chunk
      return data

   def _receive(self):
      message = b''
      while True:
         b0, b1 = self._recv_exact(2)
         n = b1 & 0x7F
         if n == 126:
            n = struct.unpack('>H', self._recv_exact(2))[0]
         elif n == 127:
            n = struct.unpack('>Q', self._recv_exact(8))[0]
         payload = self._recv_exact(n)
         if b0 & 0x0F in (0x1, 0x0):
            message += payload
            if b0 & 0x80:
               return message.decode('utf-8', 'replace')

   def evaluate(self, expression):
      self.next_id += 1
      self._send(json.dumps({'id': self.next_id, 'method': 'Runtime.evaluate',
                             'params': {'expression': expression, 'awaitPromise': True, 'returnByValue': True}}))
      while True:
         reply = json.loads(self._receive())
         if reply.get('id') == self.next_id:
            result = reply.get('result', {})
            if 'exceptionDetails' in result:
               raise RuntimeError(result['exceptionDetails'].get('text', 'JavaScript error'))
            return result.get('result', {}).get('value')


def main():
   gameid, name = sys.argv[1], sys.argv[2]
   icon = sys.argv[3] if len(sys.argv) > 3 else None
   appids = find_appids(gameid)
   if not appids:
      print(f'no Steam shortcut for {gameid}')
      return 1
   try:
      targets = json.load(urllib.request.urlopen(f'http://127.0.0.1:{CEF_PORT}/json', timeout=5))
      target = next(t for t in targets if t.get('title') == 'SharedJSContext')
      devtools = DevToolsSocket(target['webSocketDebuggerUrl'])
   except Exception as e:
      print(f'the Steam client cannot be reached ({e})')
      return 1
   for appid in appids:
      # The title may take a moment to reach the library after its registration
      expression = f'''(async () => {{
         const appid = {appid}, name = {json.dumps(name)};
         const current = () => window.appStore?.GetAppOverviewByAppID(appid)?.display_name;
         for (let i = 0; i < 50 && !current(); i++) await new Promise(r => setTimeout(r, 200));
         if (!current()) return 'missing';
         if (current() !== name) SteamClient.Apps.SetShortcutName(appid, name);
         for (let i = 0; i < 25 && current() !== name; i++) await new Promise(r => setTimeout(r, 200));
         return current() === name ? 'ok' : 'unchanged';
      }})()'''
      try:
         status = devtools.evaluate(expression)
      except Exception as e:
         print(f'the Steam client did not rename {gameid} ({e})')
         return 1
      if status != 'ok':
         print(f'the Steam client did not rename {gameid} ({status})')
         return 1
      if icon:
         expression = f'''(() => {{
            if (typeof SteamClient.Apps.SetShortcutIcon !== 'function') return 'not available';
            SteamClient.Apps.SetShortcutIcon({appid}, {json.dumps(icon)});
            return 'ok';
         }})()'''
         try:
            status = devtools.evaluate(expression)
         except Exception as e:
            status = str(e)
         if status != 'ok':
            print(f'WARNING: the Steam client did not set the icon ({status})')
   return 0


if __name__ == '__main__':
   sys.exit(main())
