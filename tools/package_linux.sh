#!/usr/bin/env bash
# Linux package: dist/ProjectGauntlet-Linux-x64-<VERSION>.tar.gz (run on Linux / WSL from a checkout of this repo).
#
#   tools/package_linux.sh [out dir]      (default: dist/)
#
# Holds our build kit (git-tracked files, as tools/package.py ships for Windows), the prebuilt runtime
# (tools/build_linux.sh: libgtrt.a, SDL3 + DXVK Native + vkd3d-shader shared libraries), capstone 5.0.7 (BSD,
# manylinux wheel from PyPI) and the licenses. No game code or data: setup.sh lifts and compiles the game on the
# player's PC from their own disc. Aborts if anything FORBIDDEN is staged.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=${1:-$root/dist}
ver=$(tr -d '\r\n' < "$root/VERSION")
name=ProjectGauntlet-Linux-x64-$ver
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
stage=$work/$name
kit=$stage/kit
mkdir -p "$kit/linux" "$kit/licenses" "$kit/pylib" "$out"

# runtime + shared libraries, stamped with the version (the game's updater compares it)
bash "$root/tools/build_linux.sh" "$work/rt" --version "$ver"
cp "$work/rt/libgtrt.a" "$kit/linux/"
cp -r "$work/rt/lib" "$work/rt/include" "$kit/linux/"
strip --strip-unneeded "$kit/linux/lib/"*.so* 2>/dev/null || true
cp "$root/installer/res/icon.png" "$kit/linux/icon.png"
# Android kit (no game code; setup.sh --android builds the APK from it): tools/build_android.sh stage 1
akit=${GT_ANDROID_KIT:-}
if [[ -z $akit ]]; then bash "$root/tools/build_android.sh" "$work/android" --version "$ver" >/dev/null; akit=$work/android/android-kit; fi
mkdir -p "$kit/android"
cp -r "$akit/libgtrt.a" "$akit/include" "$akit/template.apk" "$kit/android/"
for f in "$akit/licenses/"*; do cp "$f" "$kit/licenses/android_$(basename "$f")"; done

# kit files (git-tracked)
ship=(VERSION README.md recomp/lift.py recomp/hle.txt recomp/hooks.txt recomp/seeds_extra.txt 'recomp/include/*'
      tools/gt_build.py tools/gt_android.py tools/pyrun.py tools/extract_xiso.py tools/match_pdb.py tools/pdb2.py tools/xbedis.py)
cd "$root"
for pat in "${ship[@]}"; do
    git ls-files -- "$pat" | while read -r f; do mkdir -p "$kit/$(dirname "$f")"; tr -d '\r' < "$f" > "$kit/$f"; done
done
cp "$root/installer/linux/setup.sh" "$stage/setup.sh"
cp "$root/installer/linux/README-Linux.txt" "$stage/README-Linux.txt"
tr -d '\r' < "$root/installer/linux/ProjectGauntlet" > "$stage/ProjectGauntlet"   # RecompLauncher starts this
tr -d '\r' < "$root/recomplauncher.json" > "$stage/recomplauncher.json"
chmod +x "$stage/setup.sh" "$stage/ProjectGauntlet"

# capstone 5.0.7 (manylinux wheel: pure Python + its own libcapstone.so)
whl_dir=${GT_CACHE:-$HOME/.cache/project-gauntlet}
whl=$whl_dir/capstone-5.0.7-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl
if [[ ! -f $whl ]]; then
    mkdir -p "$whl_dir"
    curl -sSfL -o "$whl" "https://files.pythonhosted.org/packages/py3/c/capstone/$(basename "$whl")"
fi
python3 -m zipfile -e "$whl" "$kit/pylib"
rm -rf "$kit/pylib/capstone/include" "$kit/pylib/capstone/lib/"*.a
cp "$kit/pylib"/capstone-*.dist-info/LICENSE* "$kit/licenses/capstone_LICENSE.TXT" 2>/dev/null || true

# SDL_GameControllerDB (zlib): mappings for controllers SDL has none for (pinned commit)
gcdb_rev=4e87e3c6c2574f49e1cdf8526b6f829b61e5ccb3
for f in gamecontrollerdb.txt LICENSE; do
    [[ -f $whl_dir/gcdb-$gcdb_rev-$f ]] || curl -sSfL -o "$whl_dir/gcdb-$gcdb_rev-$f" \
        "https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/$gcdb_rev/$f"
done
cp "$whl_dir/gcdb-$gcdb_rev-gamecontrollerdb.txt" "$kit/linux/gamecontrollerdb.txt"
cp "$whl_dir/gcdb-$gcdb_rev-LICENSE" "$kit/licenses/SDL_GameControllerDB_LICENSE.txt"

# licenses of the shipped libraries
dxvk_src=${GT_DXVK_SRC:-$(ls -d "$HOME"/.cache/ttt2-platform/shared-dxvk/source-*/ | head -1)}
cp "${GT_SDL3_SRC:-$HOME/ttt2-deps/sdl3/src}/LICENSE.txt" "$kit/licenses/SDL3_LICENSE.txt"
cp "$dxvk_src/LICENSE" "$kit/licenses/DXVK_Native_LICENSE.txt"
cp "$(ls -d "${GT_VKD3D_SRC:-$HOME/ttt2-deps/vkd3d}"/vkd3d-*/ | head -1)COPYING" "$kit/licenses/vkd3d_COPYING.txt"
cat > "$kit/licenses/README.txt" <<'EOF'
Third-party parts of the Linux package:
- SDL3 3.4 (zlib license, https://libsdl.org): lib/libSDL3.so.0
- DXVK Native 1.9.2b (zlib license, https://github.com/Joshua-Ashton/dxvk-native), built with the
  8gram Labs patches: lib/libdxvk_d3d11.so, lib/libdxvk_dxgi.so
- vkd3d-shader 1.17 (LGPL 2.1, source: https://dl.winehq.org/vkd3d/source/vkd3d-1.17.tar.xz), unmodified,
  dynamically linked: lib/libvkd3d-shader.so.1
- SDL_GameControllerDB (zlib license, https://github.com/mdqinc/SDL_GameControllerDB): gamecontrollerdb.txt
- capstone 5.0.7 (BSD, https://www.capstone-engine.org): pylib/capstone
- stb_image 2.30 (public domain / MIT, https://github.com/nothings/stb): in libgtrt.a
EOF

# nothing from the disc
bad=$(cd "$stage" && find . -type f \( -iname '*.xbe' -o -iname '*.iso' -o -iname '*.pdb' -o -iname '*.xbx' -o -iname '*.wmv' \
      -o -iname '*.ps2' -o -iname '*.wad' -o -iname '*pdb_match*' -o -path '*/lifted/*' -o -path './kit/game/*' \) | head)
[[ -z $bad ]] || { echo "FORBIDDEN files staged: $bad" >&2; exit 1; }

tar -C "$work" -czf "$out/$name.tar.gz" "$name"
echo "package: $out/$name.tar.gz ($(du -h "$out/$name.tar.gz" | cut -f1))"
