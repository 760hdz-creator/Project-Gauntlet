"""Route that picks a character on the select screen (k right presses from the default, or -k left presses) and
plays level A1. Roster going right from the default: 0 Sorceress, 1 Jester, 2..8 locked "?" (secret characters),
9 Warrior, 10 Valkyrie, 11 Wizard, 12 Archer, 13 Dwarf, 14 Knight (16 slots, wraps). Left: -2 Knight, -3 Dwarf,
-4 Archer, -5 Wizard, -6 Valkyrie, -7 Warrior.

usage: python -I tools/make_char.py <k> [out.txt]   (default out/input_char<k>.txt)
The select screen opens at ~t=38 (after the name screen's A at t=37); the stock title taps between 38 s and the
pick are dropped. In A1: stick up 3 s, A taps and a stick square for 40 s, exit cheat, level-exit screens.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_route import TITLE, MENU  # noqa: E402

BS = "\\"


def route(k):
    n, d = abs(k), "sr" if k >= 0 else "sl"
    pick = 38.6 + 0.4 * n
    ev = [e for e in TITLE.split(",") if not 38 <= float(e.split(":")[0]) <= pick + 1.0]
    ev += [f"{38.6 + 0.4 * i:.1f}:{d}:0.15" for i in range(n)] + [f"{pick:.1f}:a:0.2"]
    ev.sort(key=lambda e: float(e.split(":")[0]))
    ev += ["85:exit:8:100", f"@f:levels{BS}levelA1:1", "8:su:3"]
    ev += [f"{t / 10:.1f}:a:0.3" for t in range(110, 500, 6)]
    for t in range(12, 50, 8):
        ev += [f"{t}:sl:1", f"{t + 2}:su:1", f"{t + 4}:sr:1", f"{t + 6}:sd:1"]
    ev += ["52:exit:8", MENU]
    return ",".join(ev)


if __name__ == "__main__":
    k = int(sys.argv[1])
    with open(sys.argv[2] if len(sys.argv) > 2 else f"out/input_char{k}.txt", "w") as f:
        f.write(route(k))
