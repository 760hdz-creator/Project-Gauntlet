"""Build Project Gauntlet on this PC from the player's own disc image (what the installer runs).

usage: python -I tools/gt_build.py --root <install dir> [--iso <image.iso>] [--jobs N]
       ... --progress [--game <image or extracted folder>]   (a launcher's setup step: see class Progress)
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
WRONG = "This is another game or version. Project Gauntlet needs Gauntlet: Dark Legacy for the original Xbox, USA."
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


class Progress:
    """--progress: a launcher runs the build without a window. stdout gets "progress 0.4 Text" lines and, last, what went
    wrong; everything else goes to <root>/build/setup.log."""
    AT = {"Extracting": .02, "Copying": .02, "Checking": .25, "Getting": .42, "downloading": .43, "extracting": .5,
          "Matching": .26, "Translating": .34, "Finding": .52, "Compiling": .54, "Linking": .95, "Done": 1}

    def __init__(self, log):
        self.out, self.buf, self.err = sys.stdout, "", False
        os.makedirs(os.path.dirname(log), exist_ok=True)
        self.log = open(log, "w", encoding="utf-8", buffering=1)

    def write(self, s):
        self.buf += s
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            self.log.write(line + "\n")
            if line.startswith("## "):
                self.say(f"progress {self.AT.get(line[3:].split(' ')[0], .3)} {line[3:]}")
            elif line.startswith("compiled ") and line.endswith(")"):
                a, b = line[line.rindex("(") + 1:-1].split("/")
                if int(b) > 2:
                    self.say(f"progress {.54 + .4 * int(a) / int(b):.3f} Compiling ({a} of {b})")
            elif line.startswith("ERROR: ") or self.err:
                self.err = True
                self.say(line.replace("ERROR: ", "", 1))
        return len(s)

    def say(self, line):
        self.out.write(line + "\n")
        self.out.flush()

    def flush(self):
        self.log.flush()

    def isatty(self):
        return False


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
    ap.add_argument("--game", help="like --iso; also a folder with the extracted disc (Default.xbe)")
    ap.add_argument("--progress", action="store_true")
    ap.add_argument("--version", default="0.0.0")
    ap.add_argument("--jobs", type=int, default=0)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    game, build = os.path.join(root, "game"), os.path.join(root, "build")
    ready = os.path.join(build, "version.txt")   # written once the build has finished
    disc_ok = os.path.join(build, "disc.txt")    # ... once the whole disc is in <root>/game
    if a.progress:
        sys.stdout = Progress(os.path.join(build, "setup.log"))
        exe = os.path.join(root, "gauntlet.exe" if os.name == "nt" else "gauntlet")
        if all(map(os.path.exists, (ready, disc_ok, exe, os.path.join(game, "Default.xbe")))):
            with open(ready) as f:
                if f.read().strip() == a.version:
                    sys.stdout.say("progress 1 Already built")
                    return
    if os.path.exists(ready):
        os.remove(ready)
    if a.game and a.progress and os.path.exists(disc_ok):
        a.game = None   # an update: the disc of the first setup is still there
    py = [sys.executable, "-I"]
    if os.name != "nt":   # Linux: the kit's capstone (pylib/) goes on sys.path; -I ignores PYTHONPATH
        py.append(os.path.join(HERE, "pyrun.py"))
    jobs = a.jobs or max(1, min(os.cpu_count() or 2, 8))

    if a.game and os.path.isdir(a.game):
        src = os.path.abspath(a.game)
        if not all(os.path.exists(os.path.join(src, f)) for f in ("Default.xbe", "shell3D.pdb")):
            fail("Default.xbe and shell3D.pdb are not in that folder. Choose the extracted disc of Gauntlet: Dark Legacy (Xbox, USA).")
        if sha256(os.path.join(src, "Default.xbe")) != XBE_SHA256:
            fail(WRONG)
        if src != os.path.abspath(game):
            step("Copying the game folder")
            if os.path.exists(disc_ok):
                os.remove(disc_ok)
            rmtree(game)
            shutil.copytree(src, game)
    elif a.game:
        a.iso = a.game
    if a.iso:
        if os.path.exists(disc_ok):
            os.remove(disc_ok)
        step("Extracting the disc image")
        rmtree(game)
        code, out = run(py + [os.path.join(HERE, "extract_xiso.py"), a.iso, game])
        print(out[-2000:], flush=True)
        if code:
            fail("Could not read the disc image. Use an Xbox (XISO or redump) image of Gauntlet: Dark Legacy (USA).")
    xbe = os.path.join(game, "Default.xbe")
    if a.progress and not os.path.exists(xbe):
        fail("The game's files are not here any more. Choose your disc image of Gauntlet: Dark Legacy (Xbox, USA) and set it up again.")
    if not os.path.exists(xbe):
        fail("game\\Default.xbe is missing: run a full install with your disc image.")
    step("Checking Default.xbe")
    if sha256(xbe) != XBE_SHA256:
        fail(WRONG)
    os.makedirs(build, exist_ok=True)
    if a.iso or a.game:
        with open(disc_ok, "w") as f:
            f.write(XBE_SHA256 + "\n")

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
