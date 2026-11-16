"""Map shell3D.pdb procs (a slightly different build) onto Default.xbe .text.

Both builds link the same objects in the same order, so functions appear in the
same sequence with small drifts where code differs. For each PDB proc (sorted by
offset) we look for an XBE address, 16-byte aligned, near the running delta,
where linear decoding hits an instruction boundary at exactly start+size and the
last instruction ends the function. A delta change is accepted only if the
following procs confirm it.

Usage: python -I tools/match_pdb.py <default.xbe> <shell3D.pdb> <out.tsv>
Output columns: xbe_va size name module status(exact|moved|guess)
"""
import sys

import capstone

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from xbedis import Xbe  # noqa: E402
from pdb2 import Pdb2, procs  # noqa: E402

TEXT_VA = 0x11000
WINDOW = 0x6000
CONFIRM = 3

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
ENDERS = {"ret", "retf", "jmp", "int3", "hlt"}


def fits(text, start, size, cache={}):
    key = (start, size)
    if key in cache:
        return cache[key]
    ok = False
    if 0 <= start and start + size <= len(text):
        if start == 0 or text[start - 1] in (0xCC, 0x90, 0xC3) or text[start - 3] == 0xC2:
            pos, last = start, None
            for ins in md.disasm(text[start : start + size], start):
                pos = ins.address + ins.size
                last = ins
            if pos == start + size and last is not None:
                after = text[start + size] if start + size < len(text) else 0xCC
                ok = (last.mnemonic in ENDERS or last.mnemonic == "call") and (
                    after in (0xCC, 0x90) or (start + size) % 16 == 0
                )
    cache[key] = ok
    return ok


def main():
    xbe, pdb, out = Xbe(sys.argv[1]), Pdb2(sys.argv[2]), sys.argv[3]
    name, va, vsize, raw, rsize = next(s for s in xbe.sections if s[0] == ".text")
    text = xbe.read(va, rsize)
    plist = sorted((off, size, n, mod) for seg, off, size, n, mod, _ in procs(pdb) if seg == 1)
    # Drop duplicates (same off) keeping the first.
    seen, items = set(), []
    for p in plist:
        if p[0] not in seen:
            seen.add(p[0])
            items.append(p)

    delta = 0
    rows, stats = [], {"exact": 0, "moved": 0, "guess": 0}
    for i, (off, size, n, mod) in enumerate(items):
        if size < 8:
            rows.append((off + delta, size, n, mod, "guess"))
            stats["guess"] += 1
            continue
        if fits(text, off + delta, size):
            rows.append((off + delta, size, n, mod, "exact"))
            stats["exact"] += 1
            continue
        best = None
        for step in range(16, WINDOW, 16):
            for d in (delta - step, delta + step):
                if not fits(text, off + d, size):
                    continue
                nxt = [p for p in items[i + 1 : i + 1 + 4 * CONFIRM] if p[1] >= 8][:CONFIRM]
                if all(fits(text, o + d, s) for o, s, _, _ in nxt):
                    best = d
                    break
            if best is not None:
                break
        if best is None:
            rows.append((off + delta, size, n, mod, "guess"))
            stats["guess"] += 1
        else:
            delta = best
            rows.append((off + delta, size, n, mod, "moved"))
            stats["moved"] += 1

    with open(out, "w", encoding="utf-8") as f:
        for o, size, n, mod, st in rows:
            f.write(f"{TEXT_VA + o:08X}\t{size:X}\t{n}\t{mod}\t{st}\n")
    print(stats, f"final delta {delta:#x}")


if __name__ == "__main__":
    main()
