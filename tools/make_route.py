"""Build a GT_INPUT route that plays one realm through with the exit cheat (see STATE.md).

usage: python -I tools/make_route.py <world hex>:<pads>[,<world hex>:<pads>...] [out.txt]
  e.g. make_route.py 1:6 out/input_realmA.txt -> pads 0100..0105 (pad 0101 loads levelA6: pad order is not the
  level number). The last pad is treated as the boss arena: A taps and a stick square for 260 s (run with GT_GOD=1
  GT_BOSSHP=20), then A taps after the hub (levelL1) reloads; the next realm's first pad follows 75 s later.
"""
import sys

TITLE = ",".join(f"{t}:start,{t + 1}:a" for t in range(30, 67, 3))   # START+A from the title to the Tower hub
MENU = "@672F0:15,12:a,16:a,20:a,24:a,28:a"                             # level-exit screens back to the hub


def realm(world, levels, ev):
    letter = chr(ord("A") + world - 1)
    for k in range(1, levels + 1):
        ev.append(f"@f:levels\\level{letter}:1")   # pad order != level number (pad 0101 = levelA6)
        if k < levels:
            ev += ["40:exit:8", MENU, "@f:tower.ads:5", f"6:exit:8:{world:X}{k:02X}"]
            continue
        ev.append("34:su:3")
        t = 37.0
        while t < 300:
            ev.append(f"{t:.1f}:a:0.3")
            t += 0.6
        for t in range(40, 300, 8):
            ev += [f"{t}:sl:1", f"{t + 2}:su:1", f"{t + 4}:sr:1", f"{t + 6}:sd:1"]
        ev += ["@f:levelL1:1"] + [f"{t}:a" for t in range(10, 62, 4)]


def route(realms):
    ev = [TITLE]
    for i, (world, levels) in enumerate(realms):
        ev.append(f"{85 if i == 0 else 75}:exit:8:{world:X}00")
        realm(world, levels, ev)
    return ",".join(ev)


if __name__ == "__main__":
    r = route([(int(w, 16), int(n)) for w, n in (x.split(":") for x in sys.argv[1].split(","))])
    if len(sys.argv) > 2:
        open(sys.argv[2], "w").write(r)
    else:
        print(r)
