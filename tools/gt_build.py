"""Build Project Gauntlet on this PC from the player's own disc image (what the installer runs).

usage: python -I tools/gt_build.py --root <install dir> [--iso <image.iso>] [--jobs N]
  1. --iso: extract the XDVDFS image into <root>/game (skipped without --iso: update mode keeps the data)
  2. check game/Default.xbe is the supported USA release (SHA-256)
  3. tools/match_pdb.py (game/shell3D.pdb from the disc) -> <root>/build/pdb_match.tsv
  4. recomp/lift.py -> <root>/build/lifted/*.c
  5. clang-cl (LLVM) + MSVC/Windows SDK libraries (VS Build Tools, found with vswhere) -> <root>/gauntlet.exe
Progress lines start with "## ", a failure with "ERROR: " (the installer shows both). Logs go to stdout.
Nothing from the disc leaves the player's PC.
"""
import argparse
import concurrent.futures
import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)  # the build kit: recomp/, runtime/, tools/
XBE_SHA256 = "0982be1a51a045af36032dc98e3a3af37fcadae9f103d1443bd480700a4c6e76"
LIFT_FLAGS = ["/nologo", "/c", "/O2", "-Wno-unused-label", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
              "-Wno-parentheses-equality", "-Wno-everything"]
LIBS = ["libcmt.lib", "kernel32.lib", "winmm.lib", "user32.lib", "gdi32.lib", "d3d11.lib", "dxgi.lib", "dxguid.lib",
        "d3dcompiler.lib", "shlwapi.lib", "winhttp.lib", "shell32.lib", "advapi32.lib", "ole32.lib",
        "windowscodecs.lib"]


def step(msg):
    print("## " + msg, flush=True)


def fail(msg):
    print("ERROR: " + msg, flush=True)
    sys.exit(1)


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


def msvc_env():
    """Environment of vcvars64.bat (VS Build Tools) with LLVM in front of PATH."""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           r"Microsoft Visual Studio\Installer\vswhere.exe")
    if not os.path.exists(vswhere):
        fail("Visual Studio Build Tools are not installed (vswhere.exe missing). Run the installer's build tools step.")
    code, out = run([vswhere, "-latest", "-products", "*", "-requires",
                     "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"])
    vs = out.strip().splitlines()[0] if out.strip() else ""
    vcvars = os.path.join(vs, r"VC\Auxiliary\Build\vcvars64.bat")
    if not vs or not os.path.exists(vcvars):
        fail("No Visual Studio C++ build tools found (VC.Tools.x86.x64).")
    code, out = run(f'cmd /s /c ""{vcvars}" >nul && set"')
    if code:
        fail("vcvars64.bat failed:\n" + out[-2000:])
    env = {}
    for line in out.splitlines():
        k, sep, v = line.partition("=")
        if sep:
            env[k.upper()] = v
    llvm = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), r"LLVM\bin")
    if not os.path.exists(os.path.join(llvm, "clang-cl.exe")):
        fail("LLVM (clang-cl) is not installed in " + llvm)
    env["PATH"] = llvm + ";" + env.get("PATH", "")
    env["GT_LLVM"] = llvm
    return env


def compile_all(jobs, env, items):
    """items: (source, object, flags). Compiles in parallel; stops at the first failure."""
    def one(it):
        src, obj, flags = it
        return src, run([os.path.join(env["GT_LLVM"], "clang-cl.exe")] + flags + [src, "/Fo" + obj], env=env)
    done = 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for src, (code, out) in ex.map(one, items):
            done += 1
            if code:
                fail(f"compile failed: {os.path.basename(src)}\n{out[-3000:]}")
            print(f"compiled {os.path.basename(src)} ({done}/{len(items)})", flush=True)


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
    jobs = a.jobs or max(1, min(os.cpu_count() or 2, 8))

    if a.iso:
        step("Extracting the disc image")
        if os.path.isdir(game):
            shutil.rmtree(game)
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
    if os.path.isdir(lifted):
        shutil.rmtree(lifted)
    code, out = run(py + [os.path.join(KIT, "recomp", "lift.py"), xbe, lifted, "--seeds", tsv])
    print(out[-1500:], flush=True)
    if code:
        fail("code translation failed:\n" + out[-2000:])

    step("Finding the C compiler")
    env = msvc_env()
    inc = os.path.join(KIT, "recomp", "include")
    src_dir = os.path.join(KIT, "runtime", "src")
    rt_obj = os.path.join(build, "rt")
    os.makedirs(rt_obj, exist_ok=True)
    items = [(os.path.join(lifted, f), os.path.join(lifted, f[:-2] + ".obj"), LIFT_FLAGS + ["/I" + inc])
             for f in sorted(os.listdir(lifted)) if f.endswith(".c")]
    items += [(os.path.join(src_dir, f), os.path.join(rt_obj, f[:-2] + ".obj"),
               ["/nologo", "/c", "/O2", "/I" + inc, "/I" + src_dir, "-D_CRT_SECURE_NO_WARNINGS",
                f'-DGT_VERSION="{a.version}"'])
              for f in sorted(os.listdir(src_dir)) if f.endswith(".c")]
    step(f"Compiling {len(items)} files with {jobs} jobs (several minutes)")
    compile_all(jobs, env, items)

    step("Linking gauntlet.exe")
    objs = [it[1] for it in items]
    rsp = os.path.join(build, "link.rsp")
    with open(rsp, "w") as f:
        f.write("\n".join(f'"{o}"' for o in objs))
    exe = os.path.join(root, "gauntlet.exe")
    code, out = run([os.path.join(env["GT_LLVM"], "lld-link.exe"), "/nologo", "/OUT:" + exe + ".new", "/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup", "@" + rsp] + LIBS, env=env)
    if code:
        fail("link failed:\n" + out[-3000:])
    os.replace(exe + ".new", exe)
    with open(os.path.join(root, "build", "version.txt"), "w") as f:
        f.write(a.version + "\n")
    step("Done")


if __name__ == "__main__":
    main()
