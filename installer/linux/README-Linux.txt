Project Gauntlet for Linux
==========================

Gauntlet: Dark Legacy (Xbox, USA) recompiled for PC. This package holds no game code or data: setup.sh
translates and compiles the game on your PC from your own disc image (about 2-5 minutes).

You need
- an Xbox disc image of Gauntlet: Dark Legacy (USA), XISO or redump .iso
- a Linux with glibc 2.38 or newer: Ubuntu 24.04, Debian 13, Fedora 39, Linux Mint 22, Arch, SteamOS 3.6 or newer
- python3 (3.9 or newer), clang or gcc, and a Vulkan driver
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
  Q/E = LT/RT, WASD = left stick. F11 or Alt+Enter = fullscreen, Esc = quit.
  Controllers: up to 4 (GT_PADS in settings.ini), in connection order. Xbox, PlayStation 3/4/5, Switch Pro and
  Joy-Con, Steam Controller / Steam Deck, 8BitDo and other USB or Bluetooth pads work through SDL3; buttons go by
  position (the bottom face button is A). A pad SDL does not know gets a generic layout; rearrange its buttons
  with GT_PADMAP, or add its SDL mapping line to <folder>/controllers.txt (kept by updates).
  PlayStation and Switch pads over USB or Bluetooth read best with the udev rules from the steam-devices package
  (Ubuntu/Debian/Fedora: sudo apt install steam-devices / sudo dnf install steam-devices; Arch: game-devices-udev).
  Steam Deck: add gauntlet.sh to Steam as a non-Steam game so the built-in controls act as a gamepad.
  Settings: <folder>/settings.ini (the same GT_* keys as the Windows launcher). Log: <folder>/out/gauntlet.log.
  Saves: <folder>/out.

Update:    the game checks for a new version at start, or run a newer package's setup.sh --update --dir <folder>.
Uninstall: the menu entry's "Uninstall" action, or <folder>/kit/setup.sh --uninstall --dir <folder> (keeps out/).
