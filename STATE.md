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

## Next
- Decide recompiler approach (x86 -> C static recomp vs direct execution with HLE).
