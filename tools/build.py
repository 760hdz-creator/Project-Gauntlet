"""Dev build with llvm-mingw (tools/bootstrap.py; no Visual Studio). The player's build is tools/gt_build.py: same flags.

usage: python -I tools/build.py [lifted] [oracle] [runtime] [--jobs N]     (default: all three)
  lifted   compile out/lifted/*.c (run recomp/lift.py first: tools/relift.sh does everything)
  oracle   out/oracle.dll (lifted objects + runtime/oracle/oracle.c) for tools/oracle_test.py
  runtime  out/gauntlet.exe (console, debug info) and out/gauntlet.map for tools/symbolize.py
"""
import argparse
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bootstrap  # noqa: E402
import gt_build  # noqa: E402

ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "out")
INC = "-I" + os.path.join(ROOT, "recomp", "include")
SRC = os.path.join(ROOT, "runtime", "src")


def objs(folder):
    return sorted(glob.glob(os.path.join(OUT, folder, "*.o")))


def link(cc, args):
    code, out = gt_build.run([cc] + args)
    if code:
        gt_build.fail("link failed:\n" + out[-3000:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", nargs="*")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    what = a.what or ["lifted", "oracle", "runtime"]
    bootstrap.main()
    bin_dir = os.path.join(bootstrap.llvm_mingw_dir(), "bin")
    cc = os.path.join(bin_dir, "clang.exe")
    if "lifted" in what:
        for f in objs("lifted"):
            os.remove(f)
        gt_build.compile_all(a.jobs, cc, [(f, f[:-2] + ".o", gt_build.CFLAGS + [INC])
                                          for f in sorted(glob.glob(os.path.join(OUT, "lifted", "*.c")))])
    if "oracle" in what:
        obj = os.path.join(OUT, "oracle.o")
        gt_build.compile_all(1, cc, [(os.path.join(ROOT, "runtime", "oracle", "oracle.c"), obj, gt_build.CFLAGS + [INC])])
        link(cc, ["-shared", "-static", "-o", os.path.join(OUT, "oracle.dll"), obj] + objs("lifted"))
    if "runtime" in what:
        rt = os.path.join(OUT, "rt")
        os.makedirs(rt, exist_ok=True)
        srcs = sorted(glob.glob(os.path.join(SRC, "*.c")))
        rt_objs = [os.path.join(rt, os.path.basename(f)[:-2] + ".o") for f in srcs]
        flags = [f for f in gt_build.CFLAGS if f != "-w"] + ["-Wall", "-Wno-unused-function", "-Wno-missing-braces"]
        gt_build.compile_all(a.jobs, cc, [(f, o, flags + [INC, "-I" + SRC]) for f, o in zip(srcs, rt_objs)])
        exe = os.path.join(OUT, "gauntlet.exe")
        link(cc, ["-static", "-g", "-o", exe] + rt_objs + objs("lifted") + gt_build.LIBS)
        code, out = gt_build.run([os.path.join(bin_dir, "llvm-nm.exe"), "--numeric-sort", exe])
        with open(os.path.join(OUT, "gauntlet.map"), "w") as f:
            f.write(out)
    print("built: " + ", ".join(what))


if __name__ == "__main__":
    main()
