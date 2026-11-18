/* HD texture replacement: PCSX2-format packs (<TEX0 hash>-<CLUT hash>-<bits>.png).
 * The Xbox build keeps the PS2 8-bit indexed textures, so the PS2 hashes can be rebuilt from the Xbox data. */
#pragma once
#include <stdint.h>
#include <d3d11.h>

int hdtex_enabled(void);
/* idx: linear 8-bit indices (w*h). pal: guest A8R8G8B8 palette (pal_len entries). Returns a pack entry id or -1. */
int hdtex_find(const uint8_t *idx, int w, int h, const uint32_t *pal, uint32_t pal_len);
/* The entry's texture with a new reference once it is loaded; NULL while it is loading (the first call queues it)
 * or when it failed. */
ID3D11ShaderResourceView *hdtex_get(ID3D11Device *dev, int id);
