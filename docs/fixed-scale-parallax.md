# Fixed-scale parallax for the volumetric window

2026-10-13

## Summary

The goal is a fixed scale: the same game distance always gives the same offset behind the window. Most 3D PS2 games can get this generically, using the per-vertex Q value (k/w) that the GS already receives for perspective-correct texturing. One constant per game is still unknown: the game's unit of distance.

Recommendation: prototype Option A (fit Z against Q, convert in `depth_normalize.comp`). Store the per-game scale in the profile, with a frozen auto-calibration as the default. Keep today's adaptive normalisation as the fallback for frames with no usable Q. Move to Option B only if games with mixed projections turn out to be common.

## Current implementation

The physical depth has a fixed maximum, but the game depth is rescaled every frame to fill it, so depth today is relative, not absolute.

**Physical side (fixed).** `VrSettings::depth_range` (`host/Settings.h`, default 0.3 m, range 0–5 m) is how far behind the screen plane the farthest point sits. `pop_out` (default 0, range 0–1) moves the whole volume forward by that share of the range. `window.vert` places each grid vertex at:

```math
p = \text{origin} + \dots + \text{back} \cdot \left((1 - n) - \text{pop\_out}\right), \quad |\text{back}| = \text{depth\_range}
```

**Game side (adaptive).** `RawNormalizedDepth` in `shaders/depth_common.glsl` maps the GS Z (R32F canvas, GS Z / 2^32 × `depth_scale`) linearly onto n:

```math
n = \operatorname{clamp}\left(\frac{z - \text{lo}}{\text{hi} - \text{lo}},\ 0,\ 1\right)
```

- `depth_range.comp` takes `lo` and `hi` as the 2nd and 98th percentiles of a 256-bin histogram of the frame's Z.
- The range widens fast (20% a frame) and narrows slowly (0.05% a frame, about 30 s at 60 fps).
- After 30 frames with no depth, it resets (`DepthView.cpp`).
- Z at or above 0.9999 of the format's maximum is treated as HUD and drawn on the screen plane. Cells with no depth go to the far end (n = 0).

**Consequences.**

- A tight corridor and an open vista both fill the same 0.3 m once the range settles.
- The mapping is linear in GS Z, which most games write as roughly 1/w. Spacing between near objects and between far objects therefore differs from the real geometry.
- The outer 2% at each end is clamped to the screen plane or the back.
- The same object changes depth whenever the range adapts.

## Why GS Z is not enough

The GS has no projection matrix, so its Z value doesn't carry a distance on its own. Each game's VU microcode transforms and divides its vertices, then writes whatever Z it chooses. Typically that is:

```math
Z = A + \frac{B}{w}
```

Here w is the view-space distance. A and B depend on the game's near and far planes and on how it scales Z into the 24- or 32-bit format. They vary between games, and sometimes between scenes or passes within one game. Some games write a Z that is linear in w instead. Without A and B, Z ranks surfaces correctly but can't give a distance. That is why the current pipeline falls back to percentiles.

## The generic signal: Q

Every perspective-correct textured primitive sends the GS a per-vertex Q that equals k/w. That gives a distance in game units, without knowing anything else about the game.

The GS samples textures at (S/Q, T/Q). For textures to look right in perspective, the game must therefore send Q proportional to 1/w, with S and T premultiplied by it:

```math
Q = \frac{k}{w} \quad\Rightarrow\quad w = \frac{k}{Q}
```

- **1/Q is linear view distance, up to one constant k.** The constant k is the game's own choice and is normally the same for the whole game.
- **Q is affine in screen space.** The GS rasteriser interpolates it linearly across a triangle, which is exact for 1/w. A per-pixel Q is therefore correct, not an approximation.
- **A projection change doesn't break it.** A zoom or field-of-view change alters A and B in the Z formula, but not the relation between Q and distance.
- **It is only present on textured draws that use STQ.** Draws with FST set (UV coordinates, typical for 2D sprites and the HUD) and untextured draws carry no usable Q. Some games also send Q = 1 everywhere (affine texturing).

## Option A: fit Z against Q, convert in the depth pass

This option learns the game's Z formula from the vertices it sends, then turns every Z in the existing depth canvas into a distance. It needs no new render target.

1. **Sample.** In the GS HW renderer's draw path, take a few vertices from each draw that is textured, uses STQ (FST off) and writes Z (ZMSK off). Only take draws into the depth buffer that the depth snapshot captures. Record each vertex's (Z, Q) in the same units as the depth canvas.
2. **Fit.** Once per frame, fit two models robustly (RANSAC or a median of pairwise slopes) and keep the one with the lower residual:
   - 1/w Z: Z = A + B·Q
   - linear Z: Z = A + B/Q
3. **Smooth.** Ease A and B over a few frames, and reset them on a projection jump (a large change in residual).
4. **Convert.** Pass A, B and the model to `depth_normalize.comp` as push constants. Recover Q per cell, then distance d = 1/Q in units of k:

   ```math
   Q = \frac{Z - A}{B} \quad\text{(1/w model)}, \qquad d = \frac{1}{Q}
   ```

5. **Fall back.** With too few samples, Q constant, or a residual above a threshold, keep today's percentile normalisation for that frame.

**Strengths:** small change (CPU sampling in the GS plus a few lines of shader), no extra bandwidth, and it reuses the snapshot-per-colour-target capture. The fit also covers untextured pixels drawn with the same projection.

**Weaknesses:** it assumes one projection per depth buffer. A sky, a weapon view model or a split Z range drawn with different A and B shows up as outliers and is converted wrongly. The fit residual must be logged to find out how often that happens.

## Option B: write 1/Q into an extra GS attachment

This option measures distance per pixel instead of fitting it, so each draw's own projection is handled exactly.

1. Add an R32F colour attachment to the GS HW renderer's 3D draws, alongside the depth buffer the snapshot captures.
2. In the tfx fragment shader, write the interpolated 1/Q wherever the draw writes Z and uses STQ. Write a "no distance" marker for FST and untextured draws, or leave the value unchanged so the surface behind keeps its distance.
3. Capture this attachment together with the depth snapshot, and read it in `depth_normalize.comp` in place of Z.
4. For pixels marked "no distance", fall back to Option A's fit or to the percentile normalisation.

**Strengths:** exact per pixel. Mixed projections (sky, view model, split Z ranges) need no special handling, and there is no model to choose.

**Weaknesses:** it changes the ARMSX2 tfx shaders and render-target management, which is a larger and more fragile fork change. It adds a full-screen R32F write per 3D draw, which costs bandwidth on the Frame's GPU. It also has to survive the GS's target reuse and copies, the same issues the depth snapshot already had to solve.

## The per-game scale

Both options give distance in units of k, so one number per game has to be fixed to get a constant scale. Two ways to set it:

| Approach | How it works | Pros | Cons |
| --- | --- | --- | --- |
| Profile value | `profiles/<serial>.ini` stores d0, the game distance that sits on the screen plane (or metres per game unit) | Exact, repeatable, can be tuned per game | Has to be set by hand for each game |
| Frozen auto-calibration | Run the current adaptive range for the first minutes of play, take the long-term median distance as d0, freeze it and write it to the profile | Works for any game with no setup | The value depends on where the player was during calibration; may need a reset option in the menu |

The recommended default is auto-calibration, saved to the profile, with the value adjustable in the settings menu. Since k is normally the same for the whole game, one value should hold across levels. Calibrating d0 separately on several levels and comparing the values would confirm this.

## Mapping distance to window offset

With a fixed scale, a mapping from distance d to the offset behind the screen plane still has to be chosen. Both options below are fixed, so the same d always gives the same offset.

**True portal (orthostereo).** The offset grows linearly with distance:

```math
\text{offset} = (d - d_0) \cdot s
```

This is geometrically correct when s is metres per game unit. Distant scenery ends up metres or kilometres behind the screen, though, and the window becomes a real hole into the world. It would need a far clamp, which brings back a maximum.

**Bounded, linear in disparity (recommended).** Infinity lands at `depth_range`:

```math
\text{offset} = \text{depth\_range} \cdot \left(1 - \frac{d_0}{d}\right)
```

- Binocular disparity varies as 1/d, so equal steps in 1/d look like equal steps in depth. This mapping keeps the eye's ordering and spacing of disparities.
- It needs no far clamp, and it fits the existing `depth_range` setting.
- In the shader it is just `n = d0 / d = d0 · Q`, so it slots into `window.vert` unchanged.

**Pop-out.** Anything nearer than d0 gives n > 1 and comes out in front of the screen. Clamp n to 1 + `pop_out` so that close objects (walls you lean on, the player's own weapon) don't fly into the viewer's face.

## Limits and fallbacks

The Q signal fails in a known set of cases. Each needs either detection with a fallback or the existing special handling.

| Case | Why it fails | Detection | Fallback |
| --- | --- | --- | --- |
| 2D games, sprites, FST draws | No Q; texture coordinates are UV | No STQ samples in the frame | Percentile normalisation (today's path) |
| Affine texturing (Q = 1) | Q carries no distance | All sampled Q equal | Percentile normalisation |
| Several projections in one depth buffer (sky, view model, split Z) | One fit can't cover them (Option A only) | High fit residual, outlier clusters | Option B, or fit per Z range |
| Linear-Z games | Z = A + B·Q model doesn't fit | Linear model fits better | Use Z = A + B/Q |
| Sky drawn without Z writes | Nothing in the depth canvas | n = 0 cells | Far end, as today |
| HUD at maximum Z | Not scene depth | Z ≥ 0.9999 | Flat on the screen plane, as today |
| Very near surfaces | d → 0 makes n large | n above the pop-out limit | Clamp n |

## Comparison and plan

Option A gives fixed-scale depth for the least work and leaves Option B as an upgrade path, so it is the one to build first.

|  | Current (adaptive) | Option A: Z–Q fit | Option B: 1/Q attachment |
| --- | --- | --- | --- |
| Same distance, same offset | No | Yes, within one projection | Yes |
| Spacing matches geometry | No (linear in Z) | Yes | Yes |
| Mixed projections | Not applicable | Wrong for outliers | Exact |
| Changes to ARMSX2 GS | None | CPU vertex sampling in the draw path | tfx shader + extra render target |
| GPU cost | Today's | About the same | One extra R32F write per 3D draw |
| Per-game setup | None | d0 (auto-calibrated) | d0 (auto-calibrated) |
| 2D / affine games | Works | Falls back to adaptive | Falls back to adaptive |

**Plan**

- [ ] Log (Z, Q) samples and both fit residuals on TC2, without changing the rendering. This confirms whether Z follows 1/w and whether one projection covers the depth buffer.
- [ ] Add A, B and the model to the depth pass push constants, and compute n = d0·Q behind a remote test flag, next to the existing path.
- [ ] Add d0 to the game profile and the auto-calibration with a reset in the settings menu.
- [ ] Compare adaptive and fixed scale on the Frame in TC2: stepping out of cover, corridors versus open areas.
- [ ] Survey a few more games for constant Q, linear Z and mixed projections before deciding on Option B.

## Volumetric+: the true portal in the multiview path

2026-10-14

The depth-map window this document starts from was removed on 2026-10-13. Volumetric is now the multiview geometric stereo: the GS draws every displayed draw once per eye and shifts each vertex by its Z (`GSVolumetric::StereoSettings`, `VRStereoPlace` in `tfx.glsl`). Volumetric+ is a third mode that applies this document's true portal there. It is a test mode, next to Volumetric, and can be removed.

**What Volumetric does.** For an eye at offset e from the screen centre (NDC), D in front of the screen, with u = Znorm / convergence:

```math
p' = p + e \cdot \frac{R}{D + R} \cdot (1 - u)
```

Most games write Z as roughly 1/w, so u ≈ d0 / d, and 1 − u is the doc's "bounded, linear in disparity" mapping already. Two things keep it from being a true portal:

- **Compressed depth.** The factor R / (D + R) (0.58 with the defaults R = 3 m, D = 2.2 m) scales every disparity down, so infinity sits R behind the glass and the whole scene is shallower than its picture says.
- **Shift only.** The picture moves sideways with the eye but never changes size. A real window shows more of the world when you lean in, because far scenery shrinks against the frame while scenery at the glass stays put. Volumetric has no such term, so leaning in or out feels like looking at a moving picture, not through a hole.

**What Volumetric+ does.** It takes the game's camera to be where the viewer stood at the screen distance D0 (`screen_distance`). Then the screen-plane depth sits on the glass, and the geometry the picture shows at u lies D0 (1/u − 1) behind it, out to infinity at u = 0: a fixed size in the room, with no depth_range. Projecting that point onto the glass from the eye's real position, D = r · D0 away, gives:

```math
p' = \frac{r \cdot p + e \cdot (1 - u)}{1 + u \cdot (r - 1)}
```

- At D = D0 (r = 1) this is Volumetric's shift with a depth factor of 1: full disparity, far scenery at infinity. Each eye's far shift equals its own offset, so the eyes never diverge.
- At the glass (u = 1), p' = p whatever the eye does.
- Leaning in (r < 1), far scenery (u → 0) shrinks to r · p: the window reveals more around its edges. Leaning back enlarges it.
- The host passes r per eye; it rides in the sign of the stereo constants' w, so the constant buffer layout doesn't change. The light gun's beam march uses the same placement.

**What to judge in the headset** (TC2, default screen plane 0.12):

- Does the full depth look right, or too deep? Infinity at infinity is what makes it a portal, but stereo-only depth on a 2.4 m screen may feel exaggerated. If so, a scale on D0 (a "world size" slider) is the knob to add.
- Leaning in: the reveal at the edges is real geometry, but the game never drew outside its frame, so far scenery leaves an empty border at the window's edges (about (1 − r)/2 of the width on each side). Whether that border spoils it decides whether the dolly term stays.
- The scale is taken about the render target's centre, which is the displayed picture's centre only when the game displays its whole target.

**Known limits.** It inherits the Z ≈ 1/w assumption: a game whose Z has a large offset A (Z = A + B/w) or is linear in w gets the wrong depths, more visibly than in Volumetric because nothing compresses them. The Z–Q fit (Option A above) is the fix for that, and it would also replace the screen-plane setting with a measured d0.
