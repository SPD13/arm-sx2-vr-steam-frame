# M3: volumetric window (status 2026-10-12)

Two approaches were built and tested in the headset on Time Crisis II. **Geometric stereo won**, and is now the direction. The depth-map window stays in the code as the default mode for comparison.

## 1. Depth-map reprojection (`VolumetricWindow`, default mode)

**How it works**
- The host draws the game picture on a grid mesh over the virtual screen. Each vertex is pushed back by the normalised depth from M2.
- The depth range is the 2nd to 98th percentile. It widens fast and narrows over about 30 s.
- Rendering is per eye, into an OpenXR projection layer.
- Optional pieces:
  - edge-fill background layer (`--edge-fill`);
  - flat HUD overlay, using live depth at the maximum, steady for about 2 s, plus per-game HUD rectangles from `profiles/<serial>.ini`;
  - fixed-camera eye inspection panels (`--eye-panels`) and eye captures (`--eye-dump`).

**Verdict**
- Each eye's image is clean (confirmed by captures).
- Single-image-plus-depth has limits that never went away:
  - Smoke, glows and flashes have no depth, so they get painted onto the surfaces behind them.
  - Gaps behind near objects get stretched or guessed, which shows up as ghosting in stereo.
  - Telling the HUD apart from effects needs per-game heuristics. HUD elements popped between the screen plane and the scene.

## 2. Geometric stereo, frame-alternating prototype (`--stereo`)

**How it works**
- The GS renders alternate frames for each eye.
- Every vertex is shifted on screen by that eye's offset from the screen centre times how far behind the screen its geometry is. The tracked eye position comes from the host every frame, through `GSVolumetric::SetEyeView`.
- This gives stereo **and** head-tracked window parallax from real geometry: nothing is reconstructed.
- No shift is applied to:
  - screen-space (FST) draws;
  - HUDs stamped at maximum depth.
- Each frame is tagged with the eye its displayed buffer was drawn for. Double-buffered games show the previous frame's buffer, and tagging with the current eye reversed the stereo.
- If one eye gets no frames for 150 ms (30 fps movies), both eyes are shown the other eye's frames.

**Result in the headset (TC2, `--stereo --stereo-z --stereo-conv 0.12 --stereo-depth 3`)**
- The portal effect works.
- The world sits inside the window, with the HUD on the glass.
- The depth strength feels right.

**Mapping notes**
- Q-driven mapping with convergence 1 left most of TC2's scene (Q 0.13–5) unshifted.
- Z-driven mapping with the convergence set at the scene's nearest normalised Z (0.12) spreads the parallax over the whole scene.

**Open items**
- **Motion:** each eye only updates at 30 Hz. Next is the full multiview port, so both eyes render every frame: two-layer targets, `VK_KHR_multiview` TFX pipelines and per-layer copies/merge, following PenguinScreen2. Their analysis is in the M3 session notes.
- **Enemy bullets:** they look off. They are probably 2D sprites (FST or at maximum depth) pinned to the glass. Trace how they're drawn and place them at the depth around them.
- **Per-game parameters:** convergence and depth should come from the game profile.
- **Light gun (M4):** aiming has to go through the same per-eye shift.
