Project Gauntlet for Linux
==========================

Gauntlet: Dark Legacy (Xbox, USA) recompiled for PC. This package holds no game code or data: setup.sh
translates and compiles the game on your PC from your own disc image (about 2-5 minutes).

You need
- an Xbox disc image of Gauntlet: Dark Legacy (USA), XISO or redump .iso
- python3 (3.8 or newer), clang or gcc, and a Vulkan driver
  Ubuntu/Debian: sudo apt install python3 clang libvulkan1 mesa-vulkan-drivers
  Fedora:        sudo dnf install python3 clang vulkan-loader mesa-vulkan-drivers
  Arch:          sudo pacman -S python clang vulkan-icd-loader (plus your GPU's vulkan driver)
- zenity (optional) for dialogs and the update prompt; curl for updates

Install
  ./setup.sh --iso "Gauntlet - Dark Legacy (USA).iso"
  options: --dir <folder> (default ~/.local/share/project-gauntlet), --hd <PCSX2 HD texture pack folder>
  Started without a terminal (double-click) it asks for the image with a file dialog.
  The game appears in your applications menu; or run <folder>/gauntlet.sh.

Play
  Keyboard: arrows = D-pad, Enter = START, Backspace = BACK, Z/X/C/V = A/B/X/Y, F/G = Black/White,
  Q/E = LT/RT, WASD = left stick. F11 or Alt+Enter = fullscreen, Esc = quit. Gamepads work through SDL.
  Settings: <folder>/settings.ini (the same GT_* keys as the Windows launcher). Log: <folder>/out/gauntlet.log.
  Saves: <folder>/out.

Update:    the game checks for a new version at start, or run a newer package's setup.sh --update --dir <folder>.
Uninstall: the menu entry's "Uninstall" action, or <folder>/kit/setup.sh --uninstall --dir <folder> (keeps out/).
