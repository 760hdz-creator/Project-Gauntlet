# Project Gauntlet - State

Static recompilation of **Gauntlet: Dark Legacy (Xbox, USA)** to PC.

## Inputs (verified 2026-11-15)
- `Gauntlet - Dark Legacy (USA).xiso.iso` (952,434,688 bytes, trimmed XISO, partition at 0) in repo root, git-ignored.
- Extracted with `python -I tools/extract_xiso.py <iso> game` -> `game/` (2480 files, 909 MB, git-ignored).
  Assets keep PS2 names (`*.PS2`): the Xbox version is a PS2 port.
- `game/Default.xbe` (1,253,376 bytes), from `tools/xbe_info.py`:
  - retail XBE, base 0x10000, image size 0xBECDC0, entry 0x000B389B, kernel thunk 0x00114140, TLS 0x00115514, stack 0x40000.
  - PE origin `C:\proj\gauntlet\Code\Release\shell3D.exe`, PE timestamp 0x3C9BB794 (2002-03-22).
  - XDK 4242 for all libs: XAPILIB D3D8 D3DX8 XGRAPHC DSOUND XBOXKRNL WMVDEC LIBCPMT LIBCMT.
  - Sections: .text 0x11000 (0xAF9FC), D3D, D3DX, XGRPH, DSOUND, WMVDEC, XPP, .rdata 0x114140, .data 0x116320 (vsize 0xADC3FC, BSS heavy), .XID, .XPP&Dat, DOLBY, $$XTIMAGE, $$XSIMAGE.
  - 100 kernel imports.
- `game/shell3D.pdb`: VC6 "program database 2.00" (JG). Read with `tools/pdb2.py` (info | publics | stream).
  - 9207 publics (S_PUB32_ST), 6134 PROCREF, 7608 UDT, plus module info and types.
  - NOT the same build as Default.xbe: its PE section table (in the PDB) has .text 0xD3A7A and D3D 0x17130 vs the XBE's 0xAF9FC / 0xFDE0 (probably debug XDK libs), and PDB signature 0x3C9BD347 is ~2 h after the XBE PE timestamp.
  - Use names, types and module lists as reference; addresses must be matched to the XBE (function signatures, strings, call graph).

## Done
- 2026-11-15: approach decided, see `docs/design.md`: x86 -> C static recomp, 64-bit host, NV2A push-buffer translation, DSOUND HLE.
- `tools/pdb2.py procs|modules` (per-module S_GPROC32_ST/S_LPROC32_ST with sizes, 7405 procs, 503 modules).
- `tools/match_pdb.py <xbe> <pdb> <out.tsv>` maps PDB procs to XBE .text: 1554 exact/moved of 4656 seg-1 procs (40 s). main = 0x11C10, mainXapiStartup = 0xB382C, mainCRTStartup = entry 0xB389B.
- `tools/xbedis.py <xbe> <va> [n]` capstone disassembly (pip capstone 5.0.7).

- 2026-11-15: lifter `recomp/lift.py` (Python + capstone -> C). `sh tools/relift.sh` = lift + compile (clang-cl, ~1 min) + oracle DLL.
  - Discovery: strong seeds (entry, PDB exact/moved from `assets/pdb_match.tsv`, call targets), weak seeds (data pointers,
    push/mov imm32 into code) dropped if their body hits junk opcodes. 4063 functions, 348k insns (some tail duplication),
    .text coverage 96.0%; 246 jump tables (bounded by cmp on the index reg, or unbounded state-machine tables near the jmp).
  - Output `out/lifted/`: f_NNN.c, funcs.h, functable.c, functions.tsv (va, insns, calls, fs, indirect, leaf, PDB name),
    lift_report.txt (unimplemented mnemonics, coverage, largest gaps). 45 unimplemented sites (cvtps2pi, pshufw, fnsave/frstor).
  - Model: regs + flags are C locals (clang drops dead flag math), synced to X86Context around calls; guest stack is real,
    `ret` = C return; x87 = ctx.st[8] doubles + top; MMX/SSE helpers in recomp/include/x86simd.h.
  - `assets/pdb_match.tsv` is git-ignored (disc-derived); regenerate with tools/match_pdb.py. It was pushed once in 8f8fc8c.
- Differential oracle: `out/oracle.dll` (runtime/oracle/oracle.c) vs unicorn on original bytes:
  `python -I tools/oracle_test.py [--trials N] [--only VA,..] [-v]` (~6 min for all leaves). Leaf functions: 875 pass,
  3 known fails (C6E90/D8FA6 `fyl2x` negative input: unicorn/QEMU does not pop; C9290 = rdtsc), 108 skipped (inputs fault).
  Bugs it found and fixed: fxch with implicit st(0) operand, fsubp/fdivp one-operand forms, rep movsd overlap, 8/16-bit shift UB.

## Next
1. HLE override list in the lifter (VA -> host function, body skipped) for kernel thunks, XAPI, D3D, DSOUND entry points.
2. Loader + runtime (`runtime/`): 4 GB reserve, sections, kernel thunk table -> sentinel VAs routed to kernel HLE,
   TLS/KPCR for fs:, thread start at entry 0xB389B; log missed indirect targets to feed back as seeds.
3. Extend oracle to non-leaf functions (callees are lifted too); skip ones that reach kernel/fs.
