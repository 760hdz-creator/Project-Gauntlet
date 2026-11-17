"""Build a GT_INPUT route that plays one realm through with the exit cheat (see STATE.md).

usage: python -I tools/make_route.py <world hex>:<pads>[:<walk s>[:<chain>]][,...] [out.txt]
  e.g. make_route.py 1:6 out/input_realmA.txt -> pads 0100..0105 (pad 0101 loads levelA6: pad order is not the
  level number). The last pad is treated as the boss arena: A taps and a stick square for 260 s (run with GT_GOD=1
  GT_BOSSHP=20), then A taps after the hub (levelL1) reloads; the next realm's first pad follows 75 s later.
The boss arena starts with the stick held up for <walk> s (default 3). A boss stays inactive until the game's
player distance (critter +0x134, MaxPlayerDist 35F70) is within its range: the yeti (I5) needs ~30 s of walking.
<chain> = levels the last pad passes through before the boss level: E1's exit loads E2 directly (5:1:3:1).
"""
import sys

TITLE = ",".join(f"{t}:start,{t + 1}:a" for t in range(30, 67, 3))   # START+A from the title to the Tower hub
MENU = "@672F0:15,12:a,16:a,20:a,24:a,28:a"                             # level-exit screens back to the hub


def realm(world, levels, ev, walk=3, chain=0):
    letter = chr(ord("A") + world - 1)
    for k in range(1, levels + 1):
        ev.append(f"@f:levels\\level{letter}:1")   # pad order != level number (pad 0101 = levelA6)
        if k < levels:
            ev += ["40:exit:8", MENU, "@f:tower.ads:5", f"6:exit:8:{world:X}{k:02X}"]
            continue
        for n in range(chain):   # this level's exit loads the next level directly (no hub, no tower.ads)
            ev += ["40:exit:8", f"@f:levels\\level{letter}{k + n + 1}:20"]
        ev.append(f"34:su:{walk:g}")
        t = 37.0
        while t < 300:
            ev.append(f"{t:.1f}:a:0.3")
            t += 0.6
        for t in range(max(40, 40 + int(walk) - 3), 300, 8):
            ev += [f"{t}:sl:1", f"{t + 2}:su:1", f"{t + 4}:sr:1", f"{t + 6}:sd:1"]
        ev += ["@f:levelL1:1"] + [f"{t}:a" for t in range(10, 62, 4)]


def route(realms):
    ev = [TITLE]
    for i, (world, levels, walk, chain) in enumerate(realms):
        ev.append(f"{85 if i == 0 else 75}:exit:8:{world:X}00")
        realm(world, levels, ev, walk, chain)
    return ",".join(ev)


if __name__ == "__main__":
    r = route([(int(f[0], 16), int(f[1]), float(f[2]) if len(f) > 2 else 3, int(f[3]) if len(f) > 3 else 0)
               for f in (x.split(":") for x in sys.argv[1].split(","))])
    if len(sys.argv) > 2:
        open(sys.argv[2], "w").write(r)
    else:
        print(r)
