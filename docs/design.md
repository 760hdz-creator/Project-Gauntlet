# Project Gauntlet - Design

Decided 2026-11-15.

## Approach: static x86 -> C recompilation, 64-bit host

The guest is 32-bit x86 (Pentium III, VC6 code, XDK 4242). We lift all code to C and run it on a 64-bit host.
We do not execute the original code natively in a 32-bit process.

Why:
- The user asked for a static recomp. The design matches Project M and Project Zero: guest memory behind a base pointer, HLE by name, and a host renderer.
- A 64-bit host allows a Linux build.
- Default.xbe has no relocations, so direct execution would need the image at exactly 0x10000. Xbox code also reads `fs:` (KPCR/TLS, 47 sites), which collides with the Windows TEB. Lifting avoids both problems.

## Measurements (tools in `tools/`, linear sweep of .text)

| What | Count | Consequence |
|---|---|---|
| Instructions in .text | 228k | Generated C is large but manageable. |
| x87 instructions | 26,455 | The lifter models the x87 stack (`ctx.st[8]` + `top`, tag word ignored), with doubles and 80-bit loads and stores converted. |
| SSE / MMX | 2 / 1 | Handle them individually. |
| `fs:` accesses | 47 | Map them to a guest KPCR/TLS block (`ctx.fs_base`). |
| Jump tables (`jmp [reg*4+imm]`) | 191 | Recover them from VC6 table patterns, emitted as `switch`. |
| Indirect calls | ~300 + vtables | Use a function lookup table keyed by guest VA. |
| Direct calls .text -> D3D / DSOUND / XPP / D3DX / XGRPH / WMVDEC | 120 / 32 / 10 / 1 / 1 / 1 | These are library entry points to HLE. |
| Data refs .text -> D3D section | 128 (~10 globals) | Inline XDK D3D code is compiled into game code (for example, `D3DDevice_SetRenderState` is 0x1CC bytes in g3dmain.obj). |

## Graphics: NV2A push-buffer translation

Game code writes D3D globals and the push buffer directly through inline XDK functions.
HLE at the D3D API level alone would miss those writes.

Plan:
- Recompile the D3D library section like the rest of the code.
- Back the push buffer with guest RAM.
- Translate NV2A methods (PGRAPH: Kelvin 0x97) to a modern API on the host, as Project M's GX FIFO bridge does with Aurora.
- Use xemu's nv2a/pgraph as a read-only reference. It is GPL, so we do not copy code from it.
- Replace D3D functions with HLE only where it helps: device creation, Present/Swap, and memory allocation.

## Audio: DSOUND HLE at the API level

The DSOUND section (and the DOLBY section) is replaced wholesale by an SDL3 mixer.
We do not emulate the APU.

## Kernel

There are 100 xboxkrnl imports, listed by ordinal in `tools/xbe_info.py` output.
They are implemented on the host. The thunk table at 0x114140 is resolved to HLE functions at load time.

## Symbols

`shell3D.pdb` comes from a slightly different build (debug XDK libraries, game code drifts).
- `tools/match_pdb.py` maps PDB procs onto XBE .text by order and exact size. The first run named 1,554 of 4,656 seg-1 procs; `main` lands exactly at 0x11C10.
- Library objects (XAPI, LIBCMT, D3D8, DSOUND) still need XDK 4242 byte signatures.

## Memory map (guest)

- 64 MB RAM (0x00000000-0x03FFFFFF) plus a contiguous mirror at 0x80000000+.
- The image spans 0x10000-0xBFCDC0. The .data vsize is 0xADC3FC, mostly BSS.
- The host reserves 4 GB and sets `base + guest_va`. NV2A MMIO (0xFD000000) and other hardware ranges are trapped or stubbed.

## Next steps (ranked)

1. Lifter prototype (`recomp/`): Python + capstone, generating C per function. Start with function discovery (call targets + PDB matches + the entry), then a flags model (lazy), x87, and jump tables.
2. Kernel HLE skeleton and loader, aiming to reach `main` (0x11C10).
3. XDK 4242 signatures for XAPI, the CRT and D3D entry points.
4. NV2A push-buffer translator.
