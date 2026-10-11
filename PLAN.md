# PS2 Volumetric: a standalone PS2 emulator for Steam Frame with a volumetric window and VR light guns

## Context
We want a native, standalone PS2 emulator for Valve's Steam Frame. It has two VR-specific features:
1. **Volumetric window.** The game is shown on a virtual screen with real 3D parallax, rebuilt from the PS2 depth buffer.
2. **VR light gun.** VR controllers act as GunCon 2 light guns, with a 3D gun model, a beam to the screen, and a hit dot.

The project directory is currently empty. The plan rests on web and source research done in this session.

**Decisions already made by the user:**
- Base: ARMSX2, so the project is GPLv3.
- Volumetric method: depth-map reprojection.
- App shape: a custom native OpenXR app with the emulator built in.
- Hardware: a Steam Frame devkit is available.

## Research findings (summary)

**Platform**
- Steam Frame hardware: Snapdragon 8 Gen 3 (Adreno 750), 16 GB RAM, SteamOS 3 on ARM64.
- Graphics and XR: Mesa Turnip Vulkan 1.4, and SteamVR's native ARM64 OpenXR runtime.
- Build and deploy: build natively for Linux ARM64 inside Valve's Steam Runtime 4 ARM64 SDK container, and deploy with the SteamOS Devkit Client.
- Controllers: `XR_VALVE_frame_controller_interaction`.
- Performance:
  - SteamOS caps clocks by default, so test in performance mode.
  - "Verified" status needs 90 fps.
  - Avoid FEX (x86 translation) and Lepton (Android). Neither fits a JIT emulator.
- Templates to copy from: AstroQuest-Frame (Vulkan, Frame controller profile, SR4 container) and OpenMW-VR-steam-frame.

**Emulator base: ARMSX2** (github.com/ARMSX2/ARMSX2, GPLv3, PCSX2 fork)
- It is the only open-source PS2 emulator with full ARM64 recompilers (EE, VU0/1 microVU, IOP, with fastmem).
- It already ships Linux arm64 nightly builds (4K and 16K page sizes), plus a libretro core and an SDL build.
- It is very active and inherits PCSX2's compatibility, Vulkan GS and GunCon 2 emulation.
- Alternatives ruled out:
  - Upstream PCSX2: no ARM64 JIT.
  - LRPS2: x86 only.
  - AetherSX2/NetherSX2: closed source, legally unusable.
  - Play!: about 60% compatibility, no GunCon 2.

**Depth: feasible for an estimated 70–80% of 3D games, but only by changing the emulator.**
- How the PS2 handles depth:
  - The GS only supports depth tests GEQUAL and GREATER, so larger Z means nearer.
  - Z is already projected and usually affine in 1/w, which makes it proportional to disparity — exactly what parallax needs.
- How PCSX2 stores depth:
  - Depth targets live in `GSTextureCache` (`m_dst[DepthStencil]`) as `VK_FORMAT_D32_SFLOAT_S8_UINT`.
  - The stored value is GS integer Z divided by 2³² (`tfx.glsl`: `gl_Position.z = min(a_z, MaxDepth) * exp2(-32)`).
  - Nothing links the displayed color target to a depth target, so we must track that pairing ourselves.
- Hard cases:
  - Z is cleared or reused before vsync.
  - Post-processing copies the image between render targets.
  - Depth-of-field effects reinterpret depth as color data (MGS2/3, Jak, God of War).
  - Interlaced field buffers.
  - HUD sprites.
  - Games with 16-bit Z.
- External tools don't work for this: ReShade, Depth3D and 3DMigoto are unreliable with PS2 output.
- Prior art: PenguinScreen2, an x86 PCSX2 VR fork that streams to a headset (GPL code; its per-game profiles are non-commercial). Use it as a reference only (`pcsx2/VR/VRVulkanBridge.cpp`, `DepthHistogram.cpp`, `StereoState.h`).

**Light gun: largely solved upstream.**
- Code: `pcsx2/USB/usb-lightgun/guncon2.cpp`.
- It already has a calibration table for 30 games (scale, center and screen size per game, keyed by serial).
- Aim input: `InputManager::UpdatePointerAbsolutePosition(0, x, y)`, in window pixels. A point outside the display rect reads as off-screen and triggers a reload.
- Button input: `USB::SetDeviceBindValue(port, idx, v)`:

  | idx | Button |
  |---|---|
  | 1 / 2 / 3 | C / B / A |
  | 4–7 | D-pad |
  | 13 | Trigger |
  | 14 | Select |
  | 15 | Start |
  | 16 | ShootOffscreen |
  | 17 | Recalibrate |

- ARMSX2's Android build already wires this up for touch aiming (`platforms/android/app/src/main/cpp/native-lib.cpp`, around line 2673, `usbLightgunAim` / `usbLightgunButton`). That code is our template.
- Limit: `MAX_POINTER_DEVICES = 1`, so two guns at once needs a small patch.
- Feedback: the GunCon 2 has no recoil signal, so we synthesize haptics when the trigger is pulled.
- Gun model: make an original "retro light gun" in glTF and avoid GunCon trademarks and trade dress. Use the CC0 Kenney Blaster Kit as a placeholder.

## Architecture

```
ps2-volumetric/                      (this repo, GPLv3)
  external/armsx2/                   git submodule → our fork of ARMSX2
  host/
    main.cpp                         app lifecycle, settings, game library
    xr/       XrSession.*            OpenXR instance/session, XR_KHR_vulkan_enable2, Frame controller profile, haptics
    gfx/      VkContext.*            single VkInstance/VkDevice shared with ARMSX2's GSDeviceVK
    emu/      EmuBridge.*            boot/pause/save-state, receives a VolumetricFrame each vsync, audio
    volume/   DepthNormalize.comp    percentile range fit, temporal smoothing, edge-aware hole fill
              VolumetricWindow.*     displaced grid mesh (later: per-eye relief ray-march)
    gun/      LightGun.*             ray → heightfield hit → GunCon2 aim + buttons, beam + dot, haptics
              GunModel.*             glTF loader (cgltf), muzzle node, grip alignment
    ui/       Panel.*                PCSX2 FullscreenUI (ImGui) drawn to an offscreen texture on a VR panel, laser-pointer input
  profiles/   *.yaml                 per-game depth overrides (Z mode, HUD rules, capture pass, depth scale)
  assets/     lightgun.glb
  tools/      build-sr4-arm64.sh, deploy.sh
```

**Threading and timing.** The XR loop runs at 90 Hz and always draws the latest `VolumetricFrame` using the current head pose. Parallax therefore stays smooth even when a game runs at 30 or 60 fps. The emulator threads (EE, MTGS) keep their own timing.

## Changes to our ARMSX2 fork (`external/armsx2`)
1. **Embedding hook.**
   - Add a host-provided Vulkan device path to `GSDeviceVK`, so it uses our device and queues instead of creating its own (`pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp`).
   - Use the existing libretro/SDL frontend target as the cut point for a static library build.
2. **Depth capture** (new `pcsx2/GS/Renderers/HW/GSDepthCapture.{h,cpp}`):
   - In `GSRendererHW::Draw()` (about line 3752, where the depth target is looked up), record which color target is being drawn with which depth target. Only count draws where `EmulateZbuffer` shows Z testing or writing is on.
   - Take a depth snapshot whenever the color target or Z buffer changes, a Z clear is detected, or vsync arrives. Copy it into an R32F image, converting `d·2³²/Zmax` in the same pass with a new `ShaderConvert` entry (template: the `GSConfig.SaveDepth` dump path, about line 5206).
   - Follow the snapshot through full-screen copies via `Source::m_from_target`, so post-processed and downsampled outputs keep their depth.
   - HUD mask (R8): flag draws as HUD when `GSVertexTrace` shows sprite/flat-Z primitives, ZTE=ALWAYS or ZMSK, drawn after the last 3D draw that wrote Z.
   - Add `MergeDepth`, mirroring the rectangles, offsets and interlacing of `GSRenderer::Merge()` (`GSRenderer.cpp` about line 82), so depth lines up pixel-for-pixel with the output from `GetOutput()`.
   - New API: `GSGetVolumetricFrame()` → {color VkImage, depth R32F, hudMask R8, Z format, quality score, timeline semaphore value}.
3. **Light gun:**
   - Add a per-port pointer index, lifting `MAX_POINTER_DEVICES` so two guns can work (`pcsx2/Input/InputManager.*`, `guncon2.cpp`).
   - Allow PCSX2's own crosshair cursor to be turned off.

Keep the patches small and isolated so we can keep merging upstream ARMSX2 changes.

## Volumetric rendering (host)
1. **Normalize (compute shader).**
   - Build a histogram of valid pixels, excluding clear values, the HUD and unwritten pixels.
   - Remap the 2nd to 98th percentile range to 0..1 and smooth it over time.
   - Fill holes with an edge-aware filter guided by color (this also hides 16-bit Z banding).
   - Unwritten or sky pixels go to the far plane.
   - Optionally fit Z ≈ a·Q + b from per-draw vertex statistics to tell projective Z from linear Z.
2. **Draw the window.** Start with a displaced grid mesh at about 320×224 vertices.
   - Depth is pushed *behind* the screen plane by `depthScale`, giving a "window into a box" effect. An optional pop-out amount brings it forward.
   - Discard or stretch triangles across large depth jumps, using a fixed screen-plane color to avoid smearing.
   - HUD pixels are pinned to the screen plane.
   - Later, if performance allows, upgrade to per-eye relief ray-marching for cleaner edges.
3. **Fallback.** If a frame's quality score is low, fade to a flat screen. A per-game YAML profile can override capture behavior. Neural depth estimation is out of scope for now: the NPU isn't available on SteamOS, and the GPU budget is too tight.
4. **Comfort settings:** depth scale, screen distance, size, curvature, and recentering.

## Light gun (host)
1. **Aim.** Use the OpenXR `aim` pose of each Frame controller.
   - Cast the ray against the **depth-displaced surface**, not the flat screen plane, so the shot lands on what the player actually sees.
   - Do this as a GPU compute ray-march on the current depth map, writing the hit UV to a small buffer.
   - Read that buffer back one frame later, or fall back to a CPU march over a downsampled depth copy.
2. **Feed the emulator.**
   - Convert UV to the emulator's virtual window pixels and call `UpdatePointerAbsolutePosition` (one pointer per port after the patch).
   - A miss sends (-1,-1). Pulling the trigger while off-screen is then the game's own reload.
3. **Button mapping.**

   | Controller input | GunCon 2 |
   |---|---|
   | Trigger | Trigger (13) |
   | A / B face buttons | A (3) / B (2) |
   | Grip | C (1) |
   | Thumbstick | D-pad (4–7) |
   | Menu | Start (15) |
   | Long-press menu | Select (14) |

   Explicit reload (16) and Recalibrate (17) are available in the panel UI.
4. **Visuals and feedback.**
   - The `lightgun.glb` gun is aligned to the grip pose. Its "muzzle" node is where the beam starts.
   - The beam is a thin additive cylinder from the muzzle to the hit point, ending in an emissive dot on the surface.
   - The beam and dot hide when the gun points off-screen.
   - Each trigger pull gets a short `xrApplyHapticFeedback` pulse as synthetic recoil.
5. **Light gun game mode.** When a game's serial is in the GunCon 2 table, automatically set USB port 1 (and port 2 when two guns are enabled) to `guncon2` and show the guns.

## Milestones
- **M0, feasibility spike (go/no-go): DONE 2026-10-11, GO.** Time Crisis II runs at 60 fps / 100% speed natively on the Frame at native and 3× upscale. Details in `docs/m0-results.md`. Follow-ups:
  - Make CD/DVD detection robust for rebuilt DVD images.
  - Test more demanding games.
- **M1, native host skeleton: DONE 2026-10-11.** TC2 runs in our OpenXR app on the Frame on a flat quad layer, at 60 fps, with audio and native Frame controls. Details in `docs/m1-status.md`.
  - The device is shared through ARMSX2's existing `VKLibretro` hook rather than a new GSDeviceVK patch.
- **M2, depth export: DONE for TC2 (2026-10-11).** Depth snapshots per colour target, a pre-HUD colour mask, publishing through `VKLibretro::Frame`, and a debug panel in VR. Details in `docs/m2-status.md`.
  - The test matrix across the hard-case games is still open; it needs more game dumps.
- **M3, volumetric window.** Normalization, the displaced mesh, the HUD layer, per-game profiles, and comfort settings.
- **M4, light gun.** Aim/button bridge, ray-heightfield hit, beam and dot, gun model, haptics, two-gun patch.
- **M5, polish and ship.**
  - FullscreenUI on a VR panel: game list, BIOS onboarding (the user supplies their own BIOS dump), settings.
  - Foveation (`XR_META_foveation_eye_tracked`).
  - Steam packaging with a GPL source offer.

## Verification
- **Performance:** per-frame timing from the SteamVR overlay. XR must hold 90 Hz and the emulator must run at 100% speed. Run each test in performance mode.
- **Depth correctness:**
  - Side-by-side debug panel showing color, raw depth, normalized depth and the HUD mask.
  - Automated check that depth and color edges line up (compare their gradients).
  - Test matrix: GT4, Ratchet & Clank, Jak, MGS2/3, Shadow of the Colossus, God of War, Persona 3/4, Okami, plus a 2D game.
  - Each game is logged as pass, needs profile, or flat fallback.
- **Volumetric feel:** move your head sideways and check that near objects shift more than far ones. The HUD should stay on the screen plane, with no smearing at depth edges.
- **Light gun:** test Time Crisis 2 (SLUS-20219), Time Crisis 3, Vampire Night, Ninja Assault and Virtua Cop EE (SLES-51229).
  - Run the in-game calibration and confirm shots land under the dot across the screen, including the corners.
  - Check that reloading by firing off-screen works and the haptic pulse fires.
  - Check two-player co-op with one gun per hand.
- **Regression:** ARMSX2's own unit tests and GS dump replay (`gsrunner`) still pass after our patches.
