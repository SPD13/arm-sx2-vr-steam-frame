# Trailer kit

Footage for a trailer, recorded in the headset and rendered on a Mac. A flat video loses the stereo of the volumetric view, so the kit records what the headset showed (both eyes' pictures, each eye's depth, the head, the screen and the light guns) and rebuilds the scene behind the window. It then films that scene with smooth camera moves, where parallax shows the depth on any screen.

| Part | What it does |
|---|---|
| `ps2v --capture DIR` (host/capture) | Records a take on the Frame. |
| `capture-frame.sh` | Starts a take from the Mac over ssh, with the headset's sound. |
| `fetch-capture.sh` | Copies a take to the Mac. |
| `render/` (`trailer`) | Swift and Metal: `info`, `still`, `render`, `verify`. |
| `trailer.example.json` | An edit list for the five features: volumetric, light gun, Wi-Fi uploads, standalone, one-click install. |
| `synth/synth_capture.cpp` | A synthetic take, written by the host's own writer, for testing without the headset. |

## 1. Record takes on the Frame

The game must be a build with `--capture`. Deploy it with `tools/steam-frame/deploy-to-frame.sh`, or use `--dev` with a development build.

```sh
tools/trailer/capture-frame.sh --name tc2-a            # Time Crisis 2 from the game list
tools/trailer/capture-frame.sh --name tc2-b -- ~/ps2-volumetric/games/TimeCrisisIIDVD-dvdfix.iso
tools/trailer/fetch-capture.sh --list
tools/trailer/fetch-capture.sh tc2-a                   # to ~/ps2v-captures/tc2-a
```

Play normally, then quit with the power button to end the take. A menu pauses the game, so it splits a take: `trailer info` lists the parts. The capture keeps going while you switch between Flat and Volumetric, and the cut can use that switch.

Tips for footage that renders well:
- Volumetric mode, with the screen recentered in front of you.
- Hold your head fairly still. The renderer adds the camera moves, and a steady head gives clean pictures to rebuild from.
- For the light gun, aim slowly at targets in the middle of the picture, then fire.
- The capture writes about 150–400 MB/s, depending on the upscale. `trailer info` reports dropped frames. If it drops any, pass `--width 1280`. The take stops at `--max-gb`, 40 GB by default.

Sound comes from `pw-record`, which records the headset's output on the side. If it is out of sync, `audio.offset` (seconds) in the edit list shifts it.

## 2. Build the renderer and look at a take

```sh
cd tools/trailer/render && swift build -c release && cd -
T=tools/trailer/render/.build/release/trailer
$T info ~/ps2v-captures/tc2-a        # takes, 3D mode, light gun shots, menus, sound
$T verify ~/ps2v-captures/tc2-a      # rebuilt view vs the right eye's real picture
```

`verify` rebuilds the scene from the left eye, renders it from where the right eye was, and compares the result with the right eye's real picture. On the synthetic take, the rebuilt view scores about 32–36 dB, against 24 dB for the left picture as is. With `TRAILER_VERIFY_PNG=DIR` it also writes the pictures and the depth map.

## 3. Edit list and render

Copy `trailer.example.json` next to your takes. For each shot, set `start` (seconds into the take, from `trailer info`). Then preview single frames and render:

```sh
$T still my-trailer.json 4 2.5 /tmp/shot4.png     # shot 4, 2.5 s in
$T render my-trailer.json                         # every shot, then the cut
$T render my-trailer.json --only 4,7              # redo some shots (the cut is assembled again)
```

The output folder holds `shots/NN-name.mov` (ProRes 422 HQ with sound, ready for an editor) and `trailer.mp4`, a first cut with the music mixed under it.

### Shots

| `type` | What it shows |
|---|---|
| `portal` | The window and the game's world behind it, filmed from a moving camera, with the guns and their beams. |
| `head` | The player's own view, smoothed (`smooth`: seconds on each side, `fov`). |
| `compare` | The same camera move with Flat on the left half and Volumetric on the right (`labels`). |
| `reveal` | The camera swings to the side with the window's clipping off: the scene as a diorama behind the glass. |
| `wiggle` | The two eyes' real pictures, swapped `hz` times a second: depth without a headset. |
| `title` | A card: `kicker`, `title`, `subtitle`. |
| `clip` | An outside video, such as a screen recording, conformed to the cut: `file`, `start`, `duration` (all of it by default). |

Cameras (`camera`): `dolly`, `dolly-back`, `push`, `lean`, `orbit`, `reveal`, `front`, `gun` (over the shoulder of the first gun, looking down its beam). A list of keyframes also works: `[{"t": 0, "pos": [x, y, z], "look": [x, y, z]}, ...]`. Coordinates are in meters in the screen's frame: the origin at the screen's centre, x right, y up, z toward the viewer.

Any shot can carry a `caption` and a `detail` line (lower left, fading in at `caption_at`). Fades: `fade`, `fade_in`, `fade_out`. Picture options: `clip`, `fill`, `outline`, `floor`, `glass`, `guns`, `flat`, `step` (grid, in pixels), `edge`, `eye` (0 = rebuild from the left eye, 1 = from the right).

### Footage the kit cannot record

The game's pictures are only part of the trailer. These need ordinary recordings, used as `clip` shots:
- **Wi-Fi uploads**: the transfer page on a phone (its screen recorder) or in a browser on the Mac (Cmd+Shift+5) while a game uploads. A second clip can show the game list in the headset with the new game in it.
- **Standalone**: headset footage of starting the game from the Steam library and the game list opening. Record it with the headset's own recording, or the SteamVR mirror.
- **One-click install**: a screen recording of the installer app going through its steps.

## How the rebuild works

In Volumetric mode the GS shifts every draw by the eye's offset times `k`. That is exactly where a point `d` behind the glass appears from that eye, with `k = d / (D + d)` for an eye `D` in front of the glass. Each captured pixel is therefore cast from the eye that saw it, through the glass, to its depth:
- Volumetric: `k = depth_factor · clamp(1 − Znorm / convergence, 0, 1)`, so `d = D k / (1 − k)`.
- Volumetric+: `d = D0 (1 / u − 1)`, with `u = Znorm / convergence`.

The depth image has the PS2's resolution, so each picture pixel takes the depth of a texel around it. The choice weighs two things: whose colour matches the pixel's, and which depth the other eye's picture agrees with. A draw the GS left unshifted, such as an overlay without Z, has someone else's depth under it. It goes on the glass when the other eye shows it unshifted and nothing hides that spot from that eye. Behind the rebuilt picture, a fill layer extends the nearest background into the gaps that no eye saw, and stretches the picture's edges outward. `Shaders.swift` has the details.

Limits: a camera far off to the side sees what neither eye saw. The `reveal` shot leaves it dark, and the other shots fill it with background. Keep the moves within about 40 cm of where the player sat.

## Testing without the headset

```sh
clang++ -std=c++20 -O2 -I host -I /opt/homebrew/include tools/trailer/synth/synth_capture.cpp \
  host/capture/CaptureWriter.cpp -L/opt/homebrew/lib -llz4 -o /tmp/synth_capture
/tmp/synth_capture /tmp/synth-take 4 1280            # 4 s, volumetric (or: plus, flat)
$T info /tmp/synth-take && $T verify /tmp/synth-take
```
