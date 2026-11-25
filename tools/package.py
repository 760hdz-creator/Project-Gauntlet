"""Stage the installer payload: dist/payload.zip (embedded in ProjectGauntlet-Setup.exe by installer/build.py).

usage: python -I tools/package.py
Holds only our build kit (git-tracked files from SHIP) + third party: the embedded CPython (python.org) and the
capstone Python package (BSD, from this PC's site-packages). No game code or data: the player's PC lifts and builds
the game from their own disc (tools/gt_build.py). Aborts if anything matching FORBIDDEN is staged.
"""
import fnmatch
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

import capstone

ROOT = Path(__file__).resolve().parent.parent
PY_VER = "3.15.0"
PY_URL = f"https://www.python.org/ftp/python/{PY_VER}/python-{PY_VER}-embed-amd64.zip"
SDL_VER = "3.4.18"   # same as the Linux package
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VER}/SDL3-{SDL_VER}-win32-x64.zip"
GCDB_REV = "4e87e3c6c2574f49e1cdf8526b6f829b61e5ccb3"
GCDB_URL = f"https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/{GCDB_REV}/"
SHIP = ["VERSION", "README.md", "recomp/lift.py", "recomp/hle.txt", "recomp/hooks.txt", "recomp/seeds_extra.txt",
        "recomp/include/*", "runtime/src/*", "tools/gt_build.py", "tools/extract_xiso.py", "tools/match_pdb.py",
        "tools/pdb2.py", "tools/xbedis.py", "tools/gt_android.py", "tools/bootstrap.py"]
# Android kit (tools/build_android.sh in WSL: android-kit/ = libgtrt.a, include/, template.apk, licenses/; no game code)
ANDROID_KIT = Path(__import__("os").environ.get("GT_ANDROID_KIT", Path(__file__).resolve().parent.parent / "dist" / "_cache" / "android-kit"))
FORBIDDEN = ["*.xbe", "*.iso", "*.pdb", "*.xbx", "*.wmv", "game/*", "*/lifted/*", "*pdb_match*", "*.ps2", "*.wad",
             "toolchain/*"]


def main():
    out = ROOT / "dist" / "payload"
    if out.exists():
        shutil.rmtree(out)
    tracked = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
    files = [f for f in tracked if any(fnmatch.fnmatch(f, p) for p in SHIP)]
    for f in files:
        (out / f).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / f, out / f)
    cache = ROOT / "dist" / "_cache" / Path(PY_URL).name
    if not cache.exists():
        nexus = Path(r"G:\Project M\dist\_cache") / cache.name
        cache.parent.mkdir(parents=True, exist_ok=True)
        if nexus.exists():
            shutil.copy2(nexus, cache)
        else:
            print("downloading " + PY_URL)
            urllib.request.urlretrieve(PY_URL, cache)
    with zipfile.ZipFile(cache) as z:
        z.extractall(out / "python")
    cs = Path(capstone.__file__).parent
    shutil.copytree(cs, out / "python" / "capstone", ignore=shutil.ignore_patterns("__pycache__", "include", "*.a"))
    for lic in list(cs.parent.glob("capstone-*.dist-info/LICENSE*")) + list(cs.parent.glob("capstone-*.dist-info/licenses/*")):
        (out / "licenses").mkdir(exist_ok=True)
        shutil.copy2(lic, out / "licenses" / ("capstone_" + lic.name))
    (out / "licenses").mkdir(exist_ok=True)
    shutil.copy2(out / "python" / "LICENSE.txt", out / "licenses" / "python_LICENSE.txt")
    # SDL3.dll (official release, zlib) + SDL_GameControllerDB (zlib, pinned commit): every controller SDL knows
    # (runtime/src/pad_sdl.c); gt_build.py copies both next to gauntlet.exe
    (out / "sdl3").mkdir()
    for url, name in [(SDL_URL, Path(SDL_URL).name)] + [(GCDB_URL + f, "gcdb-" + GCDB_REV + "-" + f)
                                                        for f in ("gamecontrollerdb.txt", "LICENSE")]:
        if not (ROOT / "dist" / "_cache" / name).exists():
            print("downloading " + url)
            urllib.request.urlretrieve(url, ROOT / "dist" / "_cache" / name)
    with zipfile.ZipFile(ROOT / "dist" / "_cache" / Path(SDL_URL).name) as z:
        (out / "sdl3" / "SDL3.dll").write_bytes(z.read("SDL3.dll"))
        lic = [n for n in z.namelist() if n.upper().startswith("LICENSE")]
        if lic:
            (out / "licenses" / "SDL3_LICENSE.txt").write_bytes(z.read(lic[0]))
    shutil.copy2(ROOT / "dist" / "_cache" / ("gcdb-" + GCDB_REV + "-gamecontrollerdb.txt"),
                 out / "sdl3" / "gamecontrollerdb.txt")
    shutil.copy2(ROOT / "dist" / "_cache" / ("gcdb-" + GCDB_REV + "-LICENSE"),
                 out / "licenses" / "SDL_GameControllerDB_LICENSE.txt")
    if not (ANDROID_KIT / "template.apk").exists():
        sys.exit(f"Android kit missing at {ANDROID_KIT} (tools/build_android.sh <dir> in WSL, then copy <dir>/android-kit)")
    shutil.copytree(ANDROID_KIT, out / "android", ignore=shutil.ignore_patterns("licenses"))
    for lic in (ANDROID_KIT / "licenses").glob("*"):
        shutil.copy2(lic, out / "licenses" / ("android_" + lic.name))
    with zipfile.ZipFile(ANDROID_KIT / "template.apk") as z:   # the template must hold no game code
        if any(n.startswith(("assets/game/", "assets/game.lst")) or n.endswith("libmain.so") for n in z.namelist()):
            sys.exit("android template.apk holds game code")
    bad = [p for p in out.rglob("*") if p.is_file() and
           any(fnmatch.fnmatch(p.relative_to(out).as_posix().lower(), f.lower()) for f in FORBIDDEN)]
    if bad:
        sys.exit("FORBIDDEN files staged: " + ", ".join(map(str, bad[:10])))
    zpath = ROOT / "dist" / "payload.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(out.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(out).as_posix())
    print(f"staged {len(files)} tracked files + python {PY_VER} + capstone {capstone.__version__} -> {zpath} "
          f"({zpath.stat().st_size / 2**20:.1f} MB)")


if __name__ == "__main__":
    main()
