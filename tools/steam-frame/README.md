# Steam Frame install and releases

ARMSX2 - Steam Frame Edition installs on the Frame as a Steam devkit title. The game sits in `~/devkit-game/<gameid>/`, and its `run.sh` starts it inside the Steam Linux Runtime 4.0 ARM64, which must be installed from the Steam library. In the library it shows as **ARMSX2 - Steam Frame Edition**, with the artwork and icon from `assets/steam/` (see `tools/steam-art/README.md`). It works the same way as the Visual Pinball X and CitraVR Steam Frame ports: Valve's devkit scripts register the title, and the Steam client names it and sets its icon.

## For players: the installer app

1. On the Frame: **Settings > System > Enable Developer Mode**, then in **Developer**, **Set User Password**.
2. On the computer, open the installer:
   - Windows: `ARMSX2-Frame-Installer-Windows.exe`
   - macOS: `ARMSX2 Installer.app` from the `.zip`
   - Linux: `ARMSX2-Frame-Installer` from the `.tar.gz`, which opens in the browser
3. Step 1 finds the Frame on the network and signs in, with the password or an ssh key. It then checks that Steam is running and that there is enough free space.
4. Step 2 downloads the latest release from GitHub and installs it, or updates the copy already there.

The installers are not signed. On Windows, SmartScreen warns on first launch: choose **More info > Run anyway**. On macOS, Gatekeeper refuses the app on first launch: right-click it and choose **Open**, or allow it in **System Settings > Privacy & Security**. macOS also asks to let the app reach the local network; it needs that to find the Frame.

The installer's log is in the user cache folder, under `armsx2-frame-installer/installer.log`. The page shows its full path when something fails.

**Uninstall** removes the game, its library entry, its artwork and its icon. It always keeps the games, BIOS files, memory cards, save states and texture packs in `~/.local/share/ps2v`. The option to delete settings also removes `inis/`, `gamesettings/`, `inputprofiles/`, `cache/`, `logs/`, `debug/` and `transfer/`.

## For developers

| Script | Runs on | What it does |
|---|---|---|
| `deploy-to-frame.sh [gameid]` | your computer | Syncs, builds on the Frame, then updates the library's **ARMSX2 - Steam Frame Edition** (gameid `ARMSX2`) with that build, versioned `<date>-<short commit>-dev`. Another gameid installs a separate copy. The game files never leave the Frame. |
| `make-release.sh [version]` | your computer | Syncs, builds on the Frame, stages the release there and downloads it to `build/release/armsx2-steam-frame-<version>.tar.gz`. The version defaults to `<date>-<short commit>`. |
| `make-installer.sh [version]` | a Mac | Builds the installer app for Windows (x64), macOS (universal) and Linux (x86_64, arm64) into `build/installer/`. Needs Go (`brew install go`) and Xcode's command line tools. |
| `stage-release.sh` | the Frame | Assembles the release folder: the game with debug info stripped, the install tools, the artwork, Valve's devkit scripts, `LICENSE`, `THIRD_PARTY_NOTICES.md` and `SOURCE.md`. |
| `frame-install.sh` | the Frame | The install steps, run from the release folder by the installer and by `deploy-to-frame.sh`. |
| `frame-uninstall.sh` | the Frame | The uninstall steps, embedded in the installer app at build time. |
| `run.sh` | the Frame | What Steam starts. It runs `ps2v` inside the Steam Linux Runtime 4.0 ARM64 (Steam does not apply a devkit title's compatibility tool) and writes its output to `~/.local/share/ps2v/logs/ps2v.log`. |
| `install-library-art.py`, `set-library-name.py` | the Frame | Copy the artwork into Steam's grid folder. Name the title and set its icon through the Steam client. |

`FRAME_HOST` (default `steamos@frame.local`) and `FRAME_DIR` (default `ps2-volumetric/repo`) select the Frame, as for `tools/sync-frame.sh`. Valve's devkit scripts (MIT) are cloned into `build/steamos-devkit` on first use.

### Publishing a release

1. Commit, push `main` and the ARMSX2 submodule's `ps2v` branch: `SOURCE.md` names both commits, as the GPL requires.
2. Run `tools/steam-frame/make-release.sh` and `tools/steam-frame/make-installer.sh`.
3. Test before publishing: run the installer with `--archive build/release/armsx2-steam-frame-<version>.tar.gz`, which installs that file instead of the latest release. `--gameid` installs under another id; `--browser` opens the page in the browser.
4. Create a GitHub release on `SPD13/arm-sx2-vr-steam-frame`. Attach the `.tar.gz`, the four installer files and `THIRD_PARTY_NOTICES.md` (all in `build/installer/` and `build/release/`). The installer always takes the latest release's `armsx2-steam-frame-*.tar.gz` and checks it against GitHub's SHA-256 digest.

The installer downloads without signing in to GitHub, so `SPD13/arm-sx2-vr-steam-frame` must be public before players can use it. Until then, only `--archive` installs work.
