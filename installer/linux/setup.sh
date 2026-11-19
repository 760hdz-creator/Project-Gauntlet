#!/bin/sh
# Project Gauntlet for Linux: install, update or uninstall (the Linux counterpart of ProjectGauntlet-Setup.exe).
# Nothing from Midway ships: the game is translated and compiled on this PC from your own disc image.
#
#   ./setup.sh [--iso <image.iso>] [--dir <folder>] [--hd <texture pack folder>] [--play]
#   ./setup.sh --update --dir <folder> [--play]     rebuild with this package's kit (no disc needed)
#   ./setup.sh --uninstall [--dir <folder>]         remove the game (keeps <folder>/out: saves, settings, log)
#
# Default folder: ${XDG_DATA_HOME:-~/.local/share}/project-gauntlet. Needs glibc 2.38+, python3 (3.9+), clang or gcc, and a
# Vulkan driver (libvulkan.so.1). With zenity installed and no terminal, it asks with dialogs and shows progress.
set -eu
here=$(cd -- "$(dirname -- "$0")" && pwd)
version=$(cat "$here/kit/VERSION" 2>/dev/null || echo 0.0.0)
mode=install iso="" dir="" hd="" play=0 nogui=0
while [ $# -gt 0 ]; do
    case $1 in
    --iso) iso=$2; shift 2 ;;
    --dir) dir=$2; shift 2 ;;
    --hd) hd=$2; shift 2 ;;
    --update) mode=update; shift ;;
    --uninstall) mode=uninstall; shift ;;
    --play) play=1; shift ;;
    --no-gui) nogui=1; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done
data=${XDG_DATA_HOME:-$HOME/.local/share}
dir=${dir:-$data/project-gauntlet}
gui=0
if [ $nogui = 0 ] && [ ! -t 0 ] && [ ! -t 1 ] && command -v zenity >/dev/null 2>&1 && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then gui=1; fi
title="Project Gauntlet $version"

say() { if [ $gui = 1 ]; then zenity --info --title "$title" --width 420 --text "$1" 2>/dev/null || true; else echo "$1"; fi; }
die() {
    if [ $gui = 1 ]; then zenity --error --title "$title" --width 480 --text "$1" 2>/dev/null || true; fi
    echo "setup: $1" >&2
    exit 1
}

desktop_entry() {   # .desktop file + icon (the menu entry starts gauntlet.sh)
    apps=$data/applications icons=$data/icons/hicolor/256x256/apps
    mkdir -p "$apps" "$icons"
    cp "$here/kit/linux/icon.png" "$icons/project-gauntlet.png"
    cat > "$apps/project-gauntlet.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Project Gauntlet
Comment=Gauntlet Dark Legacy (recompiled)
Exec="$dir/gauntlet.sh"
Path=$dir
Icon=project-gauntlet
Terminal=false
Categories=Game;
Actions=uninstall;

[Desktop Action uninstall]
Name=Uninstall
Exec="$dir/kit/setup.sh" --uninstall --dir "$dir"
EOF
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" 2>/dev/null || true
}

if [ $mode = uninstall ]; then
    [ -f "$dir/gauntlet.sh" ] || die "No Project Gauntlet install in $dir"
    if [ $gui = 1 ]; then
        zenity --question --title "$title" --width 420 --text "Remove Project Gauntlet from $dir?\n\nSaves, settings and the log in $dir/out are kept." 2>/dev/null || exit 0
    fi
    entry=$data/applications/project-gauntlet.desktop
    if [ -f "$entry" ] && grep -qF "Path=$dir" "$entry"; then   # only the menu entry that belongs to this folder
        rm -f "$entry" "$data/icons/hicolor/256x256/apps/project-gauntlet.png"
    fi
    rm -rf "$dir/kit" "$dir/game" "$dir/build" "$dir/lib" "$dir/gauntlet" "$dir/gauntlet.sh" "$dir/README-Linux.txt"
    rmdir "$dir" 2>/dev/null || true
    say "Project Gauntlet was removed. Your saves are still in $dir/out."
    exit 0
fi

# ---- checks ----
command -v python3 >/dev/null 2>&1 || die "python3 is missing. Install it with your package manager (for example: sudo apt install python3)."
python3 -c 'import sys; sys.exit(sys.version_info < (3, 9))' || die "python3 3.9 or newer is needed."
# the prebuilt libraries (SDL3, DXVK, vkd3d, the runtime) need glibc 2.38+ and libstdc++ from GCC 13+
glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')
case $glibc in
2.*) [ "${glibc#2.}" -ge 38 ] 2>/dev/null || die "This Linux is too old for Project Gauntlet: it has glibc $glibc, 2.38 or newer is needed (Ubuntu 24.04, Debian 13, Fedora 39, Linux Mint 22, Arch, SteamOS 3.6 or newer)." ;;
*) [ -n "$glibc" ] || die "Project Gauntlet needs a glibc-based Linux (glibc 2.38 or newer)." ;;
esac
cc_ok=0
for c in ${CC:-} clang gcc cc; do command -v "$c" >/dev/null 2>&1 && cc_ok=1 && break; done
[ $cc_ok = 1 ] || die "No C compiler found. Install clang (for example: sudo apt install clang, sudo dnf install clang, sudo pacman -S clang)."
if ! { ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; } && ! ls /usr/lib*/libvulkan.so.1 /usr/lib/*/libvulkan.so.1 >/dev/null 2>&1; then
    die "The Vulkan loader (libvulkan.so.1) is missing. Install your GPU's Vulkan driver (for example: sudo apt install libvulkan1 mesa-vulkan-drivers)."
fi

if [ $mode = install ] && [ -z "$iso" ] && [ -f "$dir/game/Default.xbe" ]; then
    # an existing install: update it (keeps the extracted disc) unless a new image is given
    mode=update
fi
if [ $mode = install ] && [ -z "$iso" ]; then
    if [ $gui = 1 ]; then
        iso=$(zenity --file-selection --title "Select your Gauntlet: Dark Legacy (Xbox, USA) disc image" \
              --file-filter "Disc images | *.iso *.ISO *.xiso" 2>/dev/null) || exit 0
    else
        die "Give your disc image: ./setup.sh --iso <Gauntlet Dark Legacy (USA).iso> [--dir <folder>]"
    fi
fi
[ -z "$iso" ] || [ -f "$iso" ] || die "Disc image not found: $iso"
if [ $mode = update ] && [ ! -f "$dir/game/Default.xbe" ]; then die "No installed game data in $dir: run a full install with --iso."; fi
[ -z "$iso" ] || iso=$(cd -- "$(dirname -- "$iso")" && pwd)/$(basename -- "$iso")

# ---- kit + build ----
mkdir -p "$dir"
dir=$(cd -- "$dir" && pwd)
if [ "$here" != "$dir/kit" ]; then
    rm -rf "$dir/kit.new"
    cp -R "$here/kit" "$dir/kit.new"
    cp "$here/setup.sh" "$dir/kit.new/setup.sh"
    rm -rf "$dir/kit"
    mv "$dir/kit.new" "$dir/kit"
    cp "$here/README-Linux.txt" "$dir/" 2>/dev/null || true
fi
mkdir -p "$dir/build" "$dir/out"
log=$dir/build/setup.log
set -- python3 -I "$dir/kit/tools/gt_build.py" --root "$dir" --version "$version"
[ -z "$iso" ] || set -- "$@" --iso "$iso"
status=0
if [ $gui = 1 ]; then
    # the build writes the log on its own; the dialog only follows it (closing the dialog never stops the build)
    : > "$log"
    "$@" > "$log" 2>&1 < /dev/null &
    pid=$!
    ( tail -n +1 -f --pid=$pid "$log" 2>/dev/null | { p=0; while IFS= read -r line; do
        case $line in
        "## Done"*) echo 100 ;;
        "## "*) [ $p -lt 40 ] && p=$((p + 6)); echo $p; echo "# ${line#\#\# }" ;;
        "compiled "*) n=${line##*(}; n=${n%)}; echo $(( 40 + 55 * ${n%/*} / ${n#*/} )) ;;
        esac
      done; } ) | zenity --progress --auto-close --no-cancel --title "$title" --width 460 --text "Starting" 2>/dev/null || true
    wait $pid || true
    grep -q "^## Done" "$log" || status=1
else
    "$@" 2>&1 | tee "$log" | grep --line-buffered -E "^(## |ERROR|compiled .*0/)" || true
    grep -q "^## Done" "$log" || status=1
fi
if [ $status != 0 ]; then
    err=$(grep -m1 -A12 "^ERROR: " "$log" | cut -c1-300 || true)
    die "The build failed. ${err:-See $log}"
fi

# ---- launcher, settings, menu entry ----
cat > "$dir/gauntlet.sh" <<'EOF'
#!/bin/sh
# Starts Project Gauntlet from its folder; the log goes to out/gauntlet.log. Settings: settings.ini.
cd -- "$(dirname -- "$0")" || exit 1
mkdir -p out
export DXVK_LOG_PATH=out DXVK_STATE_CACHE_PATH=out GT_INSTALLED=1
exec ./gauntlet "$@" 2>out/gauntlet.log
EOF
chmod +x "$dir/gauntlet.sh" "$dir/kit/setup.sh"
if [ ! -f "$dir/settings.ini" ]; then
    cat > "$dir/settings.ini" <<'EOF'
; Project Gauntlet settings (same keys as the Windows launcher). Remove the ; to use a line.
[settings]
; GT_FULLSCREEN=1        ; start in borderless fullscreen (F11 or Alt+Enter toggles)
; GT_FPS=60              ; 60 fps (default: the original 30)
; GT_WIDESCREEN=0        ; original 4:3 picture
; GT_SCALE=4             ; render scale 1..8 (default: from the monitor height)
; GT_MSAA=4              ; anti-aliasing 1/2/4/8
; GT_ANISO=16            ; anisotropic filtering 0..16
; GT_HDTEX=/path/to/pack ; PCSX2 HD texture pack folder (GT_HDTEX=0 = off)
; GT_HITFLASH=35         ; hit flash strength 0..100
; GT_DRAWDIST=150        ; draw distance percent 100..400
; GT_SHARPEN=50          ; sharpening 0..100
; GT_FXAA=1
; GT_CRT=1
; GT_MONITOR=0           ; display number
; GT_PADS=2              ; gamepads in the game (1..4)
; GT_NOUPDATE=1          ; no update check at start
EOF
fi
if [ -n "$hd" ]; then
    [ -d "$hd" ] || die "HD texture folder not found: $hd"
    hd=$(cd -- "$hd" && pwd)
    grep -v '^GT_HDTEX=' "$dir/settings.ini" > "$dir/settings.ini.new" || true
    sed -i "s|^\[settings\]\$|[settings]\nGT_HDTEX=$hd|" "$dir/settings.ini.new"
    mv "$dir/settings.ini.new" "$dir/settings.ini"
fi
desktop_entry
if [ $play = 1 ]; then
    nohup "$dir/gauntlet.sh" >/dev/null 2>&1 &
elif [ $mode = install ]; then
    say "Project Gauntlet is installed in $dir.\nStart it from your applications menu or with $dir/gauntlet.sh"
else
    say "Project Gauntlet was updated to $version."
fi
