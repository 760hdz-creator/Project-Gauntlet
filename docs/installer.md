# Project Gauntlet installer and updater (build on the player's PC)

Nothing from Midway ships. `ProjectGauntlet-Setup-<ver>.exe` holds only our build kit and licensed third-party parts;
the game is lifted and compiled on the player's PC from their own disc image.

## Pieces
- `tools/package.py` -> `dist/payload.zip` from an allowlist of git-tracked files (`SHIP`: recomp lifter + tables,
  runtime source, `tools/gt_build.py`, `extract_xiso.py`, `match_pdb.py`, `pdb2.py`, `xbedis.py`, the toolchain
  script), the embedded CPython 3.15 (python.org) and the capstone package (BSD) + `licenses/`. Aborts if a
  `FORBIDDEN` pattern (xbe/iso/pdb/xbx/wmv/game/lifted/pdb_match/ps2/wad) is staged. ~14 MB.
- `installer/setup.cpp` (custom Win32 window over `installer/res/installer_bg.png`, icon `res/app.ico` = the rune
  stone): unpacks the payload with System32\tar.exe, runs `installer/install_toolchain.ps1` elevated when LLVM
  clang-cl or the VS C++ build tools are missing (VS 2022 Build Tools VCTools + LLVM 23.1.4, from their makers),
  then `python\python.exe -I tools\gt_build.py --root <dir> --iso <image> --version <ver>`. Progress from the
  build's `## ` lines (and `compiled (n/m)`), errors from `ERROR: `; log `<dir>\build\setup.log`. Then shortcuts,
  `HKCU\Software\ProjectGauntlet` (InstallLocation, Version) and an HKCU uninstall entry (`uninstall.exe`, a copy of
  the setup, `--uninstall --dir`; keeps `out\` = saves). Default folder `%LOCALAPPDATA%\Project Gauntlet` (per user,
  no admin except the toolchain step).
  - Over an existing install it asks: update (new kit + rebuild from the installed data, no disc) or full reinstall.
  - Command line: `--update [--dir X]` (the game's updater; starts at once), `--uninstall [--dir X]`,
    `--iso <image> [--dir X]` (full install, starts at once: testing).
- `tools/gt_build.py`: extract (XDVDFS) -> `game\`, SHA-256 of `Default.xbe` (0982be1a...) -> `match_pdb.py` with the
  disc's `shell3D.pdb` -> `build\pdb_match.tsv` -> `lift.py` -> `build\lifted\` -> clang-cl (vcvars env via vswhere,
  LLVM from Program Files) in parallel -> `lld-link /SUBSYSTEM:WINDOWS` -> `<dir>\gauntlet.exe`, built with
  `-DGT_VERSION="<ver>"`. About 1 minute on the dev box (8 jobs).
- Installed `gauntlet.exe` runs from its own folder (cwd = exe dir when `game\Default.xbe` is not found), logs to
  `out\gauntlet.log` (no console), and relaunches itself on HalReturnToFirmware(1..3) (XAPI quick-reboots once on
  the first boot after it prepares the hard disk partitions).

## Launcher (setup.cpp `--launcher`)
- The install keeps a copy of the setup as `<install>\launcher.exe`; the desktop / Start menu shortcuts run
  `launcher.exe --launcher` and the uninstall entry runs `launcher.exe --uninstall --dir <install>` (0.1.0 had
  `uninstall.exe`, removed on update; an update also repoints existing shortcuts from `gauntlet.exe` to the launcher).
- Window: Play (starts `gauntlet.exe` with GT_NOUPDATE=1 and closes), update check on open (forge releases/latest; a
  newer tag shows Update: downloads the new setup to %TEMP%, runs `--update --dir <install>`, closes; the setup retries
  copying over `launcher.exe` for 15 s), Display (fullscreen, widescreen, render scale, MSAA, anisotropic filtering,
  gamepads), HD textures (on/off + pack folder with a PNG count), Uninstall.
- Settings: `<install>\settings.ini`. `[settings]` holds GT_* variable names; `gauntlet.exe` (main.c load_settings)
  sets each one that is not already in the environment, so env vars still win for tests. A default choice deletes
  the key. `[launcher] HdFolder` remembers the pack folder while HD is off (GT_HDTEX=0).
- HD pack: the picked folder is used in place (GT_HDTEX=<folder>, scanned recursively), never copied (2.1 GB). The setup
  has the same optional picker (`--hd <folder>` on the command line). Uninstall never touches it.
- Buttons and checkboxes are owner-drawn; labels use a pattern brush of the art. Preview:
  `installer\preview.ps1 -Out x.png -Exe <install>\launcher.exe -ExeArgs --launcher`.

## Updates (runtime/src/updater.c)
- At start (not with GT_INPUT, GT_NOUPDATE=1 or dev builds without GT_VERSION) a thread GETs
  `https://git.8gramlabs.com/api/v1/repos/8gramlabs/Project-Gauntlet/releases/latest`; a newer `tag_name` than
  GT_VERSION asks the player, downloads the asset `ProjectGauntlet-Setup-*.exe` to %TEMP%, runs it
  `--update --dir <install>` and exits. Testing: `GT_UPDATE_REPO=<owner/repo> GT_UPDATE_ASSET=<prefix>`
  (verified 2026-11-18 against 8gramlabs/TCSCC-Recomp v0.1.11: check + prompt; the download/run step untested).

## Release steps
1. Bump `VERSION`; `git add` new tracked files (package.py stages from `git ls-files`).
2. `installer\build.bat` -> `dist\ProjectGauntlet-Setup-<ver>.exe`.
3. Test: `dist\ProjectGauntlet-Setup-<ver>.exe --iso <image> --dir <empty folder>` (and Windows Sandbox for a clean PC).
4. Tag `v<ver>`, push both remotes, then publish on the forge (the updater reads it) and GitLab:
   `tea release create --login forge --repo 8gramlabs/Project-Gauntlet --tag v<ver> --title v<ver> --note "<notes>"
   --asset dist/ProjectGauntlet-Setup-<ver>.exe` and `glab release create v<ver> dist/ProjectGauntlet-Setup-<ver>.exe`.

## Verified (2026-11-18, dev box)
- Setup `--iso` into an empty folder: extract, check, match, lift, 20 files compiled, link, shortcuts, registry,
  uninstall entry (~60 s). First launch: quick reboot -> relaunch -> intro movie; no console; log file written.
- `--update --dir`: rebuild without the disc (~55 s). Uninstall: removes kit + game data + shortcuts + registry,
  keeps `out\`, deletes itself.
- Installer window rendered with `installer\preview.ps1 <out.png> <setup.exe>`.

- 2026-11-18 launcher: setup `--update --dir C:\Games\Project Gauntlet` over the 0.1.0 install (~55 s): launcher.exe
  written, uninstall.exe removed, both shortcuts now `launcher.exe --launcher`, UninstallString updated. Launcher
  rendered with ini values loaded (8x MSAA, fullscreen, 19634 PNG found); the installed game then logged scale 4,
  msaa 8, widescreen 1.672, back buffer 3440x1440, hdtex 19633 textures from the pack folder. Update check against
  the forge with no release (404) shows "Up to date".

- 2026-11-18 0.2.0: setup `--update --dir C:\Games\Project Gauntlet` (~55 s, settings.ini kept); the on-PC
  lift applies SOURCE_PATCHES (g_frame_ms in build\lifted_003.c); the installed game ran at 58.7 fps with GT_FPS=60.
  Launcher driven by BM_CLICK / posted WM_KEYDOWN: Controls page, key capture (A -> X unbinds B), Back, settings
  written (GT_KEYS, GT_FPS, GT_FXAA, GT_SHARPEN). Uninstall still not run from the launcher.

## Open
- Launcher not click-tested by hand (Play, Update download, Uninstall from the launcher, HD Browse); fresh install
  with the HD picker not run.
- Not run on a clean PC (toolchain install, file dialogs): test in Windows Sandbox.
- Setup and game exe are unsigned (SmartScreen / Defender may warn).
