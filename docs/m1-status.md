# M1: native host skeleton (status 2026-10-11)

**Done and verified on the Steam Frame:** Time Crisis II (SLUS-20219) runs in our own native OpenXR app. The game appears on a flat virtual screen at full speed, with audio, and the Frame controllers act as a DualShock 2.

## What was built
- **`host/`, the VR host (GPLv3).** It is built as an extra frontend of ARMSX2.
  - `Emu.{h,cpp}`: the embedded core, adapted from `pcsx2-sdl/Main.cpp`.
    - Settings live in `~/.local/share/ps2v/inis/PCSX2.ini`. On first run it picks the first console BIOS in `~/.local/share/ps2v/bios`.
    - Owns the CPU thread state machine and the `Host::` callbacks.
    - Applies DualShock 2 input through `Pad::SetControllerState`.
  - `XrApp.{h,cpp}`: the OpenXR instance, session, frame loop and actions.
    - Uses the Frame's native profile `/interaction_profiles/valve/frame_controller_valve`, with a Touch profile fallback.
  - `ScreenLayer.{h,cpp}`: copies each frame the GS publishes into an `R8G8B8A8_SRGB` swapchain (raw `vkCmdCopyImage`, so there is no double gamma) and shows it as an `XrCompositionLayerQuad`.
  - `main.cpp`: argument parsing, startup and teardown order, screen placement.
- **Shared Vulkan device without changing the core**, through ARMSX2's existing `VKLibretro` hook:
  1. XR_KHR_vulkan_enable (v1) gives us the runtime's instance extensions, so we create the VkInstance.
  2. The runtime picks the GPU.
  3. GSDeviceVK adopts both and creates the device, merging in the runtime's device extensions.
  4. The XR session binds to that device and queue 0.
  - A recursive `GraphicsQueueMutex()` serialises GS submits (via `lock_queue`), our submits and the OpenXR calls.
- **Fork patch (`external/armsx2`, branch `ps2v`):** the top-level `CMakeLists.txt` adds `PS2V_HOST_DIR`. ARMSX2's CMake has to stay the top-level project, because it uses `CMAKE_SOURCE_DIR` 137 times.
- **Tooling:**
  - `tools/sync-frame.sh` mirrors the repo to the headset.
  - `tools/build-frame.sh` runs a rootless Podman container with the SteamRT4 ARM64 SDK 4.0.20260805.254769. Static deps come from ARMSX2's own script. Output is `build/frame/bin/ps2v`.
  - `tools/run-frame.sh` runs the binary inside SteamLinuxRuntime_4-arm64.

## Verified on device
- **Runtime:** SteamVR/OpenXR 2.18.2.
- **Vulkan:**
  - Instance extensions from the runtime: external memory/fence/semaphore capabilities, get_physical_device_properties2, surface.
  - Device extensions: external memory/semaphore (+fd), timeline semaphore, dedicated allocation, image_format_list.
- **Session:** goes IDLE → READY → SYNCHRONIZED → VISIBLE → FOCUSED once the headset is worn. Off-head it stays at SYNCHRONIZED.
- **Screen:** a 2.4 m wide screen at 2.2 m, placed in front of the head on the first tracked frame. You confirmed this is comfortable.
- **Swapchain:** follows the GS canvas size (640×480 at 1×, 1920×1440 at 3×).
- **Performance:** 59.8 fps at 100% speed in the headset (EE about 25–40%, GPU 1–5%).
- **Audio** through Cubeb/PulseAudio works.
- **Controls** work:

  | Frame controller | DualShock 2 |
  |---|---|
  | Right A / B / X / Y | Cross / Circle / Square / Triangle |
  | Left D-pad | D-pad |
  | Sticks | Sticks |
  | Grips | L1 / R1 |
  | Triggers | L2 / R2 |
  | Stick clicks | L3 / R3 |
  | Left View (short press) | Start |
  | Left View (hold) | Select |

- **Clean shutdown** on SIGTERM or session exit (exit code 0).

## Problems found and fixed
- **Crash at exit.** On shutdown the GS destroyed the device and unloaded Vulkan while the XR side was still tearing down.
  - The CPU thread now waits for `Emu::Join()` before it closes the GS.
  - The host destroys the shared device and instance itself, after reloading the loader, because GSDeviceVK leaves a frontend-provided context to its owner.
- **Queue lock re-entrancy.** The core's wrapped `vkQueueSubmit` takes the queue lock, and so do we when we submit, so the mutex is recursive.
- **Wrong frame-controller assumptions.** SteamVR's automatic Touch emulation on the Frame merges B, X and Y, so the host suggests bindings for the native profile.

## Open items
- **TC2 intro movie artifacts.** During fast motion the intro FMV shows blocky "vertical slices". Paused frames are clean, and GS-rendered gameplay and subtitles are clean. Most likely this is the original MPEG compression, magnified by the large close screen.
  - Our fresh config enabled MTVU (8 cores). Turning it off did not clearly change this, and M0 ran with it off.
  - Consider an optional movie deblocking filter later.
- **Untested:** recentering by clicking both sticks.
- **Not yet:**
  - In-VR game picker and settings (FullscreenUI on a panel; M5).
  - Rumble to haptics.
  - Robust CD/DVD detection for rebuilt images: the test still uses the patched `TimeCrisisIIDVD-dvdfix.iso`.
- **The fork patch only exists locally** (uncommitted on branch `ps2v`). It needs a fork on GitHub before the submodule can point at it.
