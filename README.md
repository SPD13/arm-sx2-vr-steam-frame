# PS2 Volumetric

A standalone PlayStation 2 emulator for the Steam Frame. It runs [ARMSX2](https://github.com/ARMSX2/ARMSX2) (an ARM64 fork of PCSX2) inside a native OpenXR app, shows the game on a virtual screen with real 3D depth, and is driven entirely from the headset: a settings menu, a game list, controller mapping and Wi-Fi transfer of games and BIOS files.

> **Status.** The in-headset menu, Wi-Fi transfer, game list and controller mapping (branch `runtime-settings`) compile and were tested on a desktop (menu rendering, scripted clicks, the web server end to end). They have **not yet run on the Steam Frame**. See [Known limitations](#known-limitations). The project plan and milestone notes are in [`PLAN.md`](PLAN.md) and [`docs/`](docs).

## Contents

- [Getting started](#getting-started)
- [In the headset](#in-the-headset)
- [Settings menu](#settings-menu)
- [Game list](#game-list)
- [Wi-Fi transfer](#wi-fi-transfer)
- [Controller mapping](#controller-mapping)
- [Files and folders](#files-and-folders)
- [VR.ini reference](#vrini-reference)
- [Command line](#command-line)
- [How it works](#how-it-works)
- [Known limitations](#known-limitations)
- [License](#license)

## Getting started

### Build

The app is built on the Steam Frame itself (or any ARM64 Linux machine), inside Valve's Steam Runtime 4 ARM64 SDK container, with rootless Podman:

```sh
tools/sync-frame.sh          # from your computer: mirror this repo to steamos@frame.local:ps2-volumetric/repo
# then, on the headset (ssh steamos@frame.local):
tools/build-frame.sh         # first run builds ARMSX2's dependencies into build/deps
```

The result is `build/frame/bin/ps2v`, with `resources/` (game database, shaders, fonts) and `profiles/` beside it. `FRAME_HOST` and `FRAME_DIR` change where `sync-frame.sh` copies to.

### Run

```sh
tools/run-frame.sh                    # opens the game list
tools/run-frame.sh path/to/game.iso   # starts a game directly
```

`run-frame.sh` starts `ps2v` inside the Steam Linux Runtime 4 ARM64 (install it from the Steam library) and logs to `~/.local/share/ps2v/logs/ps2v.log`. The emulator's own log is `~/.local/share/ps2v/logs/emulog.txt`.

### First run

1. Put the headset on: the screen appears in front of you, and with no game given, the game list opens.
2. You need your own PS2 BIOS dump. Open **Settings › Wi-Fi Transfer**, switch it on, open the address it shows in a browser on your computer or phone, and upload the BIOS in the **BIOS** tab. The emulator picks the first BIOS it finds in the folder.
3. Upload games in the **ROMs** tab. They appear in the game list.

## In the headset

Two round buttons sit in the top right corner of the screen:

| Button | Opens |
|---|---|
| ⚙ Gear | The [settings menu](#settings-menu) |
| 🎮 Gamepad (below the gear) | The [game list](#game-list) |

- **Pointing.** Aim either controller at a button or a menu: a beam and a dot show where it points. The controller you clicked with last is the pointer.
- **Clicking.** Pull the trigger. A trigger is a menu click whenever a menu is open or the pointer is on one of the two buttons; only otherwise does it act as the game button it is mapped to (L2 / R2 by default). Aiming near the corner while playing still shoots.
- **Pausing.** Opening a menu pauses the game; closing it resumes.
- **Closing a menu.** Its ✕ button, or a click anywhere off the panel. Starting or loading a game closes it too.
- **Scrolling** the game list: either thumbstick, or drag its scroll bar.
- **Recentering.** Click both thumbsticks together, or use **Settings › Display › Recenter**.

## Settings menu

A navigation bar on the left lists the categories, with **Exit** last.

### Display

| Option | Range | Notes |
|---|---|---|
| Screen size | 1.0 – 4.0 m wide | Applies at once. Saved in `VR.ini`. |
| Screen distance | 1.0 – 5.0 m | The screen slides along its axis. Saved in `VR.ini`. |
| Recenter | button | Places the screen straight ahead. |

### 3D

| Option | Values | Notes |
|---|---|---|
| 3D effect | Off · Portal | **Portal** draws the game from each eye, so the world sits behind the screen like a window. **Off** shows a flat picture. Applies at the next start. |
| Depth | 0 – 5 m | How far behind the screen the farthest scenery appears. |
| Depth source *(advanced)* | Perspective · Depth buffer | What tells how far each object is (Q or Z). Try the other one when a game looks flat or wrong. |
| Screen plane *(advanced)* | 0.1 – 5 (Perspective), 0.01 – 1 (Depth buffer) | Which scene depth sits on the screen glass. Set it so the nearest objects just touch the glass. Time Crisis II: Depth buffer, 0.12, depth 3 m. |

The depth-map "volumetric window" mode (`flat = false`, `stereo = false`) is kept for comparison only and is not offered in the menu; it stays available through `VR.ini` and the command line.

### Graphics

| Option | Values | Stored in |
|---|---|---|
| Internal resolution | Native · 2x · 3x · 4x | `PCSX2.ini`, `[EmuCore/GS] upscale_multiplier` |
| Texture filtering | Nearest · PS2 · Smooth | `PCSX2.ini`, `[EmuCore/GS] filter` (0, 2, 1) |

Both apply while the game runs. Settings that only one renderer supports, or that could break the stereo rendering (renderer choice, aspect ratio, widescreen patches, deinterlacing), are left out on purpose.

### Audio

| Option | Range | Stored in |
|---|---|---|
| Volume | 0 – 100 %, steps of 5 | `PCSX2.ini`, `[SPU2/Output] StandardVolume` |
| Mute | on / off | `PCSX2.ini`, `[SPU2/Output] OutputMuted` |

### Game

- **Save slot** 1 – 5, showing when each slot was saved or that it is empty.
- **Save** writes the running game to the slot. **Load** restores it and closes the menu.
- **Restart game** resets the console, after a confirmation.

Save states are per game, in PCSX2's save state folder.

### Wi-Fi Transfer

| Option | Notes |
|---|---|
| Wi-Fi transfer | Starts or stops the web server. Off each time the app starts. |
| Access code | The page asks for a 6-digit code shown here. On by default; saved in `VR.ini`. |
| New code | Makes a new code and signs every browser out. |

While it runs, the page shows the address to open (for example `http://192.168.1.20:8080`), the code in large type, and the progress of an upload. See [Wi-Fi transfer](#wi-fi-transfer).

### Controls

The DualShock 2's 18 controls with the controller input each one has, and **Reset to defaults**. See [Controller mapping](#controller-mapping).

### Exit

Quits the emulator after a confirmation. Progress since the last save is lost.

## Game list

The gamepad button opens the list of games in the games folder (`roms/`), with five tabs:

| Tab | Shows | Order | Detail line |
|---|---|---|---|
| All | every game | title | serial · size |
| Recently Added | every game | newest file first | "Added today at 14:32" |
| Recent Play | games played at least once | last played first; the running game on top | "Last played yesterday at 08:02" |
| Most Played | games played at least once | total play time | "Played 4 h 31 min" |
| Favorites | starred games | title | serial · size |

- **Favourites.** Click the ☆ next to a game to star it (it turns gold); click again to remove it. Kept in `favorites.txt`.
- **Starting a game.** Click its row. If another game is running, a popup asks first, since progress since the last save is lost. Clicking the running game just resumes it.
- **Starting without a game.** `ps2v` without a disc image starts with no game and opens the list. A game that fails to start brings the list back with the reason.
- **Where the data comes from.** Titles, serials and play time come from PCSX2's game list (play time is recorded when a game stops). A running game keeps the disc drive locked, so the list cannot rescan until it stops: games added meanwhile appear under their file name and get their real title afterwards.
- **"Added"** is the file's inode change time, which an upload, copy or move into the folder sets even when the copy keeps the original modification date. Renaming a file, or changing its permissions, counts as adding it again.
- PS1 discs are not listed: the emulator cannot run them.

## Wi-Fi transfer

Manage the BIOS and game folders from any computer or phone on the same network.

1. In the headset: **Settings › Wi-Fi Transfer**, switch it on.
2. In a browser: open the address shown (port 8080, or the next free one up to 8089) and enter the code if asked.
3. Pick the **BIOS** or **ROMs** tab.

The page shows the files in that folder (name, size, date added) and the space left on the headset. You can:

- **Upload**: choose files, or drag them onto the page. Several files go one after another, each with a progress bar and its speed. A file with a name that is already there asks before replacing it; a file that would not fit is refused before it is sent.
- **Delete**: the 🗑 next to a file, after a confirmation. The running game cannot be deleted.

| Folder | Accepted files |
|---|---|
| BIOS | `.bin .rom0 .rom1 .rom2 .erom .mec .nvm` |
| ROMs | `.iso .chd .cso .zso .gz .bin .cue .img .mdf .mds .nrg .elf` |

Uploads are written to a hidden `.part` file and renamed when complete, so an interrupted upload leaves nothing behind. The headset keeps at least 64 MiB free.

### Security

The server is meant for your own network only:

- **Local clients only.** Connections from outside the private IPv4 ranges (10.x, 172.16–31.x, 192.168.x, 169.254.x, 127.x) are dropped.
- **No DNS rebinding.** The page only answers when it is opened by an IP address, `localhost` or a `.local` name, so a web site cannot point its own domain at the headset.
- **No cross-site requests.** Uploads, deletes and sign-ins need a custom header that pages from other sites cannot send.
- **Access code.** Optional, 6 digits, new each time the server starts. After 5 wrong codes a device is held off for 30 seconds, longer with each further miss. A signed-in browser keeps an `HttpOnly`, `SameSite=Strict` session cookie until the code changes.
- **Safe file names.** Names cannot contain paths, start with a dot or carry control characters, and each folder only takes its own file types.

It is plain HTTP: the code and the files cross your network unencrypted, as with most LAN tools. Switch the transfer off when you are done.

## Controller mapping

**Settings › Controls** lists every DualShock 2 control with the controller input it has. To change one:

1. Click the control.
2. A popup asks you to press what you want for it: a button, grip or trigger, or a stick for the two stick rows. It waits until you have let go of everything first.
3. The control takes that input. The control that had it takes the old input instead, or **Nothing** when another control still uses that one; map it again from the list.

The popup has no Cancel button, because a trigger press there is an input to map; it cancels by itself after 5 seconds. **Reset to defaults** restores the layout below.

### Default layout

| PS2 control | Steam Frame controllers | Touch-layout controllers |
|---|---|---|
| Cross / Circle | A / B (right) | A / B (right) |
| Square / Triangle | X / Y (right) | X / Y (left) |
| D-pad | Left D-pad | Left stick pushed all the way |
| L1 / R1 | Left / right grip | Left / right grip |
| L2 / R2 | Left / right trigger | Left / right trigger |
| L3 / R3 | Left / right stick click | Left / right stick click |
| Start | View button, tap | Menu button, tap |
| Select | View button, hold | Menu button, hold |
| Left / right analog stick | Left / right stick | Left / right stick |

Always, whatever the mapping:

- **Both sticks clicked** recenters the screen; the controls on the stick clicks are released meanwhile.
- **Start and Select on one input**: a tap is Start, holding it half a second is Select. The page shows them as "(tap)" and "(hold)".
- **Triggers** are menu clicks while a menu is open or the pointer is on a menu button, and game buttons otherwise.

Grips and triggers are analog: mapped to L1, R1, L2 or R2 they keep their pressure; on other buttons they press past half way.

## Files and folders

Everything lives in the data folder, `$XDG_DATA_HOME/ps2v` (by default `~/.local/share/ps2v`):

| Path | What |
|---|---|
| `inis/PCSX2.ini` | Emulator settings (graphics, audio and the rest of PCSX2's options) |
| `inis/VR.ini` | Screen, 3D and Wi-Fi transfer settings, see [VR.ini reference](#vrini-reference) |
| `inis/Controls.ini` | Controller mapping, section `[Pad1]`: `cross = a`, `select = menu`, `left_stick = left_stick` … |
| `inis/favorites.txt` | Favourite games, one path per line |
| `inis/playtime.dat` | Play time per game (PCSX2's) |
| `bios/` | PS2 BIOS dumps (Wi-Fi transfer's BIOS tab) |
| `roms/` | Games (Wi-Fi transfer's ROMs tab; the game list scans it, subfolders included) |
| `sstates/` | Save states |
| `profiles/<serial>.ini` | Your own per-game profiles; they take precedence over the ones shipped next to the binary |
| `logs/emulog.txt` | Emulator log |

## VR.ini reference

`VR.ini` holds the VR side's settings. The menu saves what you change a second after you change it. You can also edit the file by hand while the app runs: changes are picked up within a second.

Values are read in this order, each overriding the one before: built-in defaults, `VR.ini`, then the command line. Command-line values apply to that run only and are never written to the file, unless you change the same setting in the menu.

| Section | Key | Default | Range | Command line | Applies |
|---|---|---|---|---|---|
| `[Screen]` | `screen_width` | 2.4 | 0.3 – 10 m | `--screen-width` | live |
| | `screen_distance` | 2.2 | 0.3 – 10 m | `--screen-distance` | live |
| `[Mode]` | `stereo` | false | | `--stereo` | next start |
| | `flat` | false | | `--flat` | next start |
| `[Window]` | `depth_range` | 0.3 | 0 – 5 m | `--depth-range` | live |
| | `pop_out` | 0 | 0 – 1 | `--pop-out` | live |
| | `hud_overlay` | true | | `--no-hud-overlay` | live |
| | `edge_fill` | false | | `--edge-fill` | live |
| `[Stereo]` | `stereo_depth` | 1.0 | 0 – 20 m | `--stereo-depth` | live |
| | `stereo_convergence` | 1.0 | 0.001 – 100 | `--stereo-conv` | live |
| | `stereo_z_mode` | false | | `--stereo-z` | live |
| `[Web]` | `web_require_code` | true | | | live |

Numbers outside their range are clamped. `[Window]` settings belong to the depth-map window mode (`stereo = false`, `flat = false`); `[Stereo]` ones to the Portal mode (`stereo = true`, which always shows a flat screen per eye).

## Command line

```
ps2v [options] [disc image]
```

Without a disc image, the game list opens.

| Option | |
|---|---|
| `--bios-only` | Boot the BIOS without a disc |
| `--no-fast-boot` | Run the BIOS intro before the game |
| `--upscale N` | Internal resolution multiplier, 1 – 8 (saved to `PCSX2.ini`) |
| `--screen-width M`, `--screen-distance M` | Screen size and distance, for this run |
| `--flat`, `--stereo`, `--stereo-depth M`, `--stereo-conv F`, `--stereo-z` | 3D mode and stereo settings, for this run |
| `--depth-range M`, `--pop-out F`, `--edge-fill`, `--no-hud-overlay` | Depth-map window settings, for this run |
| `--sdl-pad` | Drive pad 1 from the first SDL gamepad (testing without the headset) |

Debugging only:

| Option | |
|---|---|
| `--depth-panel` | Show the depth debug view beside the screen |
| `--eye-panels` | Show what each eye renders on two flat panels |
| `--eye-dump DIR`, `--depth-dump DIR` | Write eye images, or depth and colour PNGs, to `DIR` every 10 seconds |
| `--stereo-alternate` | With `--stereo`, the frame-alternating prototype (each eye at 30 Hz) instead of multiview |
| `--load-state FILE` | Start from a save state; `SIGUSR1` saves `debug/state.p2s`, `SIGUSR2` writes a frame snapshot |
| `--depth-trace`, `--depth-log-ms N` | Log the colour and depth targets of 3D draws; how often to log depth statistics |

## How it works

The host code is in [`host/`](host); ARMSX2 is the submodule [`external/armsx2`](external/armsx2) (our fork, branch `ps2v`).

| Area | Files | Notes |
|---|---|---|
| Settings | `Settings.{h,cpp}` | `VrSettingsStore`: a table of settings layered from defaults, `VR.ini` and the command line, saved after a change settles, reloaded when the file is edited. `main.cpp` applies changes live. |
| Menus | `ui/` | Drawn on the CPU (`Canvas`, `Font` with stb_truetype, `Gui`, an immediate-mode widget set) and shown as alpha-blended OpenXR quad layers (`OverlayLayer`). PCSX2's ImGui context is a process-wide global that its GS threads drive, so the XR thread cannot run an ImGui frame of its own. A full menu redraw takes under half a millisecond and only happens when something visible changes. |
| Menu in the headset | `ui/VrMenu.{h,cpp}` | The two buttons, the panel, the pointer (controller aim poses), pausing, and keeping menu clicks from the game. |
| Pages | `ui/SettingsMenu.{h,cpp}`, `ui/GameListMenu.{h,cpp}` | The settings categories, and the game list. |
| Controllers | `XrApp.{h,cpp}`, `Controls.{h,cpp}` | `XrApp` reports the controllers as they are (one OpenXR action per button, grip, trigger and stick); `ControlMap` turns that into the DualShock 2 state. |
| Wi-Fi transfer | `web/` | `WebServer`, a small POSIX HTTP server; the page `web/index.html` is embedded into the binary at build time by `web/embed.cmake`. |
| Emulator | `Emu.{h,cpp}` | The CPU thread, PCSX2's host callbacks, and what the menus need: pause, settings, save states, the game list, switching games. Switching runs on the CPU thread, and resuming from a menu is skipped while a switch is under way so it cannot cancel it. |

## Known limitations

- **Not yet run on the Steam Frame.** These features were built and tested on a desktop; the headset may show things the desktop could not, such as runtime binding quirks, layer placement or performance.
- If the runtime rejects the controllers' aim pose, the gamepad still works but menus cannot be pointed at. The log says "no aim pose …; the menu pointer is unavailable."
- In Portal mode, both eyes may show the same picture while a menu has the game paused.
- Changing the internal resolution or texture filtering mid-game uses PCSX2's normal settings path, but has not been tried with the stereo changes to the renderer.
- The controller actions were renamed for mapping, so any SteamVR-side rebinding made for this app starts over.
- Wi-Fi transfer is plain HTTP, and whether SteamOS lets port 8080 through has not been checked.
- The game list cannot rescan while a game runs (see [Game list](#game-list)).

## License

GPL-3.0-or-later, as ARMSX2 and PCSX2 are. Fonts and icons come from PCSX2's resources (Roboto, Font Awesome).
