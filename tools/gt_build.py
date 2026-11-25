"""Build Project Gauntlet on this PC from the player's own disc image (what the installer runs).

usage: python -I tools/gt_build.py --root <install dir> [--iso <image.iso>] [--jobs N]
  1. --iso: extract the XDVDFS image into <root>/game (skipped without --iso: update mode keeps the data)
  2. check game/Default.xbe is the supported USA release (SHA-256)
  3. tools/match_pdb.py (game/shell3D.pdb from the disc) -> <root>/build/pdb_match.tsv
  4. recomp/lift.py -> <root>/build/lifted/*.c
  5. Windows: llvm-mingw (tools/bootstrap.py: pinned, SHA-256 checked, downloaded once to <root>/toolchain, ~190 MB)
     compiles the lifted code and the runtime -> <root>/gauntlet.exe
     Linux: clang (or gcc) compiles the lifted code and links it with the prebuilt runtime from the Linux package
     (linux/libgtrt.a + linux/lib/*.so: SDL3, DXVK Native, vkd3d-shader) -> <root>/gauntlet, rpath $ORIGIN/lib
Progress lines start with "## ", a failure with "ERROR: " (the installer shows both). Logs go to stdout.
Nothing from the disc leaves the player's PC.
"""
import argparse
import concurrent.futures
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)  # the build kit: recomp/, runtime/, tools/
XBE_SHA256 = "0982be1a51a045af36032dc98e3a3af37fcadae9f103d1443bd480700a4c6e76"
# Lifted code and runtime type-pun guest memory and rely on x86 integer/float semantics: no strict aliasing, signed
# overflow wraps, no FMA fusion (float results must match the guest bit for bit).
CFLAGS = ["-O2", "-g", "-w", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off", "-fno-math-errno",
          "-D_CRT_SECURE_NO_WARNINGS"]
LIBS = ["-l" + lib for lib in ("winmm", "user32", "gdi32", "d3d11", "dxgi", "dxguid", "d3dcompiler", "shlwapi", "winhttp",
                               "shell32", "advapi32", "ole32", "windowscodecs")]


def step(msg):
    print("## " + msg, flush=True)


def fail(msg):
    print("ERROR: " + msg, flush=True)
    sys.exit(1)


def retry(what, f):
    """f(), tried for 20 s: OneDrive, virus scanners and indexers hold files for a moment."""
    for i in range(40):
        try:
            return f()
        except PermissionError as e:
            if i == 39:
                fail(f"{what}: {e.strerror or e}: {e.filename}")
            time.sleep(0.5)


def rmtree(path):
    """Delete a folder. OneDrive marks its folders read-only, and Windows does not remove a read-only folder."""
    def once():
        if not os.path.isdir(path):
            return
        for d, dirs, files in os.walk(path):
            for p in [d] + [os.path.join(d, f) for f in files]:
                if not os.path.islink(p) and not os.stat(p).st_mode & stat.S_IWRITE:
                    os.chmod(p, os.stat(p).st_mode | stat.S_IWRITE)
        shutil.rmtree(path)
    retry("Could not delete an old folder (close programs that use it and try again)", once)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run(cmd, env=None, cwd=None):
    r = subprocess.run(cmd, env=env, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, errors="replace")
    return r.returncode, r.stdout


def linux_cc():
    """clang preferred (what the Windows build uses), gcc as fallback; CC overrides."""
    for cc in [os.environ.get("CC"), "clang", "gcc", "cc"]:
        if cc and shutil.which(cc):
            return shutil.which(cc)
    fail("No C compiler found. Install clang (or gcc) with your package manager, e.g. sudo apt install clang")


def compile_all(jobs, cc, items):
    def one(it):
        src, obj, flags = it
        return src, run([cc] + flags + ["-c", src, "-o", obj])
    done = 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for src, (code, out) in ex.map(one, items):
            done += 1
            if code:
                fail(f"compile failed: {os.path.basename(src)}\n{out[-3000:]}")
            print(f"compiled {os.path.basename(src)} ({done}/{len(items)})", flush=True)


def build_linux(root, build, lifted, jobs):
    rt = os.path.join(KIT, "linux")
    if not os.path.exists(os.path.join(rt, "libgtrt.a")):
        fail("linux/libgtrt.a is missing from the build kit (use the Linux package).")
    step("Finding the C compiler")
    cc = linux_cc()
    print("compiler: " + cc, flush=True)
    items = [(os.path.join(lifted, f), os.path.join(lifted, f[:-2] + ".o"),
              ["-O2", "-fno-strict-aliasing", "-w", "-I" + os.path.join(rt, "include")])
             for f in sorted(os.listdir(lifted)) if f.endswith(".c")]
    step(f"Compiling {len(items)} files with {jobs} jobs (several minutes)")
    compile_all(jobs, cc, items)
    step("Linking gauntlet")
    lib = os.path.join(root, "lib")
    rmtree(lib)
    shutil.copytree(os.path.join(rt, "lib"), lib)
    shutil.copy2(os.path.join(rt, "gamecontrollerdb.txt"), root)   # controller mappings (pad_sdl.c)
    libs = [os.path.join(lib, f) for f in sorted(os.listdir(lib)) if ".so" in f]
    exe = os.path.join(root, "gauntlet")
    code, out = run([cc, "-o", exe + ".new"] + [it[1] for it in items] + [os.path.join(rt, "libgtrt.a")] + libs +
                    ["-lm", "-lpthread", "-ldl", "-Wl,--disable-new-dtags", "-Wl,-rpath,$ORIGIN/lib"])
    if code:
        fail("link failed:\n" + out[-3000:])
    os.replace(exe + ".new", exe)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--iso")
    ap.add_argument("--version", default="0.0.0")
    ap.add_argument("--jobs", type=int, default=0)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    game, build = os.path.join(root, "game"), os.path.join(root, "build")
    py = [sys.executable, "-I"]
    if os.name != "nt":   # Linux: the kit's capstone (pylib/) goes on sys.path; -I ignores PYTHONPATH
        py.append(os.path.join(HERE, "pyrun.py"))
    jobs = a.jobs or max(1, min(os.cpu_count() or 2, 8))

    if a.iso:
        step("Extracting the disc image")
        rmtree(game)
        code, out = run(py + [os.path.join(HERE, "extract_xiso.py"), a.iso, game])
        print(out[-2000:], flush=True)
        if code:
            fail("Could not read the disc image. Use an Xbox (XISO or redump) image of Gauntlet: Dark Legacy (USA).")
    xbe = os.path.join(game, "Default.xbe")
    if not os.path.exists(xbe):
        fail("game\\Default.xbe is missing: run a full install with your disc image.")
    step("Checking Default.xbe")
    if sha256(xbe) != XBE_SHA256:
        fail("Default.xbe is not the supported version (Gauntlet: Dark Legacy, Xbox, USA).")

    os.makedirs(build, exist_ok=True)
    tsv = os.path.join(build, "pdb_match.tsv")
    step("Matching symbols (about 1 minute)")
    code, out = run(py + [os.path.join(HERE, "match_pdb.py"), xbe, os.path.join(game, "shell3D.pdb"), tsv])
    if code or not os.path.exists(tsv):
        fail("symbol matching failed:\n" + out[-2000:])

    lifted = os.path.join(build, "lifted")
    step("Translating the game code (a few minutes)")
    rmtree(lifted)
    code, out = run(py + [os.path.join(KIT, "recomp", "lift.py"), xbe, lifted, "--seeds", tsv])
    print(out[-1500:], flush=True)
    if code:
        fail("code translation failed:\n" + out[-2000:])

    if os.name != "nt":
        build_linux(root, build, lifted, jobs)
        with open(os.path.join(root, "build", "version.txt"), "w") as f:
            f.write(a.version + "\n")
        step("Done")
        return

    os.environ["GT_TOOLCHAIN_DIR"] = os.path.join(root, "toolchain")   # read when bootstrap.py is imported
    sys.path.insert(0, HERE)
    import bootstrap  # noqa: E402
    step("Getting the compiler (llvm-mingw, first install only: ~190 MB)")
    bootstrap.main()
    cc = os.path.join(bootstrap.llvm_mingw_dir(), "bin", "clang.exe")
    inc = os.path.join(KIT, "recomp", "include")
    src_dir = os.path.join(KIT, "runtime", "src")
    rt_obj = os.path.join(build, "rt")
    rmtree(rt_obj)
    os.makedirs(rt_obj)
    items = [(os.path.join(lifted, f), os.path.join(lifted, f[:-2] + ".o"), CFLAGS + ["-I" + inc])
             for f in sorted(os.listdir(lifted)) if f.endswith(".c")]
    items += [(os.path.join(src_dir, f), os.path.join(rt_obj, f[:-2] + ".o"),
               CFLAGS + ["-I" + inc, "-I" + src_dir, f'-DGT_VERSION="{a.version}"'])
              for f in sorted(os.listdir(src_dir)) if f.endswith(".c")]
    step(f"Compiling {len(items)} files with {jobs} jobs (1 to 3 minutes)")
    compile_all(jobs, cc, items)

    step("Linking gauntlet.exe")
    exe = os.path.join(root, "gauntlet.exe")
    # -static: only system DLLs are imported; -mwindows: no console window
    code, out = run([cc, "-static", "-g", "-mwindows", "-o", exe + ".new"] + [it[1] for it in items] + LIBS)
    if code:
        fail("link failed:\n" + out[-3000:])
    retry("Could not replace gauntlet.exe. Close the game and try again", lambda: os.replace(exe + ".new", exe))
    # SDL3.dll + gamecontrollerdb.txt: every controller SDL knows (pad_sdl.c; without them, XInput pads only)
    for f in ("SDL3.dll", "gamecontrollerdb.txt"):
        src = os.path.join(KIT, "sdl3", f)
        if os.path.exists(src):
            retry("Could not replace " + f, lambda: shutil.copy2(src, os.path.join(root, f)))
    with open(os.path.join(root, "build", "version.txt"), "w") as f:
        f.write(a.version + "\n")
    step("Done")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:   # the installer shows the ERROR line; the log keeps the details
        traceback.print_exc()
        fail(f"{type(e).__name__}: {e}")
