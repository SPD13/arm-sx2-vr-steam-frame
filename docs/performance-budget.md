# Where a frame's time goes, and what is left to gain (measured 2026-10-15)

After the blending-accuracy fix (`docs/performance-stutter.md`), the gunfights run at full rate. This document splits a heavy frame's work between the emulator, the host and SteamVR, and lists what could still be cut.

| | |
|---|---|
| **CPU** | The emulator's two threads do about 95% of the CPU work. Its core thread (EE + VU1) is at 60–90% of one core in fights: the next limit. |
| **GPU** | The emulator about two thirds, SteamVR's compositor about one third, about 45% idle at 72 Hz. |
| **First lever** | MTVU (VU1 on its own thread): 59.1 → 59.8 fps in the heaviest saved scene, measured headless. |
| **Layers** | The menu's screen-plane pieces merged into one quad: 16 → 11 layers at most. Neither the layer count nor transparent area costs the compositor anything measurable; passthrough and its own work do. |
| **Not worth it now** | Lower resolution, foveated rendering (the GPU has headroom), EE cycle-rate hacks (they change the game's timing). |

## 1. Sources

- **`PerfLog`** every 30 s in `emulog.txt`: game fps, the core thread's (EE), the GS thread's and the VU thread's busy share of a game frame, and the emulator's GPU share.
- **SteamVR's per-app stats**, `~/.local/share/Steam/logs/vrcompositor.txt` (see `docs/performance-stutter.md` §1): per 72 Hz frame, the compositor's CPU and GPU time and the app's.
- **`top -H`** on the running process for the threads' CPU, and `ps -L -o psr` for the core each runs on.
- The headset session of 2026-10-14 22:40–22:48 (Time Crisis II gunfights, blending Minimum, 3x, Volumetric+, passthrough and guns on), and headless runs on 2026-10-15 from the saved states `state-room2.p2s` (a calm room) and `debug/state-stutter.p2s` (the heaviest fight saved).

## 2. A heavy frame

**CPU**, per game frame (16.7 ms at 60 fps). The threads run in parallel, one core each:

| Thread | Busy | Per frame | Share of the CPU work |
|---|---|---|---|
| Emulator core thread, `CPU Thread` (EE, and VU1 with MTVU off) | 60–90% | 10–15 ms | ~60% |
| Emulator GS thread, `GS` (GS emulation, Vulkan recording) | 30–55% | 5–9 ms | ~33% |
| Host, `ps2v` (OpenXR, layers, input, guns) | ~5% | ~0.8 ms | ~4% |
| SteamVR compositor | | ~0.5 ms | ~3% |

The host's share is SteamVR's "ApplicationTime CPU" (0.70 ms per 72 Hz frame in that session); headless, `top` shows the host thread at 2.6% of a core.

**GPU**, per display frame (13.9 ms at 72 Hz). One GPU, so the work adds up:

| Work | Per frame | Share of the GPU work |
|---|---|---|
| Emulator (3x, both eyes in each multiview pass) | 4–6 ms (PerfLog 28–42% of a 16.7 ms game frame) | ~65% |
| SteamVR compositor (up to 16 layers, passthrough) | 2.6 ms | ~35% |
| Idle | ~6 ms | about 45% headroom |

**Calm room**, headless: core thread 47%, GS thread 18%, GPU 18%.

**The core thread is the limit now.** In `state-stutter.p2s`, headless, it ran at 88–89% and the game at 59.1 fps instead of 59.94 (1.4% slow); in the headset, the heaviest stretches logged 59.2–59.5 fps at 83–90%. That is the game slowing down, not SteamVR's half-rate stutter. The share stays under 100% because the thread also waits on the GS thread and on vsync.

Compositor GPU per 72 Hz frame, from the per-app stats:

| Session | Compositor GPU |
|---|---|
| Headless (nothing shown: the headset is not worn) | 0.67–0.89 ms |
| Headset, passthrough off | 1.38–1.57 ms |
| Headset, passthrough on, 16 layers (2026-10-14 22:48) | 2.61 ms |

## 3. MTVU

`PCSX2.ini [EmuCore/Speedhacks] vuThread` is `false`: VU1, which Time Crisis II uses for most of its geometry, runs on the core thread. Headless, `state-stutter.p2s`, 65 s per run, alternated:

| vuThread | fps | Core thread | GS thread | VU thread |
|---|---|---|---|---|
| false | 59.1, 59.1 | 69–89% | 38–42% | – |
| true | 59.7–59.8, 59.7–59.8 | 77–90% | 58–67% | 69–77% |

With MTVU the game is back at full speed; the core thread's share stays high because it now waits for VU1 instead of running it. The GS thread is busier because more frames come through. The Frame has 8 cores (2 Cortex-A520, 5 Cortex-A720, 1 Cortex-X4) and the emulator uses about 2, so the extra thread has room. MTVU breaks a few games; Time Crisis II has not been checked in the headset with it (picture, gun timing).

## 4. Where the threads run

The cores, from `/sys/devices/system/cpu`: cpu0–1 Cortex-A520 (2.27 GHz), cpu2–6 Cortex-A720 (3.15 / 2.96 GHz), cpu7 Cortex-X4 (3.05 GHz, the fastest per clock). Governor `schedutil`.

In 10 one-second samples during the heavy scene, the core thread ran on cpu7 five times, on the A720s three times and **on the little A520s twice**. The GS thread stayed on the A720s. Pinning the core thread to cpu7 (or at least off cpu0–1) is untested.

## 5. The compositor's layers

Until 2026-10-15 the host showed up to 16 quad layers, SteamVR's limit on the Frame: passthrough's back projection layer, the two screen layers, nine menu layers (panel, three button groups, tooltip, the line below the screen, the messages above it, the pointer's beam and its dot) and four gun panes (two per eye). Since SteamVR matches quad layers by their place in the list, every slot up to the last one in use is filled, the empty ones by a tiny transparent "pad" quad far out of sight; with the guns or the controllers showing, that is always 16 layers.

Measured with temporary switches (not kept): `PS2V_PAD_TO=16` fills every slot, `PS2V_BIG_PAD` adds a transparent quad of 3.4 × 2.6 m, 2 m in front of the head (most of the view). 50 s runs from `state-room2.p2s`, compositor GPU per 72 Hz frame:

| Headset worn | Run 1 | Run 2 |
|---|---|---|
| As is (16 layers: controllers in view) | 2.30 ms | 2.04 ms |
| Every slot filled | 2.23 ms | 2.13 ms |
| Plus the large transparent quad | 1.89 ms | 2.06 ms |

| Headset off (`PS2V_PLACE_ANYWAY` placed the screen) | Run 1 | Run 2 |
|---|---|---|
| 7 layers | 0.869 ms | 0.902 ms |
| 16 layers | 0.871 ms | 0.884 ms |

- **A transparent quad covering most of the view costs nothing measurable** (noise about ±0.2 ms worn). So a large, mostly transparent quad around the screen costs no more than the small ones it replaces.
- The layer count could not be compared in these runs: with the controllers in view the normal run already had 16 layers. Headless, 7 and 16 layers cost the same, but headless the compositor skips most of its work (0.9 ms against 2.0–2.6 ms worn). The headset session after the merge settled it (below).
- Most of the compositor's time is its own work and passthrough: 1.4–1.6 ms worn with passthrough off, 2.0–2.6 ms with it on.

**Merged** (host, 2026-10-15): the screen-plane menu pieces are one layer, the **sheet**: a quad in the screen's plane around it, holding the button groups, the tooltip, the Wi-Fi line, the notice and the emulator's messages. `VrMenu` keeps drawing each piece into the atlas canvas; the atlas is uploaded to a GPU image when it changes, and `OverlayLayer::Compose` builds the sheet's image from it (clear, then one blit per piece that shows, scaled into place). A new image is composed only when a piece changes, shows, hides or moves. Its bounds cover every place a piece can be, so they change only with the screen's size (SteamVR applies a quad's new pose a frame after its picture). The image is at most 2048 px on a side: on the default 2.4 m screen the pieces are scaled to 1.26 mm per pixel (0.94 before), about 30 px per degree at 2.2 m, still above the display's density. The beam, the dot and the pad keep their own quads, from a strip along the top of the same image. The panel keeps its own layer and swapchain: it only shows while the game is paused, redraws every frame while it scrolls, and its text stays at full resolution.

| | Before | After |
|---|---|---|
| Menu layers | 9 | 4 (panel, sheet, beam, dot) |
| Layers at most | 16 | 11 |
| Swapchains | 8 | 8 (the atlas's became the sheet's) |
| Compositor GPU, headset session (gunfights, passthrough on) | 2.61 ms (2026-10-14, 16 layers) | 2.64 ms (2026-10-15, at most 11) |

**The layer count costs nothing measurable either**: five layers fewer left the compositor's time where it was. The merge's gain is room under the 16-layer limit, not GPU time. Checked in the headset on 2026-10-15: buttons, tooltips, beam and dot, panels, the Wi-Fi line and the notice look and work as before.

## 6. Options

| Option | Expected gain | Cost and risk | Status |
|---|---|---|---|
| MTVU | The heaviest scenes back at full game speed (measured headless) | A setting; a few games break with it | To check in the headset |
| Core thread on the fast cores | Fewer slow stretches on the little cores | Thread affinity in the fork or the host | Untested |
| Fewer, merged layers | Room under the 16-layer limit; no compositor time (neither count nor transparent area costs any, §5) | | Done: 16 → 11 layers at most |
| Arm two swapchain images ahead | Removes the host's fallback copy when the emulator falls behind | Host change | Open (`docs/performance-stutter.md` §7) |
| Passthrough off | About 1 ms of compositor GPU | The user's choice (the eye button) | Available |
| Lower resolution, foveated rendering | GPU time, which is not short | Sharpness; foveation needs eye tracking and does not suit games that read back their frames | Not now |
| EE cycle rate / skip | Core thread time | Changes the game's timing: risky for a light-gun game | Not now |
