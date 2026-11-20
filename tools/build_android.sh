#!/usr/bin/env bash
# Android build (arm64-v8a), in two stages like the Linux package:
#   1. our side: the runtime (runtime/src + runtime/src/linux, NDK clang) -> <build>/libgtrt.a, plus the shared
#      libraries (SDL3, DXVK Native, vkd3d-shader, libc++_shared) and the APK project (SDL's android-project with
#      android/ over it: GauntletActivity, manifest, icon; gamecontrollerdb.txt as an asset);
#   2. with --lifted: the game code (lifted C) compiled and linked with libgtrt.a into libmain.so, then the APK
#      (Gradle, debug build: debuggable, so `adb shell run-as` can copy the game files in).
#
#   tools/build_android.sh <build dir> [--version X.Y.Z] [--lifted <dir with f_*.c>]
#
# The APK of stage 2 holds the game's code: it stays on this PC (never in dist/ or a release).
# Dependencies: the TTT2 Android port's tools/build_android_deps.sh output (GT_ANDROID_DEPS, default
# ~/.cache/ttt2-android: SDL 3.4.18, DXVK Native 1.9.2b + patches, vkd3d-shader 1.17), the NDK
# (GT_ANDROID_NDK, default ~/Android/android-ndk-r27c), the SDK (ANDROID_HOME, default ~/Android) and JDK 17.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=${1:?usage: build_android.sh <build dir> [--version X] [--lifted dir]}
shift
version=$(tr -d '\r\n' < "$root/VERSION") lifted=""
while [[ $# -gt 0 ]]; do
    case $1 in
    --version) version=$2; shift 2 ;;
    --lifted) lifted=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
ndk=${GT_ANDROID_NDK:-$HOME/Android/android-ndk-r27c}
sdk=${ANDROID_HOME:-$HOME/Android}
deps=${GT_ANDROID_DEPS:-$HOME/.cache/ttt2-android}
jobs=${GT_JOBS:-$(nproc)}
api=33
bin=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
cc=$bin/aarch64-linux-android$api-clang
[[ -x $cc ]] || { echo "No Android NDK at $ndk (GT_ANDROID_NDK)" >&2; exit 1; }
dxvk_src=$(cat "$deps/dxvk-source.txt")
sdl=$deps/sdl3/install
vkd3d=$deps/vkd3d/install

mkdir -p "$build/rt" "$build/lib" "$build/include"
src=$root/runtime/src
inc=(-I"$src/linux/include" -I"$dxvk_src/include/native/directx" -I"$dxvk_src/include/native/windows"
     -I"$vkd3d/include/vkd3d" -I"$sdl/include" -I"$root/recomp/include" -I"$src")
# plain char is unsigned on arm64 and signed on x86 (the game and the Windows build assume signed)
cflags=(-O2 -g -std=gnu11 -D_GNU_SOURCE -fsigned-char -fno-strict-aliasing -fPIC -Wno-everything
        -DGT_VERSION="\"$version\"")

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
"$bin/llvm-ar" rcs "$build/libgtrt.a" "${objs[@]}"
cp "$sdl/lib/libSDL3.so" "$deps/dxvk/build/src/d3d11/libdxvk_d3d11.so" "$deps/dxvk/build/src/dxgi/libdxvk_dxgi.so" \
   "$vkd3d/lib/libvkd3d-shader.so" "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" \
   "$build/lib/"
cp "$root/recomp/include/"*.h "$build/include/"

# APK project: SDL's android-project (Gradle wrapper, SDL's Java classes), then android/ over it
apk=$build/apk
rm -rf "$apk"
mkdir -p "$apk"
cp -r "$deps/SDL-3.4.18/android-project/." "$apk/"
rm -rf "$apk/app/jni" "$apk/app/src/main/res/mipmap-"*
cp -r "$root/android/." "$apk/"
mkdir -p "$apk/app/src/main/assets"
db=$(ls "${GT_CACHE:-$HOME/.cache/project-gauntlet}"/gcdb-*-gamecontrollerdb.txt 2>/dev/null | head -1)
[[ -n $db ]] && cp "$db" "$apk/app/src/main/assets/gamecontrollerdb.txt"
echo "runtime: $build/libgtrt.a"

# the Android build kit (no game code): what tools/gt_android.py turns into the game's APK on the player's PC
IFS=. read -r v1 v2 v3 <<<"$version"
export ANDROID_HOME="$sdk" ANDROID_SDK_ROOT="$sdk"
chmod +x "$apk/gradlew"
libs=$apk/app/libs/arm64-v8a
rm -rf "$libs"
mkdir -p "$libs"
cp "$build/lib/"*.so "$libs/"
"$apk/gradlew" --project-dir "$apk" --no-daemon -q -PgtVersionName="$version" \
    -PgtVersionCode=$((v1 * 10000 + v2 * 100 + v3)) assembleRelease
kitdir=$build/android-kit
rm -rf "$kitdir"
mkdir -p "$kitdir/include"
cp "$build/libgtrt.a" "$kitdir/"
cp "$root/recomp/include/"*.h "$kitdir/include/"
cp "$apk/app/build/outputs/apk/release/app-release.apk" "$kitdir/template.apk"
echo "kit: $kitdir"

[[ -n $lifted ]] || exit 0
mkdir -p "$build/lifted-obj"
lobjs=()
for f in "$lifted"/*.c; do
    o=$build/lifted-obj/$(basename "${f%.c}").o
    lobjs+=("$o")
    rm -f "$o"
    "$cc" -O2 -fsigned-char -fno-strict-aliasing -fPIC -w -I"$build/include" -c "$f" -o "$o" &
    while [[ $(jobs -r | wc -l) -ge $jobs ]]; do wait -n || true; done
done
wait || true
for o in "${lobjs[@]}"; do [[ -f $o ]] || { echo "compile failed: $o" >&2; exit 1; }; done
"$cc" -shared -o "$libs/libmain.so" "${lobjs[@]}" "$build/libgtrt.a" -L"$libs" -lSDL3 -ldxvk_d3d11 -ldxvk_dxgi \
    -lvkd3d-shader -landroid -llog -lm -Wl,-z,max-page-size=16384 -Wl,--no-undefined -Wl,-soname,libmain.so
"$apk/gradlew" --project-dir "$apk" --no-daemon -q -PgtVersionName="$version" \
    -PgtVersionCode=$((v1 * 10000 + v2 * 100 + v3)) assembleDebug
rm -f "$libs/libmain.so"
cp "$apk/app/build/outputs/apk/debug/app-debug.apk" "$build/gauntlet-$version-android-arm64-debug.apk"
echo "apk: $build/gauntlet-$version-android-arm64-debug.apk"
