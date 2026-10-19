# Keeping up with upstream ARMSX2

2026-10-14

## Summary

The emulator core is a fork of [ARMSX2/ARMSX2](https://github.com/ARMSX2/ARMSX2), pinned in this repo as the submodule `external/armsx2`. Upstream moves fast: about 430 commits in the three months to October 2026, most of them in the GS renderer we patch. The fork has to absorb that steadily without losing our changes or breaking the releases already shipped.

Three rules make that work:

1. **Merge upstream into `ps2v`, never rebase it.** Every commit of this repo pins an exact fork commit. Rewriting `ps2v` would leave older pins pointing at commits that no longer exist, and a shipped binary has to keep its matching source (GPLv3).
2. **Keep our code in files upstream does not have.** Upstream files get one-line hooks into them, not our logic. A merge then conflicts only where a hook sits, and resolving it means moving one line.
3. **Sync on a schedule, and send generic fixes upstream.** Every patch upstream takes is one we stop carrying.

## Repositories and branches

| Where | Branch / tag | Role |
|---|---|---|
| `SPD13/arm-sx2-vr-steam-frame` | `main` | The host, tools, assets, docs and the submodule pin. Upstream never sees it. |
| `SPD13/armsx2-steam-frame` (public fork) | `master` | An exact mirror of `upstream/master`. Never commit to it. |
| | `ps2v` | The product: upstream plus our patches. Only fast-forwards and merges; no force-push. |
| | feature branches (`lightgun`, …) | Branched from `ps2v` and merged back into it. Use the same name as the matching branch in this repo. |
| | `sync/YYYY-MM-DD` | Temporary branch for each upstream merge (see below). |
| | tags `armsx2-vr-vX.Y` | One per release, matching the tag in this repo. |

Remotes in `external/armsx2`: `origin` = the fork, `upstream` = ARMSX2/ARMSX2. The host builds outside the core's tree through `EXTERNAL_FRONTEND_DIR` (top-level `CMakeLists.txt`), so `host/` never touches upstream files.

One-time setup in `external/armsx2`:

```sh
git config rerere.enabled true      # remember conflict resolutions, replay them on later merges
git config rerere.autoupdate true
```

## Where our code lives

### Files only we have (never conflict)

| File | What it holds |
|---|---|
| `pcsx2/GS/GSVolumetric.h` / `.cpp` | The API the host uses (enable, stereo settings, eye views, debug dumps and traces) and its shared state; the VSync bookkeeping (`BeginVSync`, `EndMerge`). |
| `pcsx2/GS/Renderers/HW/GSRendererHWVolumetric.h` / `.cpp` | The hardware renderer's depth capture (pairings, snapshots, `MergeDepth`), eye tagging, stereo draw classification and VS constants, and draw traces. Its state is one member, `GSRendererHW::m_vr`. |
| `pcsx2/GS/Renderers/Vulkan/GSDeviceVKVolumetric.cpp` | Turning on multiview, the right-eye backbuffer, the depth/pre-HUD images handed over in `VKLibretro::Frame`, render-pass checks, the Turnip constant-buffer workaround, and the `VOLUMETRIC_FLUSH_FILE` debugging. |

### What stays in upstream files

These are the merge-conflict surface. Lines added versus the upstream base after the 2026-10-14 refactor:

| Upstream file | Lines | Why it can't move out |
|---|---|---|
| `GS/Renderers/Vulkan/GSDeviceVK.cpp` | +259 | Multiview plumbing inside upstream functions: the render-pass `multiview` key and view mask, the per-layer loops at the top of each `Do*` utility pass, layer counts in copies, blits and barriers, and array views in descriptor writes. |
| `GS/Renderers/Vulkan/GSTextureVK.cpp/.h` | +199 | Layered (per-eye) textures, array views and layer proxies, built into the texture class. |
| `USB/usb-lightgun/guncon2.cpp/.h` | +109 | Host-driven light guns and the game's Set Param. **Upstream candidate.** |
| `bin/resources/shaders/vulkan/tfx.glsl` | +76 | The parallax shift in the vertex shader, and `gl_ViewIndex`. |
| interlace shaders (Vulkan, GL, DX11, Metal), `GSInterlaceModePolicy.h` | +141 | Comb-adaptive movie deinterlace (shader mode 4), with tests in `tests/ctest/core/gs/`. **Upstream candidate.** |
| `GS/Renderers/Common/GSDevice.cpp/.h`, `GSTexture.*` | +80 | The layered-texture API (`CreateLayeredSurface`, layer proxies, stereo-content flag). |
| `GS/Renderers/Vulkan/GSDeviceVK.h` | +44 | Declarations; ours sit in one block. |
| `GS/Renderers/Common/GSRenderer.cpp/.h` | +46 | Hooks: `MergeDepth` after the merge, `GSVolumetric::BeginVSync/EndMerge`, the presented rectangle; the movie detection for the deinterlace. |
| `ImGui/ImGuiManager.*` | +36 | The frontend can show OSD messages itself. **Upstream candidate.** |
| `GS/Renderers/HW/GSRendererHW.cpp/.h` | +34 | Eight one-line hooks into `GSRendererHWVolumetric.cpp`, plus `m_vr`. |
| `VKLibretro.h` | +29 | The extra `Frame` fields the host reads. |
| CMake, `GS.cpp/.h`, `IPU.cpp`, `GSTextureCache.*` | +38 | Building `host/`, our sources, the IPU decode count, small hooks. |

The Windows project files (`pcsx2.vcxproj`) do not list our files. We build only on Linux ARM64 (the Frame) through CMake.

### Rules for new work in the core

- New logic goes in a `*Volumetric.*` file, or a new file of our own, and not into an upstream function body. The upstream file gets one call: `VolumetricSomething(...)`.
- New renderer state goes into `GSRendererHWVolumetricState`, not into new `GSRendererHW` members.
- Don't reformat, rename or reorder upstream code. Leave upstream lines as they are even when they're ugly.
- Put our declarations in upstream headers together in one commented block (`// ARMSX2 VR …`).
- Keep shader changes small and behind macros (like `MULTIVIEW`), so upstream's lines stay as they are.
- Name things "Volumetric", "Stereo" or "ARMSX2 VR", so `git grep` finds every hook.
- Keep commits that could go upstream (bug fixes, generic hooks, movie deinterlace) separate from VR-only commits, so they can be cherry-picked onto upstream as they are.

## Syncing with upstream

**When:** weekly, or before a release, whichever comes first. Don't chase every upstream commit, and don't sync during a Frame test session.

**Procedure** (in `external/armsx2`):

```sh
git fetch --multiple upstream origin
git switch ps2v && git pull --ff-only origin ps2v
git switch -c sync/$(date +%F)

# What upstream changed where we have code (read it before merging):
git log --oneline ps2v..upstream/master -- pcsx2/GS pcsx2/USB/usb-lightgun pcsx2/ImGui \
    pcsx2/IPU bin/resources/shaders CMakeLists.txt pcsx2/CMakeLists.txt

git merge --no-ff upstream/master -m "Merge upstream ARMSX2 $(git rev-parse --short upstream/master)"
# Conflicts: keep upstream's version of upstream code, and put our hook back where it belongs.
```

Then:

1. Build on the Frame (`tools/sync-frame.sh`, then `tools/build-frame.sh` there). The Frame build turns tests off (`-DENABLE_TESTS=OFF`). When a sync touches code that has tests, such as the interlace policy, also build the core with tests on and run `ctest`.
2. Smoke test on the Frame in both modes (Volumetric and Flat). Use Time Crisis II for stereo, depth, the gun and the intro movie, and one other game.
3. Watch for changes that merge cleanly but still break us. The `git log` above is the list to check. Typical cases: upstream changing the texture cache or render-pass setup under a hook, new `Do*` utility passes in `GSDeviceVK` that need the per-layer loop, and upstream light-gun work in `usb-lightgun/`.
4. Fast-forward `ps2v` to the sync branch and push both `ps2v` and the mirror:
   ```sh
   git switch ps2v && git merge --ff-only sync/$(date +%F)
   git push origin ps2v && git push origin upstream/master:master
   git branch -d sync/$(date +%F)
   ```
5. In this repo, bump the submodule in a commit of its own: `ARMSX2: sync upstream YYYY-MM-DD (upstream <short sha>)`.

If a sync goes badly, delete the sync branch. `ps2v` and the pin haven't moved.

**Hotfixes between syncs:** `git cherry-pick -x <upstream sha>` onto `ps2v`. The next merge recognises the change.

**Releases:** tag the fork commit (`armsx2-vr-vX.Y`) and push the tag before publishing a build. Tag the same version in this repo. That fork commit is the source we must be able to hand out for the release.

## Sending changes upstream

Every patch upstream accepts is one we stop carrying. The current candidates are general-purpose:

- comb-adaptive deinterlace for frame-mode movies (`GSInterlaceModePolicy`, the interlace shaders, the IPU decode count): PR 2, `pr/movie-deinterlace`, opt-in through the GS setting `movie_comb_deinterlace`, which the host turns on (`host/Emu.cpp`)
- the multiview constant-buffer fix on Turnip, once multiview itself is upstream
- GunCon 2: log the game's Set Param, and per-port host light guns
- ImGui: let the frontend show OSD messages itself
- `EXTERNAL_FRONTEND_DIR`, the generic "out-of-tree frontend" CMake option (was `PS2V_HOST_DIR`): in PR 1, `pr/frontend-hooks`

Procedure: branch from `upstream/master` (not from `ps2v`) and cherry-pick the commit, fixing it until it stands alone. Open the PR from the fork. Once it's merged upstream, the next sync makes the two copies identical, and the conflict marks, if any, are trivial.

A larger opportunity: ask the maintainer whether they'd accept a small "stereo / VR hooks" interface in `GSRendererHW` and `GSDeviceVK`. With that interface upstream, most of the remaining footprint would turn into implementations in our own files.
