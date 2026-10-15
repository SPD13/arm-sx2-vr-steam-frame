# M0 feasibility spike: ARMSX2 on Steam Frame

Date: 2026-10-11. Verdict: **GO**. ARMSX2 runs Time Crisis II gameplay at full speed natively on the Frame, with large CPU and GPU headroom left for the VR side.

## Setup
- Headset: Steam Frame (retail), SteamOS 0.4.5 (build 20261007), kernel 6.18 aarch64, **4K pages**, 8 cores, 15 GB RAM.
- GPU: Adreno 750 with Mesa Turnip 26.3-devel, Vulkan 1.4.362. GPU devfreq `simple_ondemand`, 231–903 MHz.
- Emulator: ARMSX2 nightly-20261008 (b1f3196b9e), the Linux arm64 4K-pages AppImage, extracted and run as `squashfs-root/AppRun`.
  - All recompilers on: `ee_rec=1 iop_rec=1 vu0_rec=1 vu1_rec=1 fastmem=1`.
  - Vulkan renderer. ARMSX2 applied its Turnip profile automatically.
- How it was run:
  - Launched over SSH into the gamescope session (`DISPLAY=:0`), where it appears as a flat window in the headset.
  - Stats were read from ARMSX2's OSD via `gamescopectl screenshot` and from its `PerfLog` lines.
- Scripts: `tools/m0/run-armsx2.sh` and `tools/m0/vpad.py`. `vpad.py` is a uinput virtual gamepad used for navigating menus without a person.
- BIOS: SCPH-39001 (US v1.60).

## Results

| Run | Resolution | FPS / speed | EE | GS thread | GPU | GPU clock |
|---|---|---|---|---|---|---|
| BIOS menu | native 640×448 | 60 / 100% | 29% | 13% | 9% | 231–310 MHz |
| BIOS menu | 3× (1920×1344) | 60 / 100% | 29% | 9% | 26% | 231–310 MHz |
| TC2 cutscene | native | 30* / 100% | 48% (8.0 ms) | 6% | 1% | 231 MHz |
| TC2 gameplay, Stage 1 | native | 60 / 100% | 46% (7.6 ms) | 16% (2.6 ms) | 6.5% (1.1 ms) | 231 MHz |
| TC2 cutscene | 3× | 30* / 100% | 43% (7.2 ms) | 5% | 4% (1.4 ms) | 231 MHz |
| TC2 gameplay, Stage 1 | 3× | 60 / 100% | 47% (7.8 ms) | 17% (2.9 ms) | 19–22% (3.7 ms) | 231 MHz |

\*The game renders its cutscenes at 30 fps by design. Each full run of about 160 s averaged 59.8 fps.

The only stutter seen was a single drop to 87% speed during the intro, while shaders were compiled for the first time. ARMSX2 caches pipelines afterwards.

**What the numbers mean**
- The GPU never left its lowest clock, so in GPU terms 3× upscaling costs almost nothing. There is roughly 4× more clock available for the VR compositor and the volumetric pass.
- The EE thread uses under half of its frame budget on one core.

## Problem found: rebuilt DVD images detected as CD
The supplied `TimeCrisisIIDVD.iso` (SLUS-20219, 663 MB) booted to a black screen. The game's debug output showed `open fail name \SIO2MAN.IRX;1` repeating forever.

**Cause:** ARMSX2 and upstream PCSX2 decide whether an image is a CD or a DVD using two heuristics. This image triggers both, because it was rebuilt by an authoring tool (its volume descriptor is dated 2011) rather than dumped straight from the disc.
- `InputIsoFile::tryIsoType` (`pcsx2/CDVD/InputIsoFile.cpp`) treats the image as a CD when the root directory is exactly 2048 bytes.
- `cdvdDetectDisk` (`pcsx2/CDVD/CDVDcommon.cpp`) treats it as a CD when it has at most 452,849 sectors and the two copies of the block-size field (bytes 166 and 171 of the volume descriptor) are equal.

**Workaround:** a patched copy on the headset, `games/TimeCrisisIIDVD-dvdfix.iso`. The original file is untouched.
1. Set the root directory size in the volume descriptor to 4096. The next sector begins with a zero byte, so directory readers skip it.
2. Pad the image with zeros to 460,000 sectors.

**For the product (M1):** make disc-type detection robust in our fork. Prefer the UDF volume descriptors ("BEA01"/"NSR0x" at sectors 16+), and/or the GameDB serial-to-media type, over the size heuristics. Users' own rebuilt images should then boot as they are.

## Notes for later milestones
- Simulated X11 keys (XTest) never reach apps under gamescope. A uinput virtual gamepad does, because SDL sees it as an Xbox 360 controller. `steamos` is in the `input` group.
- ARMSX2 crashes with "terminate called without an active exception" when killed with SIGTERM. Harmless here, but our host must shut the VM down cleanly.
- The pipeline cache lands in `config/ARMSX2/cache/vulkan_pipelines_*.bin`. Ship or prewarm it to avoid first-run stutter.
- The CPU boost flag is 0 by default. Clocks were not pinned for these runs, so these are default-power-mode numbers.
- Not yet tested: other demanding games (Ratchet & Clank, Shadow of the Colossus, God of War) and GunCon 2 input. Light gun input comes in M4, but USB port 1 can be set to `guncon2` to test sooner.
