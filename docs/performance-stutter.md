# Stutter in heavy scenes, and the blending-accuracy cause (found 2026-10-14)

In Time Crisis II's gunfights the picture stuttered in the headset: the game kept 60 fps, but SteamVR showed old frames again for stretches of several seconds. The cause was the GS **blending accuracy**: at Basic, the hit effects (sparks, impacts) were drawn piece by piece with a barrier between pieces, which overloaded the Frame's GPU. **At Minimum the gunfights run at full rate.** Several other costs were found and cut on the way.

| | |
|---|---|
| **Symptom** | Stutter in busy gunfights, flat and volumetric alike. The emulator stays at 59.9 fps; SteamVR drops the app to **half rate** (36 Hz instead of 72) for 10–30 s at a time. |
| **Cause** | Blending accuracy Basic (`accurate_blending_unit = 1`, the PCSX2 default). Effects that read the framebuffer are split into one draw and one barrier per primitive: 1,000–2,500 of each per frame in a fight. With both eyes drawn in each multiview pass, the GPU was 70–94% busy and SteamVR's compositor missed its slot. |
| **Fix** | Blending accuracy **Minimum**: Settings › Graphics › Blending accuracy, the default for new installs. |
| **Result** | Same fights: barriers 1–10 per frame, emulator GPU 28–42%, 720 frames every 10 s, SteamVR reprojected 8 of 12,789 frames (0.06%, was 21–28%). |
| **Also done** | Passthrough layer only while passthrough is on; the light guns' depth at the PS2's resolution; the GS presents straight into the XR swapchain images (no copies). |

This document covers:

1. how the stutter was measured;
2. the investigation, in order, with what each step ruled in or out;
3. the cause;
4. the changes made, with their measured effect;
5. the debug tools left in the code;
6. lessons;
7. open follow-ups.

Repos: `SPD13/armsx2-steam-frame` branch `ps2v` (GS changes) and `SPD13/arm-sx2-vr-steam-frame` (host, this document).

## 1. Measuring it

Three sources, which together tell where a late frame comes from.

**SteamVR's compositor log**, `~/.local/share/Steam/logs/vrcompositor.txt` (and `vrcompositor.previous.txt`, the session before). When an app disconnects it writes cumulative stats for it:

```
Cumulative stats for pid: 5329
Total..................  75392 presents.    3 dropped. 15920 reprojected
Compositor Time........CPU: 0.561ms / GPU: 1.565ms
Game Info..............FPS Average Target 72  ApplicationTime CPU: 4.208ms / GPU: 5.263ms
```

*Reprojected* counts frames the compositor showed again because the app's was late. This log is the only history across builds: it showed the stutter beginning in the evening of 2026-10-13, before any of the day's merges. Short runs of about 107 s are headless test runs, not play.

**The host's `Pacing:` line**, every 10 s in `emulog.txt`:

```
Pacing: 721 frames, 0 display slots missed (0.0%), worst gap 0, up to 16 layers, game frames copied by the GS 600 / here 0 | 3D volumetric, passthrough on, gun on, Wi-Fi off, controllers on
```

- At 72 Hz a healthy window has 720 frames. **About 360–500 frames with few slots missed means SteamVR throttled the app to half rate:** the predicted display period doubles, so the missed-slot count stays low. This was the key reading.
- It also says which features were on, so one session with toggles compares them.
- "copied by the GS / here": how many game frames the GS put into the swapchain itself versus the host's fallback copy (section 4.3).

**The GS counters**, every 600 presents (10 s at 60 fps):

```
VK stereo: per present 524.1 render passes, 245.4 restarts for VS constants, 247.7 for PS constants
VK stats: per frame 1345 draw calls, 608.7 Mpx pass area, 994 barriers, 13.0 texture copies, 0.2 uploads, 0.0 readbacks, 0.00 blocking GPU waits, 0.2 hash cache misses, 308 pipeline switches
```

The second line is `GSPerfMon`, averaged per frame. Next to these, `PerfLog` (every 30 s) gives the emulator's GPU share, and `Frame: slow` (first 300 only) splits a slow host frame into wait / input / emu / render / end.

## 2. The investigation

The obvious diffs pointed the wrong way several times. Each step, in order:

| Step | Finding | Verdict |
|---|---|---|
| Logs of the first stuttering run | Every slow host frame was spent in `xrWaitFrame` or `xrEndFrame` (12–40 ms), never in the emulator or the host's own work; emulator GPU 80–93% | The compositor misses its slot because the GPU is saturated |
| Passthrough (added 2026-10-14) | Its transparent projection layer was submitted even with passthrough off. Turning passthrough off: 49.8 → 55.9 game fps in the same scene | A real cost (about 1 ms of compositor GPU per frame), fixed (4.1), not the cause |
| Upstream merge (34 commits, 2026-10-14) | A build with the fork just before the merge stuttered too | Ruled out |
| VR code refactor (fork `b2d484938a`) | Every moved logic line has a match in the new files; the right-eye backbuffer code behaves the same | Ruled out |
| SteamVR / SteamOS updates | Both unchanged since 2026-10-07 | Ruled out |
| Headless benchmark, same save state, today's build vs 2026-10-13 `c11ef4e` | Identical GPU: 29–32% guns on, 18–19% guns off | The emulator's cost did not regress |
| HD texture pack (746 MB) | 30% GPU with and without it | Ruled out |
| Light gun depth capture | Guns on 31%, off 18% | A real cost, cut (4.2) |
| Internal resolution 3x → 2x | Headless 31% → 19%; but **in the gunfight the GPU stayed at 91–92% at 2x**, and the picture was softer | Not the cause |
| Multiview pass restarts (the Turnip constant-offset workaround, `docs/m3-multiview.md`) | Light room: 143 passes per frame with restarts, 28 without, GPU within noise. Gunfight replay: 537 passes, 660 Mpx of pass area, GPU about 20–25% either way | Cheap on this GPU, left in place |
| In-session toggles (passthrough, Wi-Fi, gun mode, Flat) | Half-rate stretches with passthrough and Wi-Fi off; compositor GPU equal to yesterday's build | The host's layers are not the cause |
| Host copies queued behind GS work | The host's swapchain copy went onto the shared queue after the GS's next frame | A real cost to pacing, fixed (4.3) |
| Gunfight counters | 1,000–2,500 draw calls and as many barriers per frame, against about 120 and 9 in a calm room | Pointed at the cause |
| Blending accuracy Basic → Minimum | Barriers 1–10, GPU 28–42%, full rate throughout | **The cause** |

Two measurement traps:

- **A headless replay doesn't reproduce a gunfight.** The state saved at the first half-rate moment replayed at about 500 draws and few barriers, even with `--gun-script` firing four shots a second: the effects come from hitting enemies, and scripted shots mostly miss. In-headset counters were needed.
- **Yesterday's build stuttered too in today's fights** (12.4% reprojected). The "regression" was mostly that the player had reached heavier fights.

## 3. The cause

The HW renderer draws some effects by reading the framebuffer in the shader (software blending, and related paths). When the primitives of such a draw overlap, each one must see the previous one's result, so the draw is split into a draw per primitive with a barrier before each: `full_barrier` in `GSDeviceVK` (the draw list loop around the `Split the draw` debug group); `GSRendererHW` sets `require_full_barrier` in many places, the blending one among them.

Which draws need the framebuffer depends on `AccurateBlendingUnit`. At Basic, Time Crisis II's hit effects take that path; in a gunfight they become 1,000–2,500 draws and barriers per frame. On the Adreno 750 (Turnip), every one of those barriers sits inside a tiled render pass, and with multiview the pass covers both eyes. At Minimum the emulator reads the framebuffer only where nothing else can do the blend, and the same fight has 1–10 barriers per frame.

Why it showed as stutter rather than a lower game speed: the emulator and the host share one Vulkan queue (`queueCount = 1` on this GPU). With the GPU busy, the host's frames finished late, and SteamVR's auto-throttling locked the app to half rate until the load dropped.

## 4. Changes

### 4.1 Passthrough layer only while passthrough is on

Host `569b34b`. SteamVR shows the room only behind a projection layer, which covers both eyes' whole view. It was submitted every frame, even with passthrough off, so that a toggle moved no layer. Off, the slot's invisible pad quad now keeps the other layers in place. Saves about 1 ms of compositor GPU per frame with passthrough off.

### 4.2 The light guns' depth at the PS2's resolution

Fork `ae44478b84`. With gun mode on, every vsync also copied the colour with each depth snapshot and filled a pre-HUD and a live-depth canvas, all at the upscaled resolution, for the depth-map window removed on 2026-10-13. These now need `GSVolumetric::SetDepthExtras` (off by default). The snapshot and the depth canvas are taken at the PS2's resolution; the host samples the canvas by UV.

- Gun mode on, same scene: GPU 29–36% → 22–26% (18% with the guns off).
- The guns' depth grid is unchanged: same percentiles, same 338 cells at or nearer than the glass. Very rarely one cell in 19,200 reads "no depth" for a frame (nearest sampling at the lower resolution).

### 4.3 The GS writes each frame into the XR swapchain itself

The host used to copy each published frame into its swapchain, on the shared queue, after the GS had already queued work for its next frame, so the copy finished up to a frame late.

**Present copy** (fork `33e47887f4`, host `630efcd`). The host acquires the next swapchain image ahead and arms it for the GS (`VKLibretro::ArmPresentCopy`, one per eye). The GS records the copy at the end of its present's command buffer, reports the image's id in the published frame (`Frame::copied_left/right`), and the host releases the image when it consumes that frame. `DisarmPresentCopy` waits out a copy in flight before the host writes, releases or destroys an image. If the GS didn't copy (no image armed in time, size change), the host copies into the same armed image as before. All OpenXR calls stay on the host thread.

**Direct present** (fork `29b4dedc6d`, host `4f91fd8`). The copies themselves cost about 1.7 ms of GPU per frame at 3x. The swapchains are now created mutable-format, and when the armed image has the presentation's size the GS draws into it through an RGBA8 UNORM view (the GS writes sRGB-encoded values; the swapchain is `R8G8B8A8_SRGB`):

- left eye: the normal present pass, ImGui overlay included;
- right eye: `StretchRect` from layer 1. A blit into the sRGB image would encode the values a second time, and `StretchRect` leaves its pass open, so it is ended before the overlay's own pass;
- with gun mode on, the left image is left readable: the light sensor samples it through a host view before the host moves it to `COLOR_ATTACHMENT_OPTIMAL` and releases it.

| Same scene, 3x, guns on | Emulator GPU |
|---|---|
| Host copies | 22–26% |
| GS copies at present | 32–36% (the copies now count as GS time) |
| GS draws straight in | 21–22% |

In Flat mode the right eye is no longer put into its swapchain, which Flat doesn't show.

### 4.4 Blending accuracy Minimum

Settings › Graphics › **Blending accuracy** (Minimum, Basic, Medium, High), stored as `PCSX2.ini [EmuCore/GS] accurate_blending_unit` (0–3). New installs get Minimum, written right after `VMManager::SetDefaultSettings` in `Emu.cpp`; existing installs keep their value. Full and Maximum are not offered. In Time Crisis II, Minimum shows no visible difference in the effects (checked in the headset on 2026-10-14).

| Gunfight, 3x, guns on | Basic | Minimum |
|---|---|---|
| Barriers per frame | 1,000–2,400 | 1–10 |
| Draw calls per frame | 1,000–2,500 | 200–420 |
| Emulator GPU | 69–92% | 28–42% |
| Frames per 10 s | 440–500 in the worst stretches | 720 throughout |
| SteamVR reprojected | 21–28% | 0.06% |

## 5. Debug tools left in the code

| Tool | What it does |
|---|---|
| `Pacing:` log line | Every 10 s: frames, display slots missed, layers, frames written by the GS or copied by the host, features on; in gun mode also the light sensor's reading |
| `VK stereo:` / `VK stats:` log lines | Every 600 presents: render passes and restarts per present; draw calls, pass area, barriers, copies, uploads, readbacks, blocking waits, hash cache misses, pipeline switches per frame |
| `debug/state-stutter.p2s` | Saved automatically, once per run, at the first 10 s window under 600 frames (not in a menu); replay with `--load-state` |
| `VOLUMETRIC_MV_RESTART=vs\|none` | Restarts multiview passes only for VS constants, or never (brings back the Turnip corruption where it strikes; for measurements) |
| `PS2V_NO_PRESENT_COPY=1` | The host copies each frame into the swapchain, as before 4.3 |
| `VOLUMETRIC_NO_DIRECT_PRESENT=1` | The GS copies into the armed images instead of drawing into them |
| `Emu::SaveDebugState(name)` | Saves a state to `<data>/debug/<name>` from the host |

**Headless benchmark on the Frame** (not in the repo, in `~/bench`):

```
~/bench/run.sh BIN SECONDS LABEL [ps2v args]
STATE=~/.local/share/ps2v/debug/state-stutter.p2s ~/bench/run.sh ~/devkit-game/ARMSX2/ps2v 70 test
```

It boots Time Crisis II from a save state (`STATE`, default `state-room2.p2s`), stops after `SECONDS`, keeps the log as `~/bench/LABEL.emulog` and prints the `PerfLog` lines. Run-to-run noise is about ±4 points of GPU, and the Frame runs warmer after several runs; compare variants back to back. Headless runs never place the screen, so there's no stereo shift, and the compositor is idle.

## 6. Lessons

- **Read the frame count, not only the missed slots.** SteamVR's half-rate mode keeps the missed-slot count low; 360–500 frames per 10 s at 72 Hz is the tell.
- **SteamVR's compositor log is the only history across builds.** It dated the problem and ruled out the day's merges.
- **Measure in the scene that stutters.** The restarts looked expensive on paper (up to 520 passes per frame) and cost nothing; the barriers didn't show at all outside a real gunfight.
- **A headless replay without the right input is a different scene.** Effects that depend on hits need the player.
- **Lowering resolution is a test, not just a fix.** When 2x left the GPU at 92%, the cost was per draw, not per pixel.
- **Rebuilding on the Frame after checking out another commit there:** the re-synced files keep older timestamps, ninja skips them, and the link fails with duplicate symbols. Touch the changed files before building.

## 7. Open follow-ups

- **Other games** may need Basic or higher for correct blending; the setting is global today. If an effect looks wrong at Minimum, fixing that effect is better than raising the level for everything.
- **60 fps on a 72 Hz display** shows every fifth game frame twice: a small steady judder in fast motion, separate from this stutter.
- **The host's fallback copy** still happens when the host arms an image late (it did in the half-rate stretches, feeding the load). Arming two images ahead would remove it.
- **Multiview pass restarts** stay: measured cheap, and removing them brings back the wall corruption. Push constants for the VS constants remain the untested alternative.
