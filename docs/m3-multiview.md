# M3: multiview stereo, and the open wall-corruption bug (status 2026-10-12)

The geometric stereo prototype (`docs/m3-status.md`) drew alternate frames for each eye, so each eye only updated at 30 Hz. The multiview port draws **both eyes in every frame** at 60 Hz. It works and looks right in most of Time Crisis II (title, intro, demo and most scenes).

**One bug is still open.** It is intermittent and only happens with multiview. In the first shooting room, right after the player steps out of cover, the walls disappear and room objects are drawn several times side by side. This document covers:

- what was built;
- how to see and reproduce the bug without a person in the headset;
- everything tested so far, with results.

Repos: `SPD13/armsx2-steam-frame` branch `ps2v` (the GS changes) and `SPD13/ps2-volumetric` (host, tools, this document).

## 1. What was built

**Stereo modes:**

| Command line | What it does |
|---|---|
| `--stereo` | Multiview, the new default. |
| `--stereo --stereo-alternate` | The old alternating-frame prototype. |
| no `--stereo` | Mono; the stereo code paths stay off. |

The multiview mode is selected through `GSVolumetric::StereoSettings::multiview` and fixed when the GS device is created. With multiview on, `GSDevice::m_stereo_layers` is 2.

**GS (fork):**

- **Textures.**
  - `GSTexture`/`GSTextureVK` can have array layers: a 2D_ARRAY main view, lazily created per-layer views, and "layer proxies". A proxy is a `GSTexture` that stands for one layer, shares the parent's image, and forwards layout and fence tracking to it.
  - `GetArrayView()` gives every texture a 2D_ARRAY view, including single-layer ones.
  - Uploads write every layer.
  - Barriers and clears cover all layers.
- **Allocation.** `GSDevice::FetchSurface` gives every render target, depth buffer and feedback target 2 layers. Textures stay single-layer unless they are copies of a two-layer target; the texture-cache copy sites and the RT clone pass the source's layer count.
- **Render passes and pipelines.**
  - TFX render passes are multiview (view mask 0b11, `VIEW_LOCAL` self-dependencies), and in-pass feedback barriers carry `VIEW_LOCAL`.
  - TFX shaders are compiled with `MULTIVIEW=1`. `tfx.glsl` samples every texture as an array (the eye's own layer, or layer 0 of a single-layer texture) and shifts vertices per view: left eye in `VRStereo.xy`, right eye in `VRStereo2.yz`.
  - The colour-clip setup/resolve and DATE prim-ID setup passes use a `MULTIVIEW` variant of `convert.glsl`.
- **Utility passes.** Stretch, multi-stretch, merge, interlace, FXAA, shade boost and DATE setup run once per layer, on proxies. Copies and blits copy every layer. CAS/FSR/SGSR are turned off with multiview.
- **Guard.** `CheckStereoRenderPass` logs any pass whose multiview-ness doesn't match its target's layer count. It never fired in testing.
- **Present.** The left eye is the normal present into the libretro backbuffer, sampling layer 0. `PresentRightEye` blits layer 1 into a second backbuffer using the same present geometry. Both are published through `VKLibretro::Frame::right_image`.
- **Which draws get the parallax shift.** Only draws into the displayed framebuffers (recent `DISPFB` bases) are shifted. The following are never shifted:
  - offscreen render targets. TC2 renders character shadows into a 128×128 target at 0x1a40 and textures 3D scenery from it;
  - draws that texture from a picture already holding per-eye content (`GSTexture::HasStereoContent`, which propagates through copies);
  - flat textured quads with Q exactly 1;
  - full-width untextured fills;
  - HUDs stamped at the Z maximum, as before.

**Host:** each eye's image goes to its own quad layer, so both eyes update every frame.

**Result in the headset (TC2):** both eyes at 60 Hz with smooth motion, and the portal effect is preserved. Performance was about 58 fps, the same as the prototype.

## 2. The bug

**What you see** (in the headset, and in the GS output itself, identically in both eyes):

- the room's walls are missing: gray, the fog/clear colour, shows through;
- tables, chairs, wall lights, bottles and vases appear two or three times side by side;
- sometimes green noise covers part of the frame (uninitialized memory);
- it can last for seconds, or come and go.

![Broken frame, captured live](img/m3-multiview/broken-live.jpg)

The same scene in mono, from the same save state, is correct:

![Mono reference](img/m3-multiview/mono-reference.jpg)

**Where and when:**

- Time Crisis II, first shooting room, right after stepping out of cover (the camera pans).
- It happens in about 1 in 3 runs. A run is either mostly broken or mostly clean.
- It has not been seen in other scenes of the game.

**Only with multiview:**

| Mode | Result |
|---|---|
| mono | always correct |
| `--stereo-alternate` | always correct |
| `--stereo --stereo-depth 0` (multiview, zero parallax) | still breaks |

So it is not the stereo shift.

**How it shows up inside a frame:** with sparse render-target saves in a broken run, the room's tables and chairs are already drawn three times side by side after the first 30 draws, with no walls. Draws are putting geometry in the wrong place.

![Render target after draws 29, 59, 89 of a broken frame; the next trace is correct](img/m3-multiview/broken-partial-draws.jpg)

## 3. Reproducing it without the headset

**Prerequisites:** the build from both repos on the Frame, and the save state `~/.local/share/ps2v/debug/state-room2.p2s` on the Frame (in cover, in the first shooting room). The save state contains game memory and is not in git; to make a new one, see the debug signals below.

**Repro:**

```
tools/mv-debug/stepout.sh <name>            # one run: step out, two present dumps, verdicts
EXTRA="TU_DEBUG=sysmem" tools/mv-debug/stepout.sh <name>   # extra environment for ps2v
```

What the script does:

1. Starts ps2v with `--sdl-pad --load-state …`.
2. Drives the virtual pad (`tools/m0/vpad.py`; **LB is the pedal**) with fresh presses after the load, then holds LB.
3. Takes present dumps after the step-out.
4. Copies the dumps to `captures/mv/<name>/` and runs `tools/mv-debug/classify.swift` on them.

`classify.swift` returns one verdict per dump:

| Verdict | What it means |
|---|---|
| `ok` | pink wallpaper visible |
| `BROKEN` | gray walls |
| `COVER` | the player is still in cover: the blue jacket fills the centre and the walls are white |

**Beware:** an early version of the classifier had no `COVER` verdict and counted cover frames as broken, which misled several test batches. Look at the contact sheet (`sheet-s.jpg`) whenever a result matters. The pedal still sometimes fails to register, so some runs stay in cover.

**Statistics:** about 1 in 3 baseline runs breaks. A candidate fix needs at least 8 runs that actually step out before it can be called a fix. Two or three clean runs mean nothing.

**Debug tools in the code** (all off unless set):

- **ps2v flags:** `--load-state FILE`, and `--sdl-pad`, which makes the SDL pad drive pad 1 alone; the XR controllers are ignored.
- **`SIGUSR1`:** writes a save state to `~/.local/share/ps2v/debug/state.p2s`.
- **`SIGUSR2`:** at the next present, writes:
  - `presentNNN-left/right.png`, the images handed to the host;
  - `-layer0/1.png`, both layers of `m_current`;
  - `-merge0.png`, the merge output.

  It also arms a one-frame draw trace in the log: a `Draw:` line and a `Draw objs:` line (texture object pointers) for every hardware draw.
- **Environment variables.** Paths are on the Frame.

  | Variable | Effect |
  |---|---|
  | `PS2V_TRACE_SAVE=dir` | Saves the render target after every draw of a traced frame. Adds a GPU flush per draw, which changes the outcome. |
  | `PS2V_TRACE_SAVE_EVERY=N` | Saves only every Nth draw instead. |
  | `PS2V_TRACE_SAVE_DS=1` | Saves the depth buffer instead of the render target. |
  | `PS2V_TRACE_SRC=dir` | Saves each source texture of the traced frame once. |
  | `PS2V_FLUSH_FILE=file` | The file holds "first last"; draws in that index range end the pass and wait for the GPU. Re-read every frame, so it can be changed during a run. |
  | `PS2V_ALL_LAYERED`, `PS2V_MV_EAGER_CLEAR`, `PS2V_MV_RT_BARRIER[=same]`, `PS2V_MV_REBIND`, `PS2V_MV_DIRTY=<hex mask, 1 = all>`, `PS2V_BIG_VRING`, `PS2V_RING_SCALE=n` | The experiments in section 4. |

- **Logs in `emulog.txt`, stereo only:**
  - `Stereo draws` every 5 s: draw classification counts;
  - `Stereo pin` and `Stereo mark` examples;
  - `TC read` / `TC move`: texture-cache read-backs and moves, first 200.
- **Vulkan validation:** the layer is copied to `~/vklayers` on the Frame. Run with `VK_LAYER_PATH=/home/steamos/vklayers VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`. `VK_KHRONOS_VALIDATION_DUPLICATE_MESSAGE_LIMIT=100000` makes counts comparable between runs.

## 4. What has been tested

"Runs" counts only runs where the player actually stepped out.

| Hypothesis / change | Result |
|---|---|
| The stereo shift itself (`--stereo-depth 0`) | Still breaks. Not the shift. |
| Pinning rules (target-textured draws, Q = 1, stereo-content marks, display targets only) | Fixed the title ghosting, the far-object flicker and TC2's 3D scenery losing its depth. Did not fix this bug. |
| Draw list differs from mono | No. The same draws, targets, sources and order (diffed traces). |
| Texture cache read-backs or memory moves | None logged in broken runs. |
| Corrupted source textures | No. The wall textures (e.g. wallpaper at 0x34b5) are intact in broken frames. |
| Depth buffer content | Correct in a broken frame (per-draw depth saves). Same object as the fog pass's depth source. |
| ARMSX2 render-pass scheduler | Already off (`CoalesceRenderPasses = false`). |
| SteamVR fragment-density-map (foveation) layer | The validation layer reports FDM images, but the same errors appear in **mono**. Unrelated. `DISABLE_VULKAN_FDM_INJECTION_LAYER` and `VK_LOADER_LAYERS_DISABLE` were reverted. |
| GPU flush after every draw (`PS2V_FLUSH_FILE "0 9999"`) | A broken run stays broken. Not an in-frame race. |
| Per-draw render-target saves | Frames came out right while saving, but they also flush and change timing. Not conclusive. |
| Render-target layout round trip per draw (`PS2V_MV_RT_BARRIER`) | Broken (1 of 4 runs that stepped out). |
| Execute pending clears outside passes (`PS2V_MV_EAGER_CLEAR`) | Broken (green noise). |
| Copy road instead of in-pass self-reads (texture barriers off) | Broken. |
| Turnip sysmem only (`TU_DEBUG=sysmem`) | Broken (1 of 7). |
| Turnip without UBWC (`TU_DEBUG=noubwc`) | Broken (2 of 3). |
| All textures two-layer (`PS2V_ALL_LAYERED`) | Broken (1 of 5). |
| Descriptor views/layouts tracked per slot | Kept in the code (correct anyway). Not the fix. |
| Push all texture slots / re-push at each multiview pass / per draw | Broken. |
| Rebind the vertex buffer per draw (`PS2V_MV_REBIND`) | Removes the validation errors `04007`. Still broken (5 of 8). |
| Rebind all state per draw (`PS2V_MV_DIRTY=1`) | Broke almost every run (15 of 16). Per-group masks were all broken too, even blend constants, so this looks like timing, not a specific binding. |
| Bigger streaming rings (`PS2V_RING_SCALE=8`, `PS2V_BIG_VRING`) | Broken (8 of 8 with `PS2V_MV_DIRTY`). |

**Validation-layer differences, 120 s each, same scene, duplicates unlimited.** Seen only with multiview:

| VUID | Count | Message |
|---|---|---|
| `vkCmdDrawIndexed-None-04007` | ~255k | no vertex buffer bound in this command buffer |
| `vkCmdDrawIndexed-None-08600` | ~17k | descriptor set 0 not bound |
| `vkCmdDrawIndexed-None-08114` | ~8.7k | set 1 binding 0 never updated |

They are spread over all three GS command buffers. The draws that report them are recorded into the same `VkCommandBuffer` handle that received the binds (checked in code). Rebinding the vertex buffer before each draw removes `04007` but not the corruption. Every other VUID appears in mono too, at the same counts.

## 5. Current hypothesis and next steps

The broken frames look like draws using another draw's vertex transform (VS uniforms in set 0) or vertex data. That fits the multiview-only validation errors about state missing from the command buffer, but rebinding that state does not fix it. So either:

- the driver loses or misreads state in multiview passes (Turnip, Adreno 750); or
- something specific to multiview corrupts state in a way rebinding can't repair.

**Next steps:**

1. **GPU capture.** Record a broken frame with the `gfxreconstruct` layer that ships on the Frame (`VkLayer_gfxreconstruct`), then inspect what the misplaced draws read: bound buffers, offsets, uniforms.
2. Compare that capture with a clean multiview frame of the same scene.
3. If it is a driver problem, reduce it to a small Vulkan repro and check newer Mesa.

**Fallback:** while the bug is open, a game can use `--stereo-alternate` (30 Hz per eye, correct). Making that selectable per game in its profile is an easy follow-up.
