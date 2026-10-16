# Full immersive VR mode: feasibility, options, risks

2026-10-15

Volumetric (screen-space stereo) and Volumetric+ (true portal with lean-in) give depth *through a window*. This document asks whether we can go further: drop the window, put the game world around the player, drive the camera from head tracking, and move HUD elements to a separate, comfortable distance. These games were never built for that. The document covers what is possible and a gated path that starts with cheap measurements, not a big build.

**Verdict.**
- A **generic 360° immersive mode is not possible**.
- A **generic, frameless, life-size, head-parallax view of what the game draws** is possible.
- **True surroundings are possible only per game**, through EE-memory camera hooks, and only where the game actually built the geometry.

## The hard constraint: the PS2 has no hardware transform

- The GS only ever receives **screen-space** vertices: x/y in 12.4 fixed point, Z, and STQ. The world→camera→projection maths runs in game code on the EE or in VU1 microcode, and differs per game. Unlike Dolphin VR (GameCube XF matrices), there is **no generic world space and no projection matrix to swap**.
- What the vertex shader does see (`VRStereoPlace`, `external/armsx2/bin/resources/shaders/vulkan/tfx.glsl:36-61`): x, y, Z and Q.
  - For perspective-textured draws Q = 1/w, which is real view depth.
  - So a generic mode can **unproject** each vertex back into the game camera's 3D space once the game's FOV is known. See [fixed-scale-parallax.md](fixed-scale-parallax.md), Option A: the Z–Q fit.
- The result is a **single-viewpoint 3D shell** of what the game drew this frame. It is not a world.

## Does each frame hold enough of the world? (culling audit)

Each frame contains only what survives all of the game's cuts:

| Cut | Where | Effect in VR |
|---|---|---|
| Frustum culling (objects outside the game's FOV are never sent) | EE/VU1, per game | Turning the head beyond the game's FOV shows **nothing**. TC2 at 4:3 covers roughly ±25–30°; the Frame shows about 100°+ per eye, and a full turn is 360°. |
| Guard-band / ADC clip | VU1 microcode | Triangles crossing the frame edge get clipped or dropped, so the edges are ragged. |
| GS scissor + framebuffer size | `GSRendererHW.cpp:11290`, target-sized | Nothing exists beyond 640×448. This is why Volumetric+ shows an empty border when you lean in. |
| Back-face culling (screen-space winding) | VU1 | Leaning or moving sideways reveals holes where back or side faces were never drawn. |
| Occlusion (draw order + Z) | GS | Disocclusion holes behind objects when the eye moves. |
| LOD / streaming tied to the camera | game | Detail is low at the edges, and assets stream relative to the game camera, not the head. |
| Screen-space effects (full-screen fades, blur, bloom, damage flashes, letterbox) | full-screen sprites | Must be pinned. In a frameless world they have nowhere natural to sit. |

**Asset verdict by genre:**
- **Rail shooters (TC2, Ninja Assault):** the worst case. Levels are film sets authored for the rail camera. Building backs, side streets and everything behind the camera often don't exist at all, even with culling disabled.
- **Free-roam 3D games (Jak, Ratchet, GTA, SotC):** have real worlds in RAM within their streaming radius, but only with per-game hooks.

## Options

### A. Immersive theatre (generic, low risk)

Push Volumetric+ to fill the field of view:
- A bigger and closer screen, optionally curved.
- A "world size" scale on D0, already flagged as the next knob for Volumetric+.
- Peripheral fill outside the frame (blurred edge extension / ambient colour) so the border dissolves.
- Head tracking stays the existing parallax and lean-in.

It keeps everything we have: gun, quads, pacing.

### B. Wider game FOV via widescreen patches (generic mechanism, per-game data)

- The core already has PCSX2's patch system and `patches.zip` support (`external/armsx2/pcsx2/Patch.cpp`, `VMManager.cpp:3772`, `EnableWideScreenPatches`). However:
  - The Frame build doesn't embed the zip (`tools/build-frame.sh` sets neither `ARMSX2_EMBED_RESOURCES` nor `ARMSX2_EMBED_PATCHES`).
  - The settings aren't exposed in `host/`.
- **TC2 NTSC-U (SLUS-20219) has a 16:9 patch that is a single float in EE RAM:** `patch=1,EE,003cd7fc,word,3f400000`, which is 0.75, a horizontal projection scale.
  - Smaller values could give ultrawide FOV (32:9 and beyond).
  - That would fill the lean-in border and the periphery with real geometry, as far as the game's culling allows.
- **Risks:**
  - Edge culling pop-in: widescreen patches often don't widen the cull cone.
  - Only some titles have patches. Ninja Assault (SCES-50889) has none.
  - `VRStereoPlace` and the screen quad must learn the non-4:3 aspect.

### C. Frameless projection-layer mode (generic, medium–high effort)

- Replace the screen quads with an OpenXR **projection layer**.
- Unproject the vertices (Q and the known FOV) into a world-anchored game-camera frame, and render each eye with its real XR projection.
- The game's view becomes life-size around you. Outside its frustum you see the ambient fill from A.

**What it needs:**
- A VR-sized render target instead of the 640×448-scaled framebuffer. That means decoupling the target size in the multiview device (`GSDeviceVKVolumetric.cpp`).
- A new gun-aim model: a ray march into the 3D shell instead of a planar quad (`host/gun/GunAim.h`). The TC2 off-screen reload also needs a new meaning.

**Biggest risks:**
- **Motion sickness:** the game camera (TC2's rail cuts, shakes and swings) moves the whole world around you.
- **Head-pose latency:**
  - Today the pose is sampled per GS draw (`GSRendererHWVolumetric.cpp:449`) and presented on a later XR frame. That's fine for a window, not for a world.
  - Head rotation must stay with the compositor. Check whether SteamVR on the Frame supports `XR_KHR_composition_layer_depth`; it isn't requested today (`host/XrApp.cpp:84`).
  - Add comfort vignettes on camera cuts.

### D. Per-game true VR camera hook (high effort per title)

- Read and write the game's camera matrix and FOV in EE RAM: inject head yaw/pitch, disable frustum culling with a code patch, and widen the FOV. This is the PCSX2 free-camera-cheat approach.
- It gives real surroundings where they exist.

**What it needs:**
- A host→core EE memory hook. None exists today: `retro_get_memory_data` returns null, and `host/` has no memory access. A pnach-style per-vsync callback like `Patch::ApplyVsyncPatches` is the natural place.
- Reverse engineering per game.

**Risks:**
- Rotation latency is tied to the game's frame rate (30/60 fps against 72 Hz), so it must be paired with compositor reprojection.
- Disabling culling raises EE/VU/GS load, and EE emulation on ARM may become the bottleneck.
- It doesn't help TC2's missing sets.

### E. Scene accumulation across frames (research)

Cache geometry already seen as the camera moves, and use it to fill the surroundings. It needs a per-frame camera pose (from D), and it ghosts dynamic objects. **Not recommended.**

## HUD at a comfortable distance (applies to A–D)

- **Feasible with existing hooks:** `VolumetricClassifyDraw` (`GSRendererHWVolumetric.cpp:381-426`) already tags pin:hud (flat Z at max), FST and Q=1 sprites per draw.
- **Route:** send those draws to a separate HUD target with alpha, either a third multiview layer or a side target, and show it as its own quad layer at about 1.2–1.5 m with lazy follow.
- **Risks:**
  - HUD draws blend onto the game framebuffer that the texture cache tracks, so a side target is invisible to the cache.
  - Misclassification: TC2 enemy bullets are 2D sprites already pinned to the glass, and there are hit flashes and full-screen fades.
  - Some games draw 3D HUDs.
  - The 16-layer budget is full today (`host/main.cpp:1041-1050`). Option C frees one slot (two quads become one projection layer); A needs one.
- **Fallback:** a pre-HUD diff (`GSVolumetricFrame::prehud`). It is noisy on interlaced lines.

## Recommended path (gated)

### Phase 0: measurement spike

There is no user-facing change. The goal is to answer "is there enough world?" with data:

1. **Vertex-cloud dump.** On SIGUSR2, write each displayed draw's unprojected vertices as a PLY. Each vertex goes from x, y, Z, Q to camera space and is tagged by classification.
   - Run it on the TC2 room 2 state (`debug/state-room2.p2s`) and Ninja Assault's forest state (`debug/state-ninja.p2s`).
   - Inspect the files on the Mac. They show how much 3D a frame really holds, how clean Q-based depth is, and where the cut edges are.
2. **TC2 FOV sweep.**
   - Embed `patches.zip` in the Frame build (`ARMSX2_EMBED_PATCHES`, `tools/build-frame.sh`).
   - Add a debug flag that writes `003cd7fc` with 0.75 / 0.5 / 0.35.
   - Dump frames to see whether geometry at the new edges exists or pops.
   - The patch is NTSC-U only, so this needs a SLUS-20219 disc.
3. **Compositor support.** Log whether SteamVR on the Frame exposes `XR_KHR_composition_layer_depth`. This decides how C and D handle latency.

### Phase 1: Immersive theatre

This is A plus the HUD split, plus B where a patch exists. It gives the best immersion per unit of risk. It becomes a mode next to Volumetric+, in the cube button cycle and as `[Mode]` in VR.ini.

### Phase 2: Frameless mode

This is C. Build it only if Phase 0 shows usable depth and enough edge geometry. Pilot it on TC2 with comfort options.

### Phase 3: Per-game hook

This is D. Do it only if true VR is wanted for specific titles. Pilot it on a free-roam game, not TC2.

## Key files

- Fork (`external/armsx2/`):
  - `bin/resources/shaders/vulkan/tfx.glsl` (`VRStereoPlace`)
  - `pcsx2/GS/Renderers/HW/GSRendererHWVolumetric.cpp` (classification, stereo constants)
  - `pcsx2/GS/Renderers/Vulkan/GSDeviceVKVolumetric.cpp` (multiview targets)
  - `pcsx2/Patch.cpp`
- Host:
  - `host/main.cpp` (layers, eye views)
  - `host/ScreenLayer.cpp`
  - `host/XrApp.cpp` (extensions)
  - `host/gun/GunAim.h`
  - `host/Emu.cpp` (core settings)
  - `tools/build-frame.sh`

## Verifying Phase 0

- **Point clouds:** run both save states headless on the Frame (`~/bench/run.sh`, `--load-state`, SIGUSR2). Copy the PLY dumps to the Mac and inspect them in a point-cloud viewer.
- **FOV sweep:** compare the present dumps at each patched FOV value against the stock 0.75.
- **Depth extension:** confirm that the extension list is printed in `emulog.txt`.
- **Write-up:** record the findings here, then decide between Phase 1 and Phase 2.
