#!/bin/sh
# Re-lift, rebuild lifted objects and the oracle DLL.
set -e
cd "$(dirname "$0")/.."
# Merge runtime misses into the lifter's extra seeds.
if [ -f out/missed_targets.txt ]; then
  sort -u out/missed_targets.txt | while read -r va; do grep -qix "$va" recomp/seeds_extra.txt || echo "$va" >> recomp/seeds_extra.txt; done
  rm -f out/missed_targets.txt
fi
python -I recomp/lift.py game/Default.xbe out/lifted --seeds assets/pdb_match.tsv | tail -3
rm -f out/lifted/*.obj out/lifted/build_errors.txt
cmd //c "tools\build_lifted.bat" 2>&1 | grep FAIL || true
grep error out/lifted/build_errors.txt | head -5 || true
cmd //c "tools\build_oracle.bat" 2>&1 | grep -iE "error|undefined" || true
cmd //c "tools\build_runtime.bat" 2>&1 | grep -iE " error|undefined" || true
