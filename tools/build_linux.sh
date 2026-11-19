#!/usr/bin/env bash
# Linux build: the runtime library (our code: runtime/src + runtime/src/linux) and its shared libraries,
# then, with --lifted, the game itself.
#
#   tools/build_linux.sh <build dir> [--version X.Y.Z] [--lifted <dir with f_*.c> [--exe <path>]]
#
# <build dir>/libgtrt.a      the runtime (prebuilt into the Linux package; players only compile the lifted code)
# <build dir>/lib/           SDL3, DXVK Native (D3D11/DXGI on Vulkan), vkd3d-shader (D3DCompile): the game's rpath
# <build dir>/include/       recomp/include copy (what the lifted code needs)
#
# Dependencies (built once by the TTT2 Linux port's tools/build_linux.sh; override the paths if elsewhere):
#   GT_SDL3=<prefix with lib/libSDL3.so.0 and include/SDL3>      default ~/ttt2-deps/sdl3/install
#   GT_DXVK=<prefix with lib/x86_64-linux-gnu/libdxvk_d3d11.so>  default ~/ttt2-deps/dxvk/install
#   GT_DXVK_SRC=<DXVK Native source, for include/native>         default ~/.cache/ttt2-platform/shared-dxvk/source-*
#   GT_VKD3D=<prefix with lib/libvkd3d-shader.so.1>              default ~/ttt2-deps/vkd3d/install
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=${1:?usage: build_linux.sh <build dir> [--version X] [--lifted dir [--exe path]]}
shift
version="" lifted="" exe=""
while [[ $# -gt 0 ]]; do
    case $1 in
    --version) version=$2; shift 2 ;;
    --lifted) lifted=$2; shift 2 ;;
    --exe) exe=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
sdl=${GT_SDL3:-$HOME/ttt2-deps/sdl3/install}
dxvk=${GT_DXVK:-$HOME/ttt2-deps/dxvk/install}
dxvk_src=${GT_DXVK_SRC:-$(ls -d "$HOME"/.cache/ttt2-platform/shared-dxvk/source-*/ 2>/dev/null | head -1)}
vkd3d=${GT_VKD3D:-$HOME/ttt2-deps/vkd3d/install}
cc=${CC:-clang}
jobs=${GT_JOBS:-$(nproc)}
dxvk_lib=$dxvk/lib/x86_64-linux-gnu
[[ -f $dxvk_lib/libdxvk_d3d11.so ]] || dxvk_lib=$dxvk/lib

mkdir -p "$build/rt" "$build/lib" "$build/include"
src=$root/runtime/src
inc=(-I"$src/linux/include" -I"$dxvk_src/include/native/directx" -I"$dxvk_src/include/native/windows"
     -I"$vkd3d/include/vkd3d" -I"$sdl/include" -I"$root/recomp/include" -I"$src")
# clang-cl (the Windows build) does not assume strict aliasing; neither do we.
cflags=(-O2 -g -std=gnu11 -D_GNU_SOURCE -fno-strict-aliasing -fPIC -Wno-everything)
[[ -n $version ]] && cflags+=(-DGT_VERSION="\"$version\"")

objs=()
for f in "$src"/*.c "$src"/linux/*.c; do
    o=$build/rt/$(basename "${f%.c}").o
    objs+=("$o")
    rm -f "$o"
    "$cc" "${cflags[@]}" "${inc[@]}" -c "$f" -o "$o" &
    while [[ $(jobs -r | wc -l) -ge $jobs ]]; do wait -n || true; done
done
wait || true
for o in "${objs[@]}"; do [[ -f $o ]] || { echo "compile failed: $o" >&2; exit 1; }; done
rm -f "$build/libgtrt.a"
ar rcs "$build/libgtrt.a" "${objs[@]}"

cp -L "$sdl/lib/libSDL3.so.0" "$dxvk_lib/libdxvk_d3d11.so" "$dxvk_lib/libdxvk_dxgi.so" "$vkd3d/lib/libvkd3d-shader.so.1" "$build/lib/"
cp "$root/recomp/include/"*.h "$build/include/"
echo "runtime: $build/libgtrt.a"

if [[ -n $lifted ]]; then
    exe=${exe:-$build/gauntlet}
    mkdir -p "$build/lifted-obj"
    lobjs=()
    for f in "$lifted"/*.c; do
        o=$build/lifted-obj/$(basename "${f%.c}").o
        lobjs+=("$o")
        rm -f "$o"
        "$cc" -O2 -fno-strict-aliasing -w -I"$build/include" -c "$f" -o "$o" &
        while [[ $(jobs -r | wc -l) -ge $jobs ]]; do wait -n || true; done
    done
    wait || true
    for o in "${lobjs[@]}"; do [[ -f $o ]] || { echo "compile failed: $o" >&2; exit 1; }; done
    "$cc" -o "$exe" "${lobjs[@]}" "$build/libgtrt.a" "$build/lib/libSDL3.so.0" "$build/lib/libdxvk_d3d11.so" \
        "$build/lib/libdxvk_dxgi.so" "$build/lib/libvkd3d-shader.so.1" -lm -lpthread -ldl -Wl,--disable-new-dtags -Wl,-rpath,'$ORIGIN/lib' \
        -Wl,-rpath,"$build/lib"
    echo "game: $exe"
fi
