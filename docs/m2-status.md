# M2: depth export (status 2026-10-11)

**Done for Time Crisis II on the Steam Frame:**
- Every presented frame comes with a depth map aligned to the pixel, and a HUD mask.
- Both reach the VR host, where a debug panel beside the screen shows them.
- It holds 60 fps at native and at 3× upscale.

## How it works

### Core: `external/armsx2`, branch `ps2v`

New header `pcsx2/GS/GSVolumetric.h`. Its features are off unless a host turns them on with `GSVolumetric::SetEnabled`.

1. **Pairing (`GSRendererHW::DrawPrims`).**
   - Every 3D draw is recorded per colour target, together with its depth target. A 3D draw here means Z writes on and Z varying across the draw (`!m_vt.m_eq.z`).
   - Flat-Z draws (clears, HUD stamps) never create a pairing.
2. **Snapshots (start of `GSRendererHW::Draw`, before any fast-clear path).**
   - A pairing becomes dirty once 3D depth is written to it.
   - A dirty pairing is copied (DEPTH_COPY to R32F, plus the colour target as RGBA8) in two cases:
     - drawing moves to another colour target;
     - a flat Z-writing ALWAYS draw is about to hit its Z buffer.
   - This captures the frame's depth before the next frame clears a shared Z buffer or the HUD is stamped into it.
3. **Depth merge (`GSRenderer::Merge` → virtual `MergeDepth`).**
   - `GetOutput` remembers the displayed target for each circuit.
   - Right after the colour merge, the displayed target's depth snapshot goes into an R32F canvas, and its colour snapshot into a "pre-HUD" RGBA8 canvas. Both use the same source and destination rectangles as the colour merge.
   - If the target is still being drawn (dirty), the live depth is used and there is no mask.
   - Each canvas is a ring of 3, so the host can still be reading the previous ones.
4. **Publish (`GSDeviceVK::EndPresent`).**
   - `VKLibretro::Frame` gains:
     - `depth_image` / `depth_view`: R32F, GS Z / 2³², larger is nearer;
     - `depth_scale`: 2³² / Zmax for the Z format;
     - `depth_src_uv` and `depth_draw_rect`: how the canvas maps onto the presented image;
     - `prehud_image` / `prehud_view`.
   - All of them are in SHADER_READ_ONLY.
   - `VSync` records `src_uv` and `draw_rect` next to `s_last_draw_rect`.
5. **Small helpers.**
   - `GSTextureCache::FindExactTarget` finds a target without changing the LRU order.
   - `GSVolumetric::SetTrace` logs which colour and depth targets the Z-writing draws use, every 300 vsyncs.

### Host

- **`DepthView`** runs two compute passes on the shared queue, using push descriptors. Shaders are compiled to SPIR-V headers at build time with `glslangValidator`.
  - `depth_stats.comp`: valid range (atomic min/max over float bits, reduced per workgroup), coverage, and the HUD fraction.
  - `depth_view.comp`: a Turbo colour ramp over the valid range, a dark checker for holes, and HUD pixels in white.
  - HUD test: the presented colour (sampled through `draw_rect`) differs from the pre-HUD colour by more than 0.15 per channel.
- **Display:** the debug image goes onto a second quad layer, 1.2 m wide, to the right of the screen and turned toward the viewer.
- **New options:**
  - `--no-depth-panel`
  - `--depth-dump DIR`: depth view and the matching colour crop as PNGs every 10 s
  - `--depth-trace`
  - `--sdl-pad`: lets `tools/m0/vpad.py` drive menus with no one in the headset.
- **Log:** `Depth: coverage N%, HUD N%, range a..b` every 5 s.

## Results (Time Crisis II, Stage 1)
- **First attempt, live depth at vsync:** only the HUD had depth. The 3D scene showed as empty.
  - The trace showed why. Two colour buffers (`0x00000`, `0x008c0`) share one 24-bit Z buffer (`0x01180`).
  - Each frame is about 7.5 flat ALWAYS draws (clear plus HUD), then about 25 3D GEQUAL draws.
  - So by vsync the shared Z already holds the next frame's clear and HUD.
- **With snapshots:**
  - Coverage is 100%.
  - The depth is perfectly aligned with the colour: near wall, agent, receding corridor.
  - Range is about 0.015–0.13 of the Z range, compressed as expected from projective Z. M3 normalises it.
- **HUD mask** (7–9% of the screen) finds the score bar, prompt text, crosshair, ammo, life, timer and the partner marker.
  - There is some noise: thin horizontal streaks and partial letter edges, most likely from deinterlacing and present filtering differences. Clean it up in M3 by dilating/eroding the mask.
- **Performance:**

  | Resolution | FPS | EE | GS | GPU |
  |---|---|---|---|---|
  | Native | 60 | 44% | 11% | 6% |
  | 3× | 60 | 49% | 15% | 14% |

## Open items
- **Test matrix:** only TC2 has been tested. The hard cases still need games: MGS2/3, Jak, God of War, Shadow of the Colossus, Persona, 2D games.
- **Heuristic limits:**
  - Overlays drawn with Z masked after the 3D, with no target switch in between, end up in the snapshot. They get no mask, and if they write Z they also get depth.
  - Effects drawn after the last snapshot count as HUD.
  - Both may need per-game profiles.
- **Back thread:** snapshots assume the GS back thread is off (the default). With it on, `Draw()` and the queued `DrawPrims` run out of order.
- **Still missing:** normalisation, hole filling, temporal smoothing and the volumetric rendering itself (M3).
