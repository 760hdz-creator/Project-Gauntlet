"""Route through all nine secret levels (S1..S9). For each: the realm's earlier hub pads (exit cheat), the parent
level, its 0x0Cxx secret exit, 60 s in the secret level, back to the parent level, exit to the hub.

usage: python -I tools/make_secrets.py [out.txt]   (default out/input_secrets.txt)
Run with GT_GOD=1 GT_CRYSTALS=1 GT_SHARDS=7FE GT_RUNES=FFFF (H2 needs the world-8 shards).
Hub pads past a realm's first are locked (c4=8000) at a new game, so the earlier pads are played first.
The hub is anchored on its level files: tower.ads is not reopened after every level.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_route import TITLE, MENU  # noqa: E402

BS = "\\"
# (world, pad index of the parent level, parent level, secret exit); exit 0x0C0n loads levelS(n+1)
SECRETS = [(0x9, 0, "I1", 0xC05), (0xB, 0, "K2", 0xC08), (0x1, 1, "A6", 0xC02), (0x2, 1, "B2", 0xC03),
           (0x3, 1, "C2", 0xC00), (0x7, 1, "G2", 0xC07), (0x8, 1, "H2", 0xC04), (0x4, 2, "D3", 0xC01),
           (0xA, 2, "J3", 0xC06)]


def route():
    ev = [TITLE]
    first = True
    hub = f"@f:levels{BS}levelL1:10"
    for w, k, lv, ex in SECRETS:
        for j in range(k + 1):
            ev += [f"{85 if first else 20}:exit:8:{w:X}{j:02X}", f"@f:levels{BS}level{lv[0]}:1"]
            first = False
            if j < k:
                ev += ["40:exit:8", MENU, hub]
        ev += [f"30:exit:8:{ex:X}", f"@f:levels{BS}levelS:1", "60:exit:8",
               f"@f:levels{BS}level{lv}:20", "30:exit:8", MENU, hub]
    return ",".join(ev)


if __name__ == "__main__":
    open(sys.argv[1] if len(sys.argv) > 1 else "out/input_secrets.txt", "w").write(route())
