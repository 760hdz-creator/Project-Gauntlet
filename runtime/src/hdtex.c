/* HD texture replacement from a PCSX2 texture pack.
 *
 * Pack files are named <TEX0 hash>-<CLUT hash>-<bits>.png (or <TEX0 hash>-<bits>.png), hex without padding.
 * bits = PSM:6 TW:4 TH:4 (TEXA/region bits above are ignored here).
 *
 * The Xbox build stores the PS2 indexed textures as D3D P8 with the same indices and palette, so we rebuild the
 * hashes PCSX2 computes (GSTextureCache HashTextureLevel / PaletteKeyHash):
 *  - TEX0 hash: XXH3-64 over the GS memory blocks of the texture in raster block order. PSMT8 block = 16x16
 *    texels (256 bytes), PSMT4 block = 32x16 (256 bytes, two texels per byte, low nibble first). A texture
 *    smaller than one block is hashed as linear indices, one byte per texel.
 *  - CLUT hash: XXH3-64 over the palette as PS2 RGBA8 words (R in the low byte) with GS alpha (0x80 = opaque):
 *    Xbox alpha 0xFF -> 0x80, else (a + 1) / 2. 256 entries for PSMT8, 16 for PSMT4.
 * Which PS2 format a texture had is not known, so PSMT8 is tried first and PSMT4 when every index is < 16.
 * If the CLUT does not match but the pack has exactly one file for that TEX0 hash and size, that file is used.
 *
 * PNG alpha is GS scale too (0x80 = opaque); it is doubled on load. Files load on a worker thread (WIC) with a
 * CPU box-filtered mip chain; until a file is ready the original texture is drawn.
 * GT_HDTEX=<dir> picks the pack folder (default "textures" next to the exe), GT_HDTEX=0 turns it off,
 * GT_HDLOG=1 logs every lookup, GT_HDDEBUG=1 tints replaced textures magenta.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define XXH_INLINE_ALL
#include "xxhash.h"
#include "hdtex.h"
#include "rt.h"

typedef struct {
    uint64_t tex0, clut;     /* clut 0 = file without a CLUT hash */
    uint32_t bits;           /* PSM | TW << 6 | TH << 10 */
    uint32_t path;           /* offset into the string pool */
    volatile LONG state;     /* 0 idle, 1 queued, 2 ready, 3 failed */
    ID3D11ShaderResourceView *srv;
} Entry;

static Entry *ents; static int n_ents, cap_ents;
static char *pool; static size_t n_pool, cap_pool;
static int *htab; static uint32_t hmask;      /* (tex0, clut, bits) -> entry, open addressing */
static int *t0tab;                             /* (tex0, bits) -> entry or -2 when several files share it */
static int g_enabled = -1, g_log, g_debug;
static char g_dir[MAX_PATH];

static uint64_t mix(uint64_t a, uint64_t b, uint32_t c) {
    uint64_t h = a * 0x9E3779B97F4A7C15ull ^ b * 0xC2B2AE3D27D4EB4Full ^ c;
    return h ^ h >> 31;
}

static void add_entry(uint64_t tex0, uint64_t clut, uint32_t bits, const char *path) {
    if (n_ents == cap_ents) { cap_ents = cap_ents ? cap_ents * 2 : 4096; ents = realloc(ents, sizeof *ents * cap_ents); }
    size_t len = strlen(path) + 1;
    while (n_pool + len > cap_pool) { cap_pool = cap_pool ? cap_pool * 2 : 1 << 20; pool = realloc(pool, cap_pool); }
    memcpy(pool + n_pool, path, len);
    Entry *e = &ents[n_ents++];
    memset(e, 0, sizeof *e);
    e->tex0 = tex0; e->clut = clut; e->bits = bits; e->path = (uint32_t)n_pool;
    n_pool += len;
}

static void scan(const char *dir) {
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char full[MAX_PATH * 2];
        if (fd.cFileName[0] == '.') continue;
        snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { scan(full); continue; }
        const char *dot = strrchr(fd.cFileName, '.');
        if (!dot || _stricmp(dot, ".png")) continue;
        /* <tex0>-<clut>-<bits>.png or <tex0>-<bits>.png (region and -mipN names are not used) */
        char *e1, *e2, *e3;
        uint64_t a = _strtoui64(fd.cFileName, &e1, 16);
        if (*e1 != '-') continue;
        uint64_t b = _strtoui64(e1 + 1, &e2, 16);
        if (e2 == dot && e2 - e1 - 1 == 8) { add_entry(a, 0, (uint32_t)b & 0x3FFF, full); continue; }
        if (*e2 != '-') continue;
        uint64_t c = _strtoui64(e2 + 1, &e3, 16);
        if (e3 != dot) continue;
        add_entry(a, b, (uint32_t)c & 0x3FFF, full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int *probe(int *tab, uint64_t tex0, uint64_t clut, uint32_t bits, int by_tex0) {
    uint32_t i = (uint32_t)mix(tex0, by_tex0 ? 0 : clut, bits) & hmask;
    for (;; i = (i + 1) & hmask) {
        int k = tab[i];
        if (k == -1) return &tab[i];
        int ek = k >= 0 ? k : -k - 3;    /* -2 marks "several": stored as -(first + 3) */
        Entry *e = &ents[ek];
        if (e->tex0 == tex0 && e->bits == bits && (by_tex0 || e->clut == clut)) return &tab[i];
    }
}

/* ---------------- loader thread ---------------- */

static CRITICAL_SECTION q_lock;
static HANDLE q_event;
static int q_buf[4096]; static unsigned q_head, q_tail;
static ID3D11Device *q_dev;
static IWICImagingFactory *wic;

static ID3D11ShaderResourceView *load_png(const char *path) {
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    WCHAR wpath[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH * 2);
    UINT w = 0, h = 0;
    uint32_t *lv[16] = {0};
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(wic, wpath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
        goto out;
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame))) goto out;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(wic, &conv))) goto out;
    if (FAILED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppRGBA,
                                              WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)))
        goto out;
    IWICFormatConverter_GetSize(conv, &w, &h);
    if (!w || !h || w > 8192 || h > 8192) goto out;
    lv[0] = malloc((size_t)w * h * 4);
    if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, (BYTE *)lv[0]))) goto out;
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {   /* GS alpha (0x80 = 1.0) -> 8-bit */
        uint32_t c = lv[0][i], a = c >> 24;
        a = a >= 0x80 ? 255 : a * 2;
        if (g_debug) c = (c & 0xFF00FF) | ((c >> 10) & 0x3F00);   /* GT_HDDEBUG=1: replaced textures turn magenta */
        lv[0][i] = (c & 0xFFFFFF) | a << 24;
    }
    D3D11_SUBRESOURCE_DATA init[16];
    int nl = 0;
    UINT lw = w, lh = h;
    for (;;) {
        init[nl].pSysMem = lv[nl]; init[nl].SysMemPitch = lw * 4; init[nl].SysMemSlicePitch = 0;
        nl++;
        if ((lw == 1 && lh == 1) || nl == 16) break;
        UINT nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
        uint32_t *src = lv[nl - 1], *dst = malloc((size_t)nw * nh * 4);
        for (UINT y = 0; y < nh; y++)
            for (UINT x = 0; x < nw; x++) {
                UINT x0 = x * lw / nw, y0 = y * lh / nh, x1 = lw > 1 ? x0 + 1 : x0, y1 = lh > 1 ? y0 + 1 : y0;
                uint32_t p[4] = {src[y0 * lw + x0], src[y0 * lw + x1], src[y1 * lw + x0], src[y1 * lw + x1]}, o = 0;
                for (int s = 0; s < 32; s += 8)
                    o |= (((p[0] >> s & 255) + (p[1] >> s & 255) + (p[2] >> s & 255) + (p[3] >> s & 255) + 2) / 4) << s;
                dst[y * nw + x] = o;
            }
        lv[nl] = dst; lw = nw; lh = nh;
    }
    D3D11_TEXTURE2D_DESC d = {0};
    d.Width = w; d.Height = h; d.MipLevels = nl; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *tex = NULL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(q_dev, &d, init, &tex))) {
        ID3D11Device_CreateShaderResourceView(q_dev, (ID3D11Resource *)tex, NULL, &srv);
        ID3D11Texture2D_Release(tex);   /* the view keeps it alive */
    }
out:
    for (int i = 0; i < 16; i++) free(lv[i]);
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    return srv;
}

static DWORD WINAPI loader_thread(LPVOID unused) {
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic))) {
        LOG("[hdtex] WIC unavailable, HD textures off\n");
        g_enabled = 0;
        return 0;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        WaitForSingleObject(q_event, INFINITE);
        for (;;) {
            EnterCriticalSection(&q_lock);
            int id = q_head != q_tail ? q_buf[q_head++ % 4096] : -1;
            LeaveCriticalSection(&q_lock);
            if (id < 0) break;
            Entry *e = &ents[id];
            DWORD t0 = GetTickCount();
            e->srv = load_png(pool + e->path);
            if (g_log) LOG("[hdtex] load %s: %s (%lu ms)\n", pool + e->path, e->srv ? "ok" : "FAILED", GetTickCount() - t0);
            InterlockedExchange(&e->state, e->srv ? 2 : 3);
        }
    }
}

int hdtex_enabled(void) {
    if (g_enabled >= 0) return g_enabled;
    g_enabled = 0;
    const char *env = getenv("GT_HDTEX");
    g_log = getenv("GT_HDLOG") && *getenv("GT_HDLOG") == '1';
    g_debug = getenv("GT_HDDEBUG") && *getenv("GT_HDDEBUG") == '1';
    if (env && !strcmp(env, "0")) return 0;
    snprintf(g_dir, sizeof g_dir, "%s", env && *env ? env : "textures");
    DWORD t0 = GetTickCount();
    scan(g_dir);
    if (!n_ents) { if (env) LOG("[hdtex] no PNG files in %s\n", g_dir); return 0; }
    uint32_t sz = 1;
    while (sz < (uint32_t)n_ents * 2) sz <<= 1;
    hmask = sz - 1;
    htab = malloc(sizeof *htab * sz); t0tab = malloc(sizeof *t0tab * sz);
    memset(htab, 0xFF, sizeof *htab * sz); memset(t0tab, 0xFF, sizeof *t0tab * sz);
    int dup = 0;
    for (int i = 0; i < n_ents; i++) {
        Entry *e = &ents[i];
        int *s = probe(htab, e->tex0, e->clut, e->bits, 0);
        if (*s == -1) *s = i; else dup++;
        s = probe(t0tab, e->tex0, 0, e->bits, 1);
        if (*s == -1) *s = i;
        else if (*s >= 0) *s = -*s - 3;   /* several files: no CLUT-less fallback */
    }
    InitializeCriticalSection(&q_lock);
    q_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_enabled = 1;
    LOG("[hdtex] %d replacement textures in %s (%d duplicates, %lu ms)\n", n_ents, g_dir, dup, GetTickCount() - t0);
    return 1;
}

/* GS block layouts within one 256-byte block (GSLocalMemory column tables): byte offset of PSMT8 texel (x, y)
 * in a 16x16 block, nibble offset of PSMT4 texel (x, y) in a 32x16 block. Four 4-row columns; odd columns and
 * rows 2-3 swap the two 4-texel halves. */
static unsigned off8(unsigned x, unsigned y) {
    unsigned c = y >> 2, r = y & 3, xs = x ^ ((((r >> 1) ^ c) & 1) * 4);
    return c * 64 + (xs & 1) * 4 + ((xs >> 1) & 3) * 16 + ((x >> 3) & 1) * 2 + (r & 1) * 8 + ((r >> 1) & 1);
}
static unsigned off4(unsigned x, unsigned y) {
    unsigned c = y >> 2, r = y & 3, xs = x ^ ((((r >> 1) ^ c) & 1) * 4);
    return c * 128 + (xs & 1) * 8 + ((xs >> 1) & 3) * 32 + ((x >> 3) & 3) * 2 + (r & 1) * 16 + ((r >> 1) & 1);
}

static uint64_t tex_hash(const uint8_t *idx, int w, int h, int psmt4) {
    int bw = psmt4 ? 32 : 16, bh = 16;
    if (w < bw || h < bh) return XXH3_64bits(idx, (size_t)w * h);
    static unsigned short o8[256], o4[512];
    if (!o8[1]) {
        for (unsigned y = 0; y < 16; y++) {
            for (unsigned x = 0; x < 16; x++) o8[y * 16 + x] = (unsigned short)off8(x, y);
            for (unsigned x = 0; x < 32; x++) o4[y * 32 + x] = (unsigned short)off4(x, y);
        }
    }
    XXH3_state_t st;
    XXH3_64bits_reset(&st);
    uint8_t blk[256];
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += bw) {
            if (psmt4) {
                memset(blk, 0, sizeof blk);
                for (int y = 0; y < 16; y++)
                    for (int x = 0; x < 32; x++) {
                        unsigned n = o4[y * 32 + x];
                        blk[n >> 1] |= (uint8_t)((idx[(by + y) * w + bx + x] & 15) << ((n & 1) * 4));
                    }
            } else {
                for (int y = 0; y < 16; y++)
                    for (int x = 0; x < 16; x++) blk[o8[y * 16 + x]] = idx[(by + y) * w + bx + x];
            }
            XXH3_64bits_update(&st, blk, sizeof blk);
        }
    return XXH3_64bits_digest(&st);
}

static int log2i(int v) { int n = 0; while ((1 << n) < v) n++; return n; }

int hdtex_find(const uint8_t *idx, int w, int h, const uint32_t *pal, uint32_t pal_len) {
    if (!hdtex_enabled()) return -1;
    uint32_t gs[256] = {0};
    for (uint32_t k = 0; k < pal_len && k < 256; k++) {
        uint32_t c = pal[k], a = c >> 24;
        gs[k] = (c >> 16 & 255) | (c & 0xFF00) | (c & 255) << 16 | (a == 255 ? 0x80u : (a + 1) / 2) << 24;
    }
    uint8_t mx = 0;
    for (int i = 0, n = w * h; i < n; i++) if (idx[i] > mx) mx = idx[i];
    uint32_t size_bits = (uint32_t)log2i(w) << 6 | (uint32_t)log2i(h) << 10;
    int id = -1;
    const char *how = "miss";
    uint64_t t8 = tex_hash(idx, w, h, 0), t4 = 0, c8 = XXH3_64bits(gs, (size_t)(pal_len < 256 ? pal_len : 256) * 4),
             c4 = XXH3_64bits(gs, 16 * 4);
    if (mx < 16) t4 = tex_hash(idx, w, h, 1);
    struct { uint64_t t, c; uint32_t psm; } tries[4] = {{t8, c8, 0x13}, {t4, c4, 0x14}, {t4, c8, 0x14}, {t8, c4, 0x13}};
    for (int k = 0; k < 4 && id < 0; k++) {
        if (tries[k].psm == 0x14 && mx >= 16) continue;
        int s = *probe(htab, tries[k].t, tries[k].c, tries[k].psm | size_bits, 0);
        if (s >= 0) { id = s; how = "exact"; }
    }
    for (int k = 0; k < 2 && id < 0; k++) {   /* one file for this TEX0 hash: use it whatever the CLUT */
        if (k == 1 && mx >= 16) break;
        int s = *probe(t0tab, k ? t4 : t8, 0, (k ? 0x14 : 0x13) | size_bits, 1);
        if (s >= 0) { id = s; how = "tex0"; }
    }
    if (g_log) LOG("[hdtex] %dx%d pal %u: t8 %llx t4 %llx clut %llx/%llx -> %s %s\n", w, h, pal_len,
                   (unsigned long long)t8, (unsigned long long)t4, (unsigned long long)c8, (unsigned long long)c4, how,
                   id >= 0 ? pool + ents[id].path : "");
    return id;
}

ID3D11ShaderResourceView *hdtex_get(ID3D11Device *dev, int id) {
    if (id < 0 || id >= n_ents || g_enabled != 1) return NULL;
    Entry *e = &ents[id];
    LONG st = e->state;
    if (st == 2) { ID3D11ShaderResourceView_AddRef(e->srv); return e->srv; }
    if (st != 0) return NULL;
    static HANDLE thr;
    if (!thr) { q_dev = dev; thr = CreateThread(NULL, 0, loader_thread, NULL, 0, NULL); }
    EnterCriticalSection(&q_lock);
    if (q_tail - q_head < 4096) { q_buf[q_tail++ % 4096] = id; e->state = 1; }
    LeaveCriticalSection(&q_lock);
    SetEvent(q_event);
    return NULL;
}
