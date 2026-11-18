/* Host renderer for NV2A Kelvin state: Direct3D 11 backend.
 *
 * - Guest color/zeta surfaces map to host render targets keyed by physical address (scaled by GPU_SCALE).
 * - Vertices arrive decoded (KVertex, 16 x float4) with a triangle/line/point index list.
 * - Fixed-function transform: composite matrix -> screen space -> clip space (perspective-correct).
 * - Register combiners are translated to HLSL per combiner state and cached.
 * - Textures are decoded on the CPU (unswizzle, palette expansion) and cached by address, format and content hash.
 * - gpu_flip() presents a surface in a 4:3 letterbox.
 * All gpu_* calls come from the push-buffer thread.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "kelvin.h"
#include "hdtex.h"

static int g_scale = 0;   /* GT_SCALE=1..8: host render-target scale over the guest resolution; 0 = from the monitor */
static int g_msaa = 4;    /* GT_MSAA=1/2/4/8: samples per pixel on host render targets */
#define GPU_SCALE g_scale
static float g_wide = 1;  /* widescreen: main 640x480 targets are this much wider (GT_WIDESCREEN=0 off, GT_ASPECT=w:h) */
static int g_aniso = 16;  /* GT_ANISO=0/1..16: anisotropic filtering on minifying linear samplers */

static HWND g_hwnd;
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *swap;
static ID3D11RenderTargetView *bb_rtv;
static UINT bb_w, bb_h;
static DWORD gpu_tid;
static int gpu_ok;
static uint32_t frame_no;

/* ---------------- window ---------------- */

/* Borderless fullscreen on the window's monitor; F11 or Alt+Enter toggles, GT_FULLSCREEN=1 starts in it. */
static void toggle_fullscreen(HWND h) {
    static WINDOWPLACEMENT saved = {sizeof saved};
    LONG st = GetWindowLongW(h, GWL_STYLE);
    if (st & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = {sizeof mi};
        GetWindowPlacement(h, &saved);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(h, GWL_STYLE, (st & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(h, GWL_STYLE, (st & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &saved);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: LOG("[gpu] window closed\n"); ExitProcess(0);
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { LOG("[gpu] escape\n"); ExitProcess(0); }
        if (w == VK_F11) { toggle_fullscreen(h); return 0; }
        break;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) { toggle_fullscreen(h); return 0; }
        break;
    case WM_SYSCHAR: if (w == VK_RETURN) return 0; break;   /* no beep on Alt+Enter */
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Window placement: GT_MONITOR=N picks monitor N (enumeration order); default = the first non-primary
 * monitor, so test runs stay off the primary desktop. The window opens without taking focus. */
typedef struct { int want, idx; RECT pick; int found; } MonPick;
static BOOL CALLBACK mon_enum(HMONITOR m, HDC dc, LPRECT rc, LPARAM lp) {
    MonPick *mp = (MonPick *)lp;
    MONITORINFO mi = {sizeof mi};
    (void)dc; (void)rc;
    GetMonitorInfoW(m, &mi);
    int hit = mp->want >= 0 ? mp->idx == mp->want : !(mi.dwFlags & MONITORINFOF_PRIMARY);
    if (hit && !mp->found) { mp->pick = mi.rcWork; mp->found = 1; }
    mp->idx++;
    return TRUE;
}

static DWORD WINAPI window_thread(LPVOID ev) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"GauntletRecomp";
    RegisterClassW(&wc);
    const char *me = getenv("GT_MONITOR");
    MonPick mp = {me ? atoi(me) : -1, 0, {0}, 0};
    EnumDisplayMonitors(NULL, NULL, mon_enum, (LPARAM)&mp);
    /* window: 2/3 of the monitor height, monitor aspect (4:3 with GT_WIDESCREEN=0) */
    int wh = 960, ww = 1280;
    {
        POINT origin = {0, 0};
        HMONITOR hm = mp.found ? MonitorFromRect(&mp.pick, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(hm, &mi);
        int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        const char *ws = getenv("GT_WIDESCREEN");
        wh = mh * 2 / 3;
        ww = ws && *ws == '0' ? wh * 4 / 3 : (int)((double)wh * mw / mh);
    }
    RECT r = {0, 0, ww, wh};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (mp.found) { x = mp.pick.left + 32; y = mp.pick.top + 32; }
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Gauntlet Dark Legacy (recomp)", WS_OVERLAPPEDWINDOW,
                             x, y, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    if (getenv("GT_FULLSCREEN") && *getenv("GT_FULLSCREEN") == '1') toggle_fullscreen(g_hwnd);
    SetEvent((HANDLE)ev);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

/* ---------------- shaders ---------------- */

static ID3DBlob *compile(const char *src, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        LOG("[gpu] shader compile failed (%s):\n%s\n---\n%s\n", target, err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?", src);
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static const char *vs_src =
    "cbuffer V : register(b0) { float4 M[4]; float4 vp; float4 zs; float4 T[16]; float4 ten; };\n"
    "struct VI { float4 a0:A0; float4 a3:A3; float4 a4:A4; float4 a5:A5; float4 a9:A9; float4 a10:A10; float4 a11:A11; float4 a12:A12; };\n"
    "struct VO { float4 pos:SV_Position; float4 d:COLOR0; float4 s:COLOR1; float4 t0:TEXCOORD0; float4 t1:TEXCOORD1;\n"
    "            float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; float fog:FOG; };\n"
    "float4 tm(int s, float4 t) { return ten[s] > 0 ? float4(dot(T[s*4+0],t), dot(T[s*4+1],t), dot(T[s*4+2],t), dot(T[s*4+3],t)) : t; }\n"
    "VO main(VI i) {\n"
    "  float4 p = i.a0;\n"
    "  float4 c = float4(dot(M[0], p), dot(M[1], p), dot(M[2], p), dot(M[3], p));\n"
    "  VO o;\n"
    "  float sx = c.x + vp.x * c.w, sy = c.y + vp.y * c.w;\n"
    "  o.pos = float4((sx * vp.z - c.w) * zs.y, c.w - sy * vp.w, c.z * zs.x, c.w);\n"
    "  o.d = i.a3; o.s = i.a4;\n"
    "  o.t0 = tm(0, i.a9); o.t1 = tm(1, i.a10); o.t2 = tm(2, i.a11); o.t3 = tm(3, i.a12);\n"
    "  o.fog = 1;\n"
    "  return o;\n"
    "}\n";

static const char *blit_vs_src =
    "struct VO { float4 pos:SV_Position; float2 uv:TEXCOORD0; };\n"
    "VO main(uint id : SV_VertexID) { VO o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = t; o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";
/* Present pass with optional post-processing (pp: x sharpen 0..1, y FXAA on, z CRT on, w source lines).
 * FXAA: a small FXAA 3.11-style edge blend. Sharpen: contrast-adaptive (CAS-like), strength x. CRT: scanlines on the
 * game's 480 lines, a faint RGB mask and a soft vignette. */
static const char *blit_ps_src =
    "Texture2D tx : register(t0); SamplerState sm : register(s0);\n"
    "cbuffer P : register(b0) { float4 pp; };\n"
    "float L(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "float3 S(float2 uv) { return tx.SampleLevel(sm, uv, 0).rgb; }\n"
    "float4 main(float4 pos:SV_Position, float2 uv:TEXCOORD0) : SV_Target {\n"
    "  uint W, H; tx.GetDimensions(W, H); float2 t = 1.0 / float2(W, H);\n"
    "  float3 c = S(uv);\n"
    "  if (pp.y > 0 || pp.x > 0) {\n"
    "    float3 n = S(uv + float2(0, -t.y)), s = S(uv + float2(0, t.y)), e = S(uv + float2(t.x, 0)), w = S(uv - float2(t.x, 0));\n"
    "    if (pp.y > 0) {\n"
    "      float lc = L(c), ln = L(n), ls = L(s), le = L(e), lw = L(w);\n"
    "      float mn = min(lc, min(min(ln, ls), min(le, lw))), mx = max(lc, max(max(ln, ls), max(le, lw)));\n"
    "      if (mx - mn > max(0.0312, mx * 0.125)) {\n"
    "        float a = L(S(uv - t)), b = L(S(uv + float2(t.x, -t.y))), d = L(S(uv + float2(-t.x, t.y))), f = L(S(uv + t));\n"
    "        float2 dir = float2(-((a + b) - (d + f)), (a + d) - (b + f));\n"
    "        float red = max((a + b + d + f) * 0.03125, 1.0 / 128);\n"
    "        dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + red), -8, 8) * t;\n"
    "        float3 r1 = 0.5 * (S(uv - dir / 6) + S(uv + dir / 6));\n"
    "        float3 r2 = r1 * 0.5 + 0.25 * (S(uv - dir * 0.5) + S(uv + dir * 0.5));\n"
    "        float l2 = L(r2); c = (l2 < mn || l2 > mx) ? r1 : r2;\n"
    "      }\n"
    "    }\n"
    "    if (pp.x > 0) {\n"
    "      float3 mn3 = min(c, min(min(n, s), min(e, w))), mx3 = max(c, max(max(n, s), max(e, w)));\n"
    "      float3 amp = sqrt(saturate(min(mn3, 1 - mx3) / max(mx3, 1e-4)));\n"
    "      float3 wt = -amp * lerp(0.125, 0.2, pp.x);\n"
    "      c = saturate((c + (n + s + e + w) * wt) / (1 + 4 * wt));\n"
    "    }\n"
    "  }\n"
    "  if (pp.z > 0) {\n"
    "    float sl = sin(uv.y * pp.w * 3.14159265); sl = 0.62 + 0.38 * sl * sl;\n"
    "    uint m = (uint)pos.x % 3; float3 mask = float3(m == 0 ? 1.0 : 0.9, m == 1 ? 1.0 : 0.9, m == 2 ? 1.0 : 0.9);\n"
    "    float2 v = uv * (1 - uv); float vig = saturate(pow(v.x * v.y * 16, 0.15));\n"
    "    c = saturate(c * sl * mask * vig * 1.22);\n"
    "  }\n"
    "  return float4(c, 1); }\n";
static ID3D11Buffer *post_cb;
static float post_pp[4];   /* GT_SHARPEN=0..100, GT_FXAA=1, GT_CRT=1 */

static ID3D11VertexShader *vs, *blit_vs;
static ID3D11PixelShader *blit_ps;
static ID3D11InputLayout *layout;
static ID3D11Buffer *vs_cb, *ps_cb, *vbuf, *ibuf;
static ID3D11SamplerState *blit_sampler;
#define VBUF_SIZE (16u << 20)
#define IBUF_SIZE (4u << 20)
static UINT vbuf_pos = VBUF_SIZE, ibuf_pos = IBUF_SIZE;

typedef struct { float M[16]; float vp[4]; float zs[4]; float T[64]; float ten[4]; } VSConst;
typedef struct { float C0[8][4]; float C1[8][4]; float fogc[4]; float at[4]; float tsz[4][4]; } PSConst;

/* ---------------- surfaces ---------------- */

typedef struct {
    uint32_t addr; int w, h;
    ID3D11Texture2D *tex; ID3D11RenderTargetView *rtv; ID3D11ShaderResourceView *srv;
    ID3D11Texture2D *ms;   /* multisampled color (rtv points here when g_msaa > 1); resolved into tex on read */
    int dirty;
    float k;               /* horizontal widening (g_wide for the main 640x480 targets, else 1) */
    int hw, hh;            /* host size */
    uint32_t last_frame;
} Surface;
typedef struct { uint32_t addr; int w, h; ID3D11Texture2D *tex; ID3D11DepthStencilView *dsv; } ZSurface;
static Surface rts[16]; static int n_rts;
static ZSurface zss[8]; static int n_zss;

static void surface_resolve(Surface *s) {
    if (s->ms && s->dirty) {
        ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *)s->tex, 0, (ID3D11Resource *)s->ms, 0,
                                               DXGI_FORMAT_B8G8R8A8_UNORM);
        s->dirty = 0;
    }
}

static Surface *find_rt(uint32_t addr) {
    for (int i = 0; i < n_rts; i++) if (rts[i].addr == addr) return &rts[i];
    return NULL;
}

static float wide_k(int w, int h) { return w == 640 && h == 480 ? g_wide : 1.0f; }
static int host_w(int w, int h) { return (int)(w * GPU_SCALE * wide_k(w, h) + 0.5f) & ~1; }

static Surface *get_rt(uint32_t addr, int w, int h) {
    Surface *s = find_rt(addr);
    if (s && s->w == w && s->h == h) return s;
    if (!s) {
        if (n_rts == 16) { /* recycle the oldest */
            int o = 0;
            for (int i = 1; i < n_rts; i++) if (rts[i].last_frame < rts[o].last_frame) o = i;
            s = &rts[o];
        } else s = &rts[n_rts++];
    }
    if (s->tex) { ID3D11Texture2D_Release(s->tex); ID3D11RenderTargetView_Release(s->rtv); ID3D11ShaderResourceView_Release(s->srv); }
    if (s->ms) { ID3D11Texture2D_Release(s->ms); s->ms = NULL; }
    D3D11_TEXTURE2D_DESC d = {0};
    d.Width = host_w(w, h); d.Height = h * GPU_SCALE; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, &s->tex);
    if (g_msaa > 1) {
        d.SampleDesc.Count = g_msaa; d.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Device_CreateTexture2D(dev, &d, NULL, &s->ms);
    }
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)(s->ms ? s->ms : s->tex), NULL, &s->rtv);
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)s->tex, NULL, &s->srv);
    float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, s->rtv, black);
    s->addr = addr; s->w = w; s->h = h; s->dirty = 1;
    s->k = wide_k(w, h); s->hw = host_w(w, h); s->hh = h * GPU_SCALE;
    LOG("[gpu] render target %08X %dx%d\n", addr, w, h);
    return s;
}

static ZSurface *get_zs(uint32_t addr, int w, int h) {
    for (int i = 0; i < n_zss; i++) if (zss[i].addr == addr && zss[i].w == w && zss[i].h == h) return &zss[i];
    ZSurface *z = NULL;
    for (int i = 0; i < n_zss; i++) if (zss[i].addr == addr) z = &zss[i];
    if (!z) z = n_zss < 8 ? &zss[n_zss++] : &zss[0];
    if (z->tex) { ID3D11Texture2D_Release(z->tex); ID3D11DepthStencilView_Release(z->dsv); }
    D3D11_TEXTURE2D_DESC d = {0};
    d.Width = host_w(w, h); d.Height = h * GPU_SCALE; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; d.SampleDesc.Count = g_msaa; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, &z->tex);
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)z->tex, NULL, &z->dsv);
    ID3D11DeviceContext_ClearDepthStencilView(ctx, z->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    z->addr = addr; z->w = w; z->h = h;
    LOG("[gpu] depth surface %08X %dx%d\n", addr, w, h);
    return z;
}

static Surface *cur_rt;
static ZSurface *cur_zs;

static void surface_size(int *w, int *h) {
    uint32_t ch = KREG(K_CLIP_H), cv = KREG(K_CLIP_V);
    *w = (int)((ch & 0xFFFF) + (ch >> 16));
    *h = (int)((cv & 0xFFFF) + (cv >> 16));
    if (*w <= 0 || *w > 4096) *w = 640;
    if (*h <= 0 || *h > 4096) *h = 480;
}

static void bind_targets(void) {
    int w, h;
    surface_size(&w, &h);
    uint32_t caddr = (nv2a_dma_base(KREG(K_DMA_COLOR)) + KREG(K_COLOR_OFFSET)) & 0x03FFFFFF;
    cur_rt = get_rt(caddr, w, h);
    cur_rt->last_frame = frame_no;
    cur_rt->dirty = 1;
    uint32_t zfmt = (KREG(K_SURF_FORMAT) >> 4) & 0xF;
    cur_zs = NULL;
    if (zfmt) {
        uint32_t zaddr = (nv2a_dma_base(KREG(K_DMA_ZETA)) + KREG(K_ZETA_OFFSET)) & 0x03FFFFFF;
        cur_zs = get_zs(zaddr, w, h);
    }
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &cur_rt->rtv, cur_zs ? cur_zs->dsv : NULL);
    D3D11_VIEWPORT vp = {0, 0, (float)cur_rt->hw, (float)cur_rt->hh, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
}

/* ---------------- init ---------------- */

static void make_bb_rtv(void) {
    ID3D11Texture2D *bb = NULL;
    IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &bb_rtv);
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(bb, &d);
    bb_w = d.Width; bb_h = d.Height;
    ID3D11Texture2D_Release(bb);
}

static ID3D11Buffer *make_buffer(UINT size, UINT bind) {
    D3D11_BUFFER_DESC d = {0};
    d.ByteWidth = size; d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = bind; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *b = NULL;
    ID3D11Device_CreateBuffer(dev, &d, NULL, &b);
    return b;
}

void gpu_init(void) {
    if (getenv("GT_SCALE")) { g_scale = atoi(getenv("GT_SCALE")); if (g_scale < 1) g_scale = 1; if (g_scale > 8) g_scale = 8; }
    if (getenv("GT_MSAA")) { g_msaa = atoi(getenv("GT_MSAA")); if (g_msaa != 2 && g_msaa != 4 && g_msaa != 8) g_msaa = 1; }
    if (getenv("GT_ANISO")) { g_aniso = atoi(getenv("GT_ANISO")); if (g_aniso < 0) g_aniso = 0; if (g_aniso > 16) g_aniso = 16; }
    if (gpu_ok) return;
    gpu_ok = -1;
    gpu_tid = GetCurrentThreadId();
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, window_thread, ev, 0, NULL);
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);

    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    UINT flags = 0;
    if (getenv("GT_D3DDEBUG")) flags |= D3D11_CREATE_DEVICE_DEBUG;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, NULL, 0, D3D11_SDK_VERSION,
                                               &sd, &swap, &dev, NULL, &ctx);
    if (FAILED(hr)) { LOG("[gpu] D3D11CreateDeviceAndSwapChain failed %08lX\n", hr); return; }
    if (!g_scale) {   /* auto: enough to cover the monitor's height (the 448-line safe area fills it) */
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        g_scale = (mh + 447) / 448;
        if (g_scale < 2) g_scale = 2;
        if (g_scale > 6) g_scale = 6;
    }
    {   /* widescreen (default; GT_WIDESCREEN=0 = original 4:3): hor+ to the monitor aspect (GT_ASPECT=w:h overrides). The visible area is the
         * 448-line title-safe height, so the target covers 448 * aspect guest columns. */
        const char *ws = getenv("GT_WIDESCREEN"), *as = getenv("GT_ASPECT");
        if (!(ws && *ws == '0')) {
            MONITORINFO mi = {sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            float aspect = (float)(mi.rcMonitor.right - mi.rcMonitor.left) / (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);
            float aw, ah;
            if (as && sscanf(as, "%f:%f", &aw, &ah) == 2 && aw > 0 && ah > 0) aspect = aw / ah;
            g_wide = 448.0f * aspect / 640.0f;
            if (g_wide < 1) g_wide = 1;
            if (g_wide > 3) g_wide = 3;
        }
    }
    if (g_msaa > 1) {
        UINT q1 = 0, q2 = 0;
        ID3D11Device_CheckMultisampleQualityLevels(dev, DXGI_FORMAT_B8G8R8A8_UNORM, g_msaa, &q1);
        ID3D11Device_CheckMultisampleQualityLevels(dev, DXGI_FORMAT_D24_UNORM_S8_UINT, g_msaa, &q2);
        if (!q1 || !q2) { LOG("[gpu] %dx MSAA not supported, off\n", g_msaa); g_msaa = 1; }
    }
    make_bb_rtv();
    {   /* Alt+Enter is ours (borderless), not DXGI's exclusive mode */
        IDXGIFactory *fac = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetParent(swap, &IID_IDXGIFactory, (void **)&fac))) {
            IDXGIFactory_MakeWindowAssociation(fac, g_hwnd, DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory_Release(fac);
        }
    }

    ID3DBlob *b = compile(vs_src, "vs_5_0");
    if (!b) return;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &vs);
    static const int attrs[] = {0, 3, 4, 5, 9, 10, 11, 12};
    D3D11_INPUT_ELEMENT_DESC el[8];
    for (int i = 0; i < 8; i++) {
        el[i].SemanticName = "A"; el[i].SemanticIndex = attrs[i]; el[i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        el[i].InputSlot = 0; el[i].AlignedByteOffset = attrs[i] * 16; el[i].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        el[i].InstanceDataStepRate = 0;
    }
    ID3D11Device_CreateInputLayout(dev, el, 8, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), &layout);
    ID3D10Blob_Release(b);
    b = compile(blit_vs_src, "vs_5_0");
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &blit_vs);
    ID3D10Blob_Release(b);
    b = compile(blit_ps_src, "ps_5_0");
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &blit_ps);
    ID3D10Blob_Release(b);

    D3D11_BUFFER_DESC cd = {0};
    cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.ByteWidth = (sizeof(VSConst) + 15) & ~15u;
    ID3D11Device_CreateBuffer(dev, &cd, NULL, &vs_cb);
    cd.ByteWidth = (sizeof(PSConst) + 15) & ~15u;
    ID3D11Device_CreateBuffer(dev, &cd, NULL, &ps_cb);
    vbuf = make_buffer(VBUF_SIZE, D3D11_BIND_VERTEX_BUFFER);
    ibuf = make_buffer(IBUF_SIZE, D3D11_BIND_INDEX_BUFFER);

    D3D11_SAMPLER_DESC smp = {0};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &smp, &blit_sampler);
    {
        const char *e;
        post_pp[0] = (e = getenv("GT_SHARPEN")) ? (float)atoi(e) / 100.f : 0.f;
        if (post_pp[0] > 1) post_pp[0] = 1;
        post_pp[1] = (e = getenv("GT_FXAA")) && *e == '1';
        post_pp[2] = (e = getenv("GT_CRT")) && *e == '1';
        post_pp[3] = 480;
        D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA sd = {post_pp, 0, 0};
        ID3D11Device_CreateBuffer(dev, &bd, &sd, &post_cb);
        if (post_pp[0] > 0 || post_pp[1] > 0 || post_pp[2] > 0)
            LOG("[gpu] post: sharpen %.2f fxaa %d crt %d\n", post_pp[0], (int)post_pp[1], (int)post_pp[2]);
    }
    gpu_ok = 1;
    LOG("[gpu] D3D11 ready, scale %d, msaa %d, aniso %d, widescreen %.3f, back buffer %ux%u\n", GPU_SCALE, g_msaa, g_aniso, g_wide, bb_w, bb_h);
}

static int ready(void) {
    if (!gpu_ok) gpu_init();
    if (gpu_ok != 1) return 0;
    return 1;
}

/* ---------------- hashing ---------------- */

static uint64_t hash_mem(uint32_t a, uint32_t n, uint64_t h) {
    a &= 0x03FFFFFF;
    if (a + n > RAM_SIZE) n = a < RAM_SIZE ? RAM_SIZE - a : 0;
    const uint8_t *p = g_mem + a;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v; memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* ---------------- textures ---------------- */

typedef struct {
    uint32_t addr, fmt, pitch, rect, pal_addr, pal_len;
    uint64_t hash;
    uint32_t last_frame;
    int w, h;
    int hd;   /* HD replacement entry still loading (hdtex.c), or -1 */
    ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv;
} TexEntry;

/* Swap in the HD replacement once its loader is done. */
static ID3D11ShaderResourceView *tex_srv(TexEntry *e) {
    if (e->hd >= 0) {
        ID3D11ShaderResourceView *s = hdtex_get(dev, e->hd);
        if (s) {
            if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
            if (e->tex) { ID3D11Texture2D_Release(e->tex); e->tex = NULL; }
            e->srv = s; e->hd = -1;
        }
    }
    return e->srv;
}
#define TEX_CACHE 4096
static TexEntry tcache[TEX_CACHE];
#define TEX_MEMO 1024
typedef struct { uint32_t frame, addr, fmt, pal_addr, pitch; TexEntry *e; } TexMemo;
static TexMemo tmemo[TEX_MEMO];

/* swizzle offsets: interleave x and y bits (x first) while both dimensions have bits left */
static void swizzle_tables(int w, int h, uint32_t *xs, uint32_t *ys) {
    for (int x = 0; x < w; x++) {
        uint32_t o = 0, bit = 1;
        for (int i = 0; (1 << i) < w || (1 << i) < h; i++) {
            if ((1 << i) < w) { if (x & (1 << i)) o |= bit; bit <<= 1; }
            if ((1 << i) < h) bit <<= 1;
        }
        xs[x] = o;
    }
    for (int y = 0; y < h; y++) {
        uint32_t o = 0, bit = 1;
        for (int i = 0; (1 << i) < w || (1 << i) < h; i++) {
            if ((1 << i) < w) bit <<= 1;
            if ((1 << i) < h) { if (y & (1 << i)) o |= bit; bit <<= 1; }
        }
        ys[y] = o;
    }
}

enum { TF_NONE, TF_Y8, TF_AY8, TF_A8, TF_A8Y8, TF_1555, TF_X555, TF_4444, TF_565, TF_8888, TF_X888, TF_I8,
       TF_ABGR, TF_BGRA, TF_RGBA, TF_DXT1, TF_DXT3, TF_DXT5 };
typedef struct { int kind, bpp, swizzled; } TexFmt;

static TexFmt tex_format(uint32_t c) {
    TexFmt f = {TF_NONE, 0, 1};
    switch (c) {
    case 0x00: f.kind = TF_Y8; f.bpp = 1; break;
    case 0x01: f.kind = TF_AY8; f.bpp = 1; break;
    case 0x02: f.kind = TF_1555; f.bpp = 2; break;
    case 0x03: f.kind = TF_X555; f.bpp = 2; break;
    case 0x04: f.kind = TF_4444; f.bpp = 2; break;
    case 0x05: f.kind = TF_565; f.bpp = 2; break;
    case 0x06: f.kind = TF_8888; f.bpp = 4; break;
    case 0x07: f.kind = TF_X888; f.bpp = 4; break;
    case 0x0B: f.kind = TF_I8; f.bpp = 1; break;
    case 0x0C: f.kind = TF_DXT1; f.swizzled = 0; break;
    case 0x0E: f.kind = TF_DXT3; f.swizzled = 0; break;
    case 0x0F: f.kind = TF_DXT5; f.swizzled = 0; break;
    case 0x10: f.kind = TF_1555; f.bpp = 2; f.swizzled = 0; break;
    case 0x11: f.kind = TF_565; f.bpp = 2; f.swizzled = 0; break;
    case 0x12: f.kind = TF_8888; f.bpp = 4; f.swizzled = 0; break;
    case 0x13: f.kind = TF_Y8; f.bpp = 1; f.swizzled = 0; break;
    case 0x19: f.kind = TF_A8; f.bpp = 1; break;
    case 0x1A: f.kind = TF_A8Y8; f.bpp = 2; break;
    case 0x1B: f.kind = TF_AY8; f.bpp = 1; f.swizzled = 0; break;
    case 0x1C: f.kind = TF_X555; f.bpp = 2; f.swizzled = 0; break;
    case 0x1D: f.kind = TF_4444; f.bpp = 2; f.swizzled = 0; break;
    case 0x1E: f.kind = TF_X888; f.bpp = 4; f.swizzled = 0; break;
    case 0x1F: f.kind = TF_A8; f.bpp = 1; f.swizzled = 0; break;
    case 0x20: f.kind = TF_A8Y8; f.bpp = 2; f.swizzled = 0; break;
    case 0x3A: f.kind = TF_ABGR; f.bpp = 4; break;
    case 0x3B: f.kind = TF_BGRA; f.bpp = 4; break;
    case 0x3C: f.kind = TF_RGBA; f.bpp = 4; break;
    case 0x3F: f.kind = TF_ABGR; f.bpp = 4; f.swizzled = 0; break;
    case 0x40: f.kind = TF_BGRA; f.bpp = 4; f.swizzled = 0; break;
    case 0x41: f.kind = TF_RGBA; f.bpp = 4; f.swizzled = 0; break;
    default: break;
    }
    return f;
}

static uint32_t c5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t c6(uint32_t v) { return (v << 2) | (v >> 4); }

/* One texel -> RGBA8 (R in the low byte). */
static uint32_t texel(int kind, const uint8_t *p, const uint32_t *pal) {
    uint32_t v, r, g, b, a;
    switch (kind) {
    case TF_Y8: v = p[0]; return v | v << 8 | v << 16 | 0xFF000000u;
    case TF_AY8: v = p[0]; return v | v << 8 | v << 16 | v << 24;
    case TF_A8: return (uint32_t)p[0] << 24;
    case TF_A8Y8: v = p[0]; return v | v << 8 | v << 16 | (uint32_t)p[1] << 24;
    case TF_1555: case TF_X555:
        v = p[0] | p[1] << 8;
        r = c5((v >> 10) & 31); g = c5((v >> 5) & 31); b = c5(v & 31);
        a = kind == TF_X555 ? 255 : (v & 0x8000) ? 255 : 0;
        return r | g << 8 | b << 16 | a << 24;
    case TF_4444:
        v = p[0] | p[1] << 8;
        r = ((v >> 8) & 15) * 17; g = ((v >> 4) & 15) * 17; b = (v & 15) * 17; a = (v >> 12) * 17;
        return r | g << 8 | b << 16 | a << 24;
    case TF_565:
        v = p[0] | p[1] << 8;
        return c5(v >> 11) | c6((v >> 5) & 63) << 8 | c5(v & 31) << 16 | 0xFF000000u;
    case TF_8888: return p[2] | p[1] << 8 | p[0] << 16 | (uint32_t)p[3] << 24;
    case TF_X888: return p[2] | p[1] << 8 | p[0] << 16 | 0xFF000000u;
    case TF_ABGR: return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    case TF_BGRA: return p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[0] << 24;
    case TF_RGBA: return p[3] | p[2] << 8 | p[1] << 16 | (uint32_t)p[0] << 24;
    case TF_I8: return pal[p[0]];
    default: return 0;
    }
}

static ID3D11ShaderResourceView *create_texture(TexEntry *e, TexFmt f, int levels, const uint32_t *pal) {
    int w = e->w, h = e->h;
    uint32_t src = e->addr;
    D3D11_SUBRESOURCE_DATA init[16];
    void *bufs[16] = {0};
    int dxt = f.kind >= TF_DXT1;
    if (dxt && ((w & 3) || (h & 3))) return NULL;
    int nl = 0;
    for (int l = 0; l < levels && l < 16; l++) {
        int lw = w >> l, lh = h >> l;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        if (dxt) {
            if (l > 0 && (lw < 4 || lh < 4)) break;
            uint32_t bsz = f.kind == TF_DXT1 ? 8 : 16, bw = (lw + 3) / 4, bh = (lh + 3) / 4;
            uint32_t sz = bw * bh * bsz;
            bufs[l] = malloc(sz);
            memcpy(bufs[l], g_mem + (src & 0x03FFFFFF), sz);
            init[l].pSysMem = bufs[l]; init[l].SysMemPitch = bw * bsz; init[l].SysMemSlicePitch = 0;
            src += sz;
        } else {
            uint32_t *out = malloc((size_t)lw * lh * 4);
            const uint8_t *base = g_mem + (src & 0x03FFFFFF);
            if (f.swizzled) {
                uint32_t *xs = malloc(lw * 4), *ys = malloc(lh * 4);
                swizzle_tables(lw, lh, xs, ys);
                for (int y = 0; y < lh; y++)
                    for (int x = 0; x < lw; x++) out[y * lw + x] = texel(f.kind, base + (xs[x] | ys[y]) * f.bpp, pal);
                free(xs); free(ys);
                src += (uint32_t)lw * lh * f.bpp;
            } else {
                uint32_t pitch = e->pitch ? e->pitch : (uint32_t)lw * f.bpp;
                for (int y = 0; y < lh; y++)
                    for (int x = 0; x < lw; x++) out[y * lw + x] = texel(f.kind, base + y * pitch + x * f.bpp, pal);
            }
            if (l == 0) {   /* GT_TEXDUMP=addr,addr,...: write those textures (level 0) to out/tex_<addr>.ppm (RGB + alpha) */
                static const char *td = (const char *)-1;
                if (td == (const char *)-1) td = getenv("GT_TEXDUMP");
                char key[16];
                snprintf(key, sizeof key, "%08X", e->addr);
                if (td && strstr(td, key)) {
                    char path[64];
                    snprintf(path, sizeof path, "out/tex_%s.ppm", key);
                    FILE *fp = fopen(path, "wb");
                    if (fp) {
                        fprintf(fp, "P6 %d %d 255\n", lw * 2, lh);
                        for (int y = 0; y < lh; y++)
                            for (int x = 0; x < lw * 2; x++) {
                                uint32_t px = out[y * lw + (x % lw)];
                                uint8_t a = px >> 24, rgb[3] = {px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF};
                                if (x >= lw) rgb[0] = rgb[1] = rgb[2] = a;
                                fwrite(rgb, 1, 3, fp);
                            }
                        fclose(fp);
                        LOG("[gpu] texdump %s %dx%d kind %d\n", key, lw, lh, f.kind);
                    }
                }
            }
            bufs[l] = out;
            init[l].pSysMem = out; init[l].SysMemPitch = lw * 4; init[l].SysMemSlicePitch = 0;
        }
        nl++;
        if (!f.swizzled) break;   /* linear textures: one level */
    }
    D3D11_TEXTURE2D_DESC d = {0};
    d.Width = w; d.Height = h; d.MipLevels = nl; d.ArraySize = 1;
    d.Format = f.kind == TF_DXT1 ? DXGI_FORMAT_BC1_UNORM : f.kind == TF_DXT3 ? DXGI_FORMAT_BC2_UNORM :
               f.kind == TF_DXT5 ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11ShaderResourceView *srv = NULL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, init, &e->tex)))
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)e->tex, NULL, &srv);
    for (int l = 0; l < 16; l++) free(bufs[l]);
    return srv;
}

static uint32_t tex_bytes(TexFmt f, int w, int h, int levels, uint32_t pitch) {
    if (!f.swizzled && f.kind < TF_DXT1) return (pitch ? pitch : (uint32_t)w * f.bpp) * h;
    uint32_t n = 0;
    for (int l = 0; l < levels; l++) {
        int lw = w >> l, lh = h >> l;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        if (f.kind >= TF_DXT1) n += ((lw + 3) / 4) * ((lh + 3) / 4) * (f.kind == TF_DXT1 ? 8 : 16);
        else n += (uint32_t)lw * lh * f.bpp;
    }
    return n;
}

/* Resolve stage i to an SRV. *rect gets 1 for linear (unnormalized-coordinate) textures, tw/th the size. */
static ID3D11ShaderResourceView *get_texture(int i, int *rect, int *tw, int *th) {
    uint32_t fmtw = KREG(K_TEX_FORMAT(i));
    uint32_t dma = KREG((fmtw & 3) == 2 ? K_DMA_B : K_DMA_A);
    uint32_t addr = (nv2a_dma_base(dma) + KREG(K_TEX_OFFSET(i))) & 0x03FFFFFF;
    uint32_t color = (fmtw >> 8) & 0xFF;
    TexFmt f = tex_format(color);
    *rect = 0;
    if (fmtw & 4) {
        static int warned;
        if (!warned++) LOG("[gpu] cube map textures not supported (format %08X)\n", fmtw);
        return NULL;
    }
    if (f.kind == TF_NONE) {
        static uint32_t warned[8]; static int nw;
        int seen = 0;
        for (int k = 0; k < nw; k++) seen |= warned[k] == color;
        if (!seen && nw < 8) { warned[nw++] = color; LOG("[gpu] unsupported texture format %02X\n", color); }
        return NULL;
    }
    int w, h, levels;
    uint32_t pitch = 0;
    if (f.swizzled || f.kind >= TF_DXT1) {
        w = 1 << ((fmtw >> 20) & 15); h = 1 << ((fmtw >> 24) & 15);
        levels = (fmtw >> 16) & 15;
        if (levels < 1) levels = 1;
    } else {
        uint32_t r = KREG(K_TEX_RECT(i));
        w = r >> 16; h = r & 0xFFFF; levels = 1;
        pitch = KREG(K_TEX_CONTROL1(i)) >> 16;
        *rect = 1;
    }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    *tw = w; *th = h;
    /* render-to-texture: sample the host surface */
    Surface *s = find_rt(addr);
    if (s) { surface_resolve(s); return s->srv; }

    uint32_t pal_addr = 0, pal_len = 0;
    uint32_t palette[256];
    if (f.kind == TF_I8) {
        uint32_t pw = KREG(K_TEX_PALETTE(i));
        pal_len = 256u >> ((pw >> 2) & 3);
        pal_addr = (nv2a_dma_base(KREG((pw & 1) ? K_DMA_B : K_DMA_A)) + (pw & ~0x3Fu)) & 0x03FFFFFF;
    }
    /* per-frame memo: the same binding within one frame reuses the entry without rehashing */
    uint32_t mslot = ((addr >> 6) ^ fmtw ^ (pal_addr >> 6) * 31u) & (TEX_MEMO - 1);
    TexMemo *mm = &tmemo[mslot];
    if (mm->frame == frame_no && mm->addr == addr && mm->fmt == fmtw && mm->pal_addr == pal_addr && mm->pitch == pitch &&
        mm->e && mm->e->srv && mm->e->addr == addr && mm->e->fmt == fmtw)
        return tex_srv(mm->e);
    uint32_t nbytes = tex_bytes(f, w, h, levels, pitch);
    uint64_t hsh = hash_mem(addr, nbytes, 0xCBF29CE484222325ull);
    if (pal_len) hsh = hash_mem(pal_addr, pal_len * 4, hsh);
    uint32_t slot = (uint32_t)((hsh ^ addr * 0x9E3779B1u ^ fmtw) % TEX_CACHE);
    for (int probe = 0; probe < 8; probe++) {
        TexEntry *e = &tcache[(slot + probe) % TEX_CACHE];
        if (e->srv && e->hash == hsh && e->addr == addr && e->fmt == fmtw && e->pitch == pitch && e->w == w && e->h == h &&
            e->pal_addr == pal_addr && e->pal_len == pal_len) {
            e->last_frame = frame_no;
            mm->frame = frame_no; mm->addr = addr; mm->fmt = fmtw; mm->pal_addr = pal_addr; mm->pitch = pitch; mm->e = e;
            return tex_srv(e);
        }
    }
    /* miss: take an empty slot or the least recently used one in the probe window */
    TexEntry *v = &tcache[slot];
    for (int probe = 0; probe < 8; probe++) {
        TexEntry *e = &tcache[(slot + probe) % TEX_CACHE];
        if (!e->srv) { v = e; break; }
        if (e->last_frame < v->last_frame) v = e;
    }
    if (v->srv) { ID3D11ShaderResourceView_Release(v->srv); v->srv = NULL; }
    if (v->tex) { ID3D11Texture2D_Release(v->tex); v->tex = NULL; }
    if (pal_len) {
        for (uint32_t k = 0; k < 256; k++) {
            uint32_t c = k < pal_len ? RD32(pal_addr + k * 4) : 0;   /* A8R8G8B8 */
            palette[k] = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (c & 0xFF000000u);
        }
    }
    v->addr = addr; v->fmt = fmtw; v->pitch = pitch; v->w = w; v->h = h; v->pal_addr = pal_addr; v->pal_len = pal_len;
    v->hash = hsh; v->last_frame = frame_no; v->hd = -1;
    if (f.kind == TF_I8 && f.swizzled && pal_len && hdtex_enabled() && (addr & 0x03FFFFFF) + (uint32_t)w * h <= RAM_SIZE &&
        pal_addr + pal_len * 4 <= RAM_SIZE) {
        uint8_t *lin = malloc((size_t)w * h);
        uint32_t *xs = malloc(w * 4), *ys = malloc(h * 4);
        swizzle_tables(w, h, xs, ys);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) lin[y * w + x] = g_mem[addr + (xs[x] | ys[y])];
        v->hd = hdtex_find(lin, w, h, (const uint32_t *)(g_mem + pal_addr), pal_len);
        free(lin); free(xs); free(ys);
    }
    if (f.kind == TF_I8 && f.swizzled) {   /* GT_TEXRAW=1: dump palettized textures (texture-pack research) */
        static int raw = -1, nraw;
        if (raw < 0) { const char *e = getenv("GT_TEXRAW"); raw = e && *e == '1'; }
        if (raw && nraw < 600) {
            char path[64];
            CreateDirectoryA("out\\texraw", NULL);
            snprintf(path, sizeof path, "out\\texraw\\%03d_%08X.bin", nraw++, addr);
            FILE *fp = fopen(path, "wb");
            if (fp) {   /* w, h, pal_len, then linear (unswizzled) indices, then the raw A8R8G8B8 palette */
                uint32_t hdr[3] = {(uint32_t)w, (uint32_t)h, pal_len};
                fwrite(hdr, 4, 3, fp);
                uint32_t *xs = malloc(w * 4), *ys = malloc(h * 4);
                swizzle_tables(w, h, xs, ys);
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) fputc(g_mem[(addr & 0x03FFFFFF) + (xs[x] | ys[y])], fp);
                free(xs); free(ys);
                fwrite(g_mem + pal_addr, 4, pal_len, fp);
                fclose(fp);
            }
        }
    }
    v->srv = create_texture(v, f, levels, palette);
    mm->frame = frame_no; mm->addr = addr; mm->fmt = fmtw; mm->pal_addr = pal_addr; mm->pitch = pitch; mm->e = v;
    return tex_srv(v);
}

static D3D11_TEXTURE_ADDRESS_MODE addr_mode(uint32_t m) {
    switch (m) {
    case 1: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

typedef struct { uint32_t address, filter, border; ID3D11SamplerState *s; } SamplerEntry;
static SamplerEntry samplers[64]; static int n_samplers;

static ID3D11SamplerState *get_sampler(int i) {
    uint32_t a = KREG(K_TEX_ADDRESS(i)), f = KREG(K_TEX_FILTER(i)) & 0x0FFF0000u, b = KREG(K_TEX_BORDER(i));
    for (int k = 0; k < n_samplers; k++)
        if (samplers[k].address == a && samplers[k].filter == f && samplers[k].border == b) return samplers[k].s;
    uint32_t mn = (f >> 16) & 0xFF, mg = (f >> 24) & 0xF;
    int min_lin = mn == 2 || mn == 4 || mn == 6 || mn == 7, mag_lin = mg != 1, mip_lin = mn == 5 || mn == 6, mips = mn >= 3 && mn <= 6;
    D3D11_SAMPLER_DESC d = {0};
    d.Filter = (D3D11_FILTER)((min_lin ? 0x10 : 0) | (mag_lin ? 0x4 : 0) | (mip_lin ? 0x1 : 0));
    d.AddressU = addr_mode(a & 0xF); d.AddressV = addr_mode((a >> 8) & 0xF); d.AddressW = addr_mode((a >> 16) & 0xF);
    d.MaxAnisotropy = 1;
    if (g_aniso > 1 && min_lin && mag_lin && mips) { d.Filter = D3D11_FILTER_ANISOTROPIC; d.MaxAnisotropy = g_aniso; }
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.BorderColor[0] = ((b >> 16) & 0xFF) / 255.f; d.BorderColor[1] = ((b >> 8) & 0xFF) / 255.f;
    d.BorderColor[2] = (b & 0xFF) / 255.f; d.BorderColor[3] = (b >> 24) / 255.f;
    d.MinLOD = 0; d.MaxLOD = mips ? D3D11_FLOAT32_MAX : 0;
    ID3D11SamplerState *s = NULL;
    ID3D11Device_CreateSamplerState(dev, &d, &s);
    if (n_samplers < 64) { samplers[n_samplers].address = a; samplers[n_samplers].filter = f; samplers[n_samplers].border = b; samplers[n_samplers++].s = s; }
    return s;
}

/* ---------------- register combiners -> HLSL ---------------- */

typedef struct {
    uint32_t ctl, cicw[8], cocw[8], aicw[8], aocw[8], fcw0, fcw1;
    uint8_t stage[4];   /* 0 none, 1 2D, 2 2D rect (unnormalized), 3 pass-through */
} PsKey;

typedef struct { PsKey key; ID3D11PixelShader *ps; } PsEntry;
static PsEntry *ps_cache; static int n_ps, cap_ps;

static const char *reg_name(int r) {
    static const char *n[16] = {"zero", "c0", "c1", "fg", "v0", "v1", "zero", "zero",
                                "t0", "t1", "t2", "t3", "r0", "r1", "sum", "ef"};
    return n[r & 15];
}

/* Combiner input expression. alpha_portion selects .b/.a reads instead of .rgb/.aaa. */
static void input_expr(char *o, size_t n, uint32_t b, int alpha_portion) {
    const char *r = reg_name(b & 15);
    char src[32];
    if (alpha_portion) snprintf(src, sizeof src, "%s.%s", r, (b & 0x10) ? "a" : "b");
    else snprintf(src, sizeof src, "%s.%s", r, (b & 0x10) ? "aaa" : "rgb");
    switch ((b >> 5) & 7) {
    case 0: snprintf(o, n, "saturate(%s)", src); break;
    case 1: snprintf(o, n, "(1 - saturate(%s))", src); break;
    case 2: snprintf(o, n, "(2 * saturate(%s) - 1)", src); break;
    case 3: snprintf(o, n, "(1 - 2 * saturate(%s))", src); break;
    case 4: snprintf(o, n, "(saturate(%s) - 0.5)", src); break;
    case 5: snprintf(o, n, "(0.5 - saturate(%s))", src); break;
    case 6: snprintf(o, n, "(%s)", src); break;
    default: snprintf(o, n, "(-%s)", src); break;
    }
}

static const char *out_op(uint32_t ocw) {
    /* bit 15 bias, bits 17:16 scale (0 x1, 1 x2, 2 x4, 3 x0.5) */
    static const char *ops[8] = {"(x)", "(x*2)", "(x*4)", "(x*0.5)", "(x-0.5)", "((x-0.5)*2)", "((x-0.5)*4)", "((x-0.5)*0.5)"};
    return ops[((ocw >> 16) & 3) | ((ocw >> 13) & 4)];
}

typedef struct { char *p; size_t n, cap; } Str;
static void sp(Str *s, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(s->p + s->n, s->cap - s->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && s->n + (size_t)k < s->cap) { s->n += k; return; }
        s->cap = s->cap ? s->cap * 2 : 8192;
        s->p = realloc(s->p, s->cap);
    }
}

static void emit_op(Str *s, const char *dst, const char *x, uint32_t ocw) {
    char buf[256];
    const char *op = out_op(ocw);
    /* substitute x in op */
    size_t k = 0;
    for (const char *q = op; *q && k < sizeof buf - 64; q++) {
        if (*q == 'x') k += snprintf(buf + k, sizeof buf - k, "%s", x);
        else buf[k++] = *q;
    }
    buf[k] = 0;
    sp(s, "  %s = clamp(%s, -1, 1);\n", dst, buf);
}

static char *gen_ps(const PsKey *k) {
    Str s = {0};
    sp(&s, "Texture2D tx0:register(t0); Texture2D tx1:register(t1); Texture2D tx2:register(t2); Texture2D tx3:register(t3);\n"
           "SamplerState sm0:register(s0); SamplerState sm1:register(s1); SamplerState sm2:register(s2); SamplerState sm3:register(s3);\n"
           "cbuffer P:register(b0) { float4 C0[8]; float4 C1[8]; float4 fogc; float4 at; float4 tsz[4]; };\n"
           "struct PI { float4 pos:SV_Position; float4 d:COLOR0; float4 s:COLOR1; float4 t0:TEXCOORD0; float4 t1:TEXCOORD1;\n"
           "            float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; float fog:FOG; };\n"
           "float4 main(PI i) : SV_Target {\n"
           "  float4 zero = 0, fg = float4(fogc.rgb, i.fog), v0 = i.d, v1 = i.s;\n"
           "  float4 c0 = C0[0], c1 = C1[0];\n");
    for (int t = 0; t < 4; t++) {
        switch (k->stage[t]) {
        case 1: sp(&s, "  float4 t%d = tx%d.Sample(sm%d, i.t%d.xy / (i.t%d.w != 0 ? i.t%d.w : 1));\n", t, t, t, t, t, t); break;
        case 2: sp(&s, "  float4 t%d = tx%d.Sample(sm%d, i.t%d.xy / (i.t%d.w != 0 ? i.t%d.w : 1) * tsz[%d].xy);\n", t, t, t, t, t, t, t); break;
        case 3: sp(&s, "  float4 t%d = saturate(i.t%d);\n", t, t); break;
        default: sp(&s, "  float4 t%d = 0;\n", t); break;
        }
    }
    sp(&s, "  float4 r0 = float4(0, 0, 0, t0.a), r1 = 0, sum = 0, ef = 0;\n");
    int count = k->ctl & 0xFF;
    if (count > 8) count = 8;
    for (int st = 0; st < count; st++) {
        uint32_t ci = k->cicw[st], co = k->cocw[st], ai = k->aicw[st], ao = k->aocw[st];
        char A[96], B[96], C[96], D[96];
        sp(&s, "  {\n");
        if (k->ctl & 0x1000) sp(&s, "  c0 = C0[%d];\n", st);
        if (k->ctl & 0x10000) sp(&s, "  c1 = C1[%d];\n", st);
        /* color portion */
        input_expr(A, sizeof A, ci >> 24, 0); input_expr(B, sizeof B, (ci >> 16) & 0xFF, 0);
        input_expr(C, sizeof C, (ci >> 8) & 0xFF, 0); input_expr(D, sizeof D, ci & 0xFF, 0);
        sp(&s, "  float3 cab = %s, ccd = %s, csum;\n", (co & 0x2000) ? "0" : "0", "0");
        if (co & 0x2000) sp(&s, "  cab = dot(%s, %s).xxx;\n", A, B); else sp(&s, "  cab = %s * %s;\n", A, B);
        if (co & 0x1000) sp(&s, "  ccd = dot(%s, %s).xxx;\n", C, D); else sp(&s, "  ccd = %s * %s;\n", C, D);
        if (co & 0x4000) sp(&s, "  csum = r0.a >= 0.5 ? ccd : cab;\n"); else sp(&s, "  csum = cab + ccd;\n");
        emit_op(&s, "cab", "cab", co); emit_op(&s, "ccd", "ccd", co); emit_op(&s, "csum", "csum", co);
        /* alpha portion */
        input_expr(A, sizeof A, ai >> 24, 1); input_expr(B, sizeof B, (ai >> 16) & 0xFF, 1);
        input_expr(C, sizeof C, (ai >> 8) & 0xFF, 1); input_expr(D, sizeof D, ai & 0xFF, 1);
        sp(&s, "  float aab = %s * %s, acd = %s * %s, asum;\n", A, B, C, D);
        if (ao & 0x4000) sp(&s, "  asum = r0.a >= 0.5 ? acd : aab;\n"); else sp(&s, "  asum = aab + acd;\n");
        emit_op(&s, "aab", "aab", ao); emit_op(&s, "acd", "acd", ao); emit_op(&s, "asum", "asum", ao);
        /* writes (outputs computed first, then stored) */
        uint32_t cd_dst = co & 15, ab_dst = (co >> 4) & 15, sum_dst = (co >> 8) & 15;
        if (cd_dst) sp(&s, "  %s.rgb = ccd;\n", reg_name(cd_dst));
        if (ab_dst) sp(&s, "  %s.rgb = cab;\n", reg_name(ab_dst));
        if (sum_dst) sp(&s, "  %s.rgb = csum;\n", reg_name(sum_dst));
        uint32_t acd_dst = ao & 15, aab_dst = (ao >> 4) & 15, asum_dst = (ao >> 8) & 15;
        if (acd_dst) sp(&s, "  %s.a = acd;\n", reg_name(acd_dst));
        if (aab_dst) sp(&s, "  %s.a = aab;\n", reg_name(aab_dst));
        if (asum_dst) sp(&s, "  %s.a = asum;\n", reg_name(asum_dst));
        if ((co & 0x80000) && ab_dst) sp(&s, "  %s.a = cab.b;\n", reg_name(ab_dst));    /* AB blue to alpha */
        if ((co & 0x40000) && cd_dst) sp(&s, "  %s.a = ccd.b;\n", reg_name(cd_dst));    /* CD blue to alpha */
        sp(&s, "  }\n");
    }
    /* final combiner: rgb = A*B + (1-A)*C + D, alpha = G; sum = r0 + v1, ef = E*F */
    uint32_t f0 = k->fcw0, f1 = k->fcw1;
    char A[96], B[96], C[96], D[96], E[96], F[96], G[96];
    sp(&s, "  sum.rgb = %s;\n", (f1 & 0x80) ? "saturate(r0.rgb + v1.rgb)" : "r0.rgb + v1.rgb");
    input_expr(E, sizeof E, f1 >> 24, 0); input_expr(F, sizeof F, (f1 >> 16) & 0xFF, 0);
    sp(&s, "  ef.rgb = %s * %s;\n", E, F);
    input_expr(A, sizeof A, f0 >> 24, 0); input_expr(B, sizeof B, (f0 >> 16) & 0xFF, 0);
    input_expr(C, sizeof C, (f0 >> 8) & 0xFF, 0); input_expr(D, sizeof D, f0 & 0xFF, 0);
    input_expr(G, sizeof G, (f1 >> 8) & 0xFF, 1);
    sp(&s, "  float4 o;\n  o.rgb = saturate(%s * %s + (1 - %s) * %s + %s);\n  o.a = saturate(%s);\n", A, B, A, C, D, G);
    sp(&s, "  if (at.z > 0) {\n"
           "    float a = round(o.a * 255), r = at.x; int f = (int)at.y; bool p = true;\n"
           "    if (f == 0) p = false; else if (f == 1) p = a < r; else if (f == 2) p = a == r; else if (f == 3) p = a <= r;\n"
           "    else if (f == 4) p = a > r; else if (f == 5) p = a != r; else if (f == 6) p = a >= r;\n"
           "    if (!p) discard;\n"
           "  }\n"
           "  return o;\n}\n");
    return s.p;
}

static ID3D11PixelShader *get_ps(const PsKey *k) {
    for (int i = 0; i < n_ps; i++) if (!memcmp(&ps_cache[i].key, k, sizeof *k)) return ps_cache[i].ps;
    char *src = gen_ps(k);
    ID3DBlob *b = compile(src, "ps_5_0");
    ID3D11PixelShader *ps = NULL;
    if (b) {
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &ps);
        ID3D10Blob_Release(b);
    }
    if (getenv("GT_PSDUMP")) LOG("[gpu] pixel shader %d:\n%s\n", n_ps, src);
    free(src);
    if (n_ps == cap_ps) { cap_ps = cap_ps ? cap_ps * 2 : 64; ps_cache = realloc(ps_cache, cap_ps * sizeof *ps_cache); }
    ps_cache[n_ps].key = *k; ps_cache[n_ps].ps = ps;
    n_ps++;
    LOG("[gpu] pixel shader %d (combiner stages %u)\n", n_ps, k->ctl & 0xFF);
    return ps;
}

/* ---------------- fixed-function state ---------------- */

static D3D11_BLEND blend_factor(uint32_t v, int alpha) {
    switch (v) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t v) {
    switch (v) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_COMPARISON_FUNC cmp_func(uint32_t v) { return (D3D11_COMPARISON_FUNC)(((v - 0x200) & 7) + 1); }

static D3D11_STENCIL_OP stencil_op(uint32_t v) {
    switch (v) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

typedef struct { uint32_t key[8]; void *obj; } StateEntry;
static StateEntry blend_states[256], depth_states[256], rast_states[64];
static int n_blend, n_depth, n_rast;

static void *find_state(StateEntry *t, int n, const uint32_t *key) {
    for (int i = 0; i < n; i++) if (!memcmp(t[i].key, key, 32)) return t[i].obj;
    return NULL;
}
static void add_state(StateEntry *t, int *n, int cap, const uint32_t *key, void *obj) {
    if (*n < cap) { memcpy(t[*n].key, key, 32); t[*n].obj = obj; (*n)++; }
}

static void apply_states(void) {
    /* blend */
    uint32_t bk[8] = {KREG(K_BLEND_EN) & 1, KREG(K_BLEND_SFACTOR), KREG(K_BLEND_DFACTOR), KREG(K_BLEND_EQUATION), KREG(K_COLOR_MASK)};
    ID3D11BlendState *bs = find_state(blend_states, n_blend, bk);
    if (!bs) {
        D3D11_BLEND_DESC d = {0};
        D3D11_RENDER_TARGET_BLEND_DESC *r = &d.RenderTarget[0];
        r->BlendEnable = bk[0];
        r->SrcBlend = blend_factor(bk[1], 0); r->DestBlend = blend_factor(bk[2], 0);
        r->SrcBlendAlpha = blend_factor(bk[1], 1); r->DestBlendAlpha = blend_factor(bk[2], 1);
        r->BlendOp = r->BlendOpAlpha = blend_op(bk[3]);
        uint32_t m = bk[4];
        /* B8G8R8A8 target: write mask bits are R=1 G=2 B=4 A=8 */
        r->RenderTargetWriteMask = ((m >> 16) & 1 ? 1 : 0) | ((m >> 8) & 1 ? 2 : 0) | (m & 1 ? 4 : 0) | ((m >> 24) & 1 ? 8 : 0);
        ID3D11Device_CreateBlendState(dev, &d, &bs);
        add_state(blend_states, &n_blend, 256, bk, bs);
    }
    uint32_t bc = KREG(K_BLEND_COLOR);
    float bf[4] = {((bc >> 16) & 0xFF) / 255.f, ((bc >> 8) & 0xFF) / 255.f, (bc & 0xFF) / 255.f, (bc >> 24) / 255.f};
    ID3D11DeviceContext_OMSetBlendState(ctx, bs, bf, 0xFFFFFFFF);

    /* depth / stencil */
    int has_z = cur_zs != NULL;
    uint32_t dk[8] = {has_z && (KREG(K_DEPTH_TEST_EN) & 1), KREG(K_DEPTH_FUNC), KREG(K_DEPTH_MASK) & 1,
                      has_z && (KREG(K_STENCIL_TEST_EN) & 1), KREG(K_STENCIL_FUNC) | KREG(K_STENCIL_FMASK) << 16,
                      KREG(K_STENCIL_MASK), KREG(K_STENCIL_OP_FAIL) | KREG(K_STENCIL_OP_ZFAIL) << 16, KREG(K_STENCIL_OP_ZPASS)};
    ID3D11DepthStencilState *ds = find_state(depth_states, n_depth, dk);
    if (!ds) {
        D3D11_DEPTH_STENCIL_DESC d = {0};
        d.DepthEnable = dk[0];
        d.DepthWriteMask = dk[2] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = cmp_func(dk[1]);
        d.StencilEnable = dk[3];
        d.StencilReadMask = (UINT8)(dk[4] >> 16);
        d.StencilWriteMask = (UINT8)dk[5];
        d.FrontFace.StencilFunc = cmp_func(dk[4] & 0xFFFF);
        d.FrontFace.StencilFailOp = stencil_op(dk[6] & 0xFFFF);
        d.FrontFace.StencilDepthFailOp = stencil_op(dk[6] >> 16);
        d.FrontFace.StencilPassOp = stencil_op(dk[7]);
        d.BackFace = d.FrontFace;
        ID3D11Device_CreateDepthStencilState(dev, &d, &ds);
        add_state(depth_states, &n_depth, 256, dk, ds);
    }
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds, KREG(K_STENCIL_REF) & 0xFF);

    /* rasterizer. GT_CULL=0 disables culling, GT_CULLFLIP=1 inverts the winding convention (debug). */
    static int cull_on = -1, cull_flip;
    if (cull_on < 0) {
        const char *e = getenv("GT_CULL"); cull_on = !(e && *e == '0');
        e = getenv("GT_CULLFLIP"); cull_flip = e && *e == '1';
    }
    uint32_t ff = KREG(K_FRONT_FACE) ? KREG(K_FRONT_FACE) : 0x900;
    uint32_t rk[8] = {cull_on && (KREG(K_CULL_EN) & 1), KREG(K_CULL_FACE), ff};
    ID3D11RasterizerState *rs = find_state(rast_states, n_rast, rk);
    if (!rs) {
        D3D11_RASTERIZER_DESC d = {0};
        d.FillMode = D3D11_FILL_SOLID; d.DepthClipEnable = TRUE; d.ScissorEnable = FALSE;
        d.CullMode = !rk[0] ? D3D11_CULL_NONE : rk[1] == 0x404 ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
        d.FrontCounterClockwise = (ff == 0x901) ^ cull_flip;
        ID3D11Device_CreateRasterizerState(dev, &d, &rs);
        add_state(rast_states, &n_rast, 64, rk, rs);
    }
    ID3D11DeviceContext_RSSetState(ctx, rs);
}

static void argb_to_f4(uint32_t c, float *o) {
    o[0] = ((c >> 16) & 0xFF) / 255.f; o[1] = ((c >> 8) & 0xFF) / 255.f; o[2] = (c & 0xFF) / 255.f; o[3] = (c >> 24) / 255.f;
}

/* ---------------- draw ---------------- */

static void *ring_map(ID3D11Buffer *b, UINT *pos, UINT cap, UINT bytes) {
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (*pos + bytes > cap) { *pos = 0; mode = D3D11_MAP_WRITE_DISCARD; }
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)b, 0, mode, 0, &m))) return NULL;
    return (uint8_t *)m.pData + *pos;
}

static void draw_impl(int topo, const KVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni) {
    static int nodraw = -1;
    if (nodraw < 0) { const char *e = getenv("GT_NODRAW"); nodraw = e && *e && *e != '0'; }
    if (nodraw || !ready() || !ni) return;
    if ((KREG(K_TRANSFORM_MODE) & 3) == 2) {
        static int warned;
        if (!warned++) LOG("[gpu] vertex programs not implemented; skipping draws\n");
        return;
    }
    if (KREG(K_LIGHTING_EN) & 1) {
        static int warned;
        if (!warned++) LOG("[gpu] fixed-function lighting not implemented (using vertex diffuse)\n");
    }
    if (KREG(K_FOG_ENABLE) & 1) {
        static int warned;
        if (!warned++) LOG("[gpu] fog not implemented (mode %X gen %X color %08X)\n", KREG(K_FOG_MODE),
                           KREG(K_FOG_GEN_MODE), KREG(K_FOG_COLOR));
    }
    if ((size_t)nv * sizeof(KVertex) > VBUF_SIZE || (size_t)ni * 4 > IBUF_SIZE) return;
    bind_targets();
    {   /* GT_DRAWSTACK=texaddr: for draws with that stage-0 texture, log (every 2 s) the guest stack's likely return
         * addresses (.text values from the current guest thread's esp upward; X86Context is GThread's first member) */
        static uint32_t ds = 1;
        static double last;
        if (ds == 1) { const char *e = getenv("GT_DRAWSTACK"); ds = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (ds && KREG(K_TEX_OFFSET(0)) == ds && sched_current()) {
            double now = sched_now_100ns() / 1e7;
            if (now - last > 2.0) {
                last = now;
                X86Context *gc = (X86Context *)sched_current();
                char buf[512];
                int n = 0, k = 0;
                for (uint32_t a = gc->esp; a < gc->esp + 0x800 && k < 20; a += 4) {
                    uint32_t v = RD32(a);
                    if (v >= 0x11000 && v < 0xB0000 && g_mem[v - 5] == 0xE8) { n += snprintf(buf + n, sizeof buf - n, " %X", v); k++; }
                }
                LOG("[drawstack] t=%.1f esp %08X:%s\n", now, gc->esp, buf);
            }
        }
    }
    {   /* GT_DRAWLOG=N: log every draw of frame N (state + first vertex) */
        static int dl = -1;
        if (dl < 0) { const char *e = getenv("GT_DRAWLOG"); dl = e ? atoi(e) : 0; }
        if (dl && (int)frame_no == dl)
            LOG("[draw] t%d nv %u ni %u v0 %.1f %.1f %.4f %.3f d %.2f %.2f %.2f %.2f uv %.3f %.3f | blend %X %X/%X ztest %X zf %X zm %X cmask %08X atest %X/%X/%X stencil %X prog %08X tex0 %08X fmt %08X comb %X c0 %08X cicw %08X cocw %08X\n",
                topo, nv, ni, v[0].a[0][0], v[0].a[0][1], v[0].a[0][2], v[0].a[0][3], v[0].a[3][0], v[0].a[3][1],
                v[0].a[3][2], v[0].a[3][3], v[0].a[9][0], v[0].a[9][1], KREG(K_BLEND_EN), KREG(K_BLEND_SFACTOR),
                KREG(K_BLEND_DFACTOR), KREG(K_DEPTH_TEST_EN), KREG(K_DEPTH_FUNC), KREG(K_DEPTH_MASK), KREG(K_COLOR_MASK),
                KREG(K_ALPHA_TEST_EN), KREG(K_ALPHA_FUNC), KREG(K_ALPHA_REF), KREG(K_STENCIL_TEST_EN),
                KREG(K_SHADER_STAGE_PROG), KREG(K_TEX_OFFSET(0)), KREG(K_TEX_FORMAT(0)), KREG(K_COMB_CONTROL),
                KREG(K_COMB_FACTOR0), KREG(K_COMB_COLOR_ICW), KREG(K_COMB_COLOR_OCW));
    }

    /* vertex constants */
    VSConst vc;
    for (int i = 0; i < 16; i++) vc.M[i] = kf(K_COMPOSITE + i * 4);
    vc.vp[0] = kf(K_VIEWPORT_OFFSET); vc.vp[1] = kf(K_VIEWPORT_OFFSET + 4);
    vc.vp[2] = 2.0f / cur_rt->w; vc.vp[3] = 2.0f / cur_rt->h;
    uint32_t zfmt = (KREG(K_SURF_FORMAT) >> 4) & 0xF;
    vc.zs[0] = 1.0f / (zfmt == 1 ? 65535.0f : 16777215.0f); vc.zs[1] = 1.0f / cur_rt->k; vc.zs[2] = vc.zs[3] = 0;
    for (int s = 0; s < 4; s++) {
        vc.ten[s] = (KREG(K_TEXMAT_EN + s * 4) & 1) ? 1.f : 0.f;
        for (int j = 0; j < 16; j++) vc.T[s * 16 + j] = kf(K_TEXTURE_MATRIX + s * 64 + j * 4);
    }
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)vs_cb, 0, NULL, &vc, 0, 0);

    /* textures + pixel shader key */
    PsKey key;
    memset(&key, 0, sizeof key);
    key.ctl = KREG(K_COMB_CONTROL);
    int count = key.ctl & 0xFF;
    if (count > 8) count = 8;
    for (int st = 0; st < count; st++) {
        key.cicw[st] = KREG(K_COMB_COLOR_ICW + st * 4); key.cocw[st] = KREG(K_COMB_COLOR_OCW + st * 4);
        key.aicw[st] = KREG(K_COMB_ALPHA_ICW + st * 4); key.aocw[st] = KREG(K_COMB_ALPHA_OCW + st * 4);
    }
    key.fcw0 = KREG(K_COMB_SPECFOG_CW0); key.fcw1 = KREG(K_COMB_SPECFOG_CW1);
    PSConst pc;
    memset(&pc, 0, sizeof pc);
    ID3D11ShaderResourceView *srvs[4] = {0};
    ID3D11SamplerState *smps[4] = {0};
    uint32_t prog = KREG(K_SHADER_STAGE_PROG);
    for (int t = 0; t < 4; t++) {
        uint32_t mode = (prog >> (t * 5)) & 31;
        if (!(KREG(K_TEX_CONTROL0(t)) & 0x40000000u) || mode == 0) continue;
        if (mode == 4) { key.stage[t] = 3; continue; }
        if (mode != 1) {
            static uint32_t warned;
            if (!(warned & (1u << mode))) { warned |= 1u << mode; LOG("[gpu] shader stage mode %u not implemented\n", mode); }
            continue;
        }
        int rect = 0, tw = 1, th = 1;
        srvs[t] = get_texture(t, &rect, &tw, &th);
        if (!srvs[t]) continue;
        smps[t] = get_sampler(t);
        key.stage[t] = rect ? 2 : 1;
        pc.tsz[t][0] = 1.0f / tw; pc.tsz[t][1] = 1.0f / th;
    }
    for (int i = 0; i < 8; i++) { argb_to_f4(KREG(K_COMB_FACTOR0 + i * 4), pc.C0[i]); argb_to_f4(KREG(K_COMB_FACTOR1 + i * 4), pc.C1[i]); }
    argb_to_f4(KREG(K_FOG_COLOR), pc.fogc);
    /* fog color is stored as ABGR (R in the low byte) */
    { uint32_t fc = KREG(K_FOG_COLOR); pc.fogc[0] = (fc & 0xFF) / 255.f; pc.fogc[1] = ((fc >> 8) & 0xFF) / 255.f; pc.fogc[2] = ((fc >> 16) & 0xFF) / 255.f; }
    pc.at[0] = (float)(KREG(K_ALPHA_REF) & 0xFF);
    pc.at[1] = (float)((KREG(K_ALPHA_FUNC) - 0x200) & 7);
    pc.at[2] = (KREG(K_ALPHA_TEST_EN) & 1) ? 1.f : 0.f;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)ps_cb, 0, NULL, &pc, 0, 0);
    ID3D11PixelShader *ps = get_ps(&key);
    if (!ps) return;

    apply_states();

    /* geometry */
    UINT vbytes = nv * sizeof(KVertex), ibytes = ni * 4;
    void *vp = ring_map(vbuf, &vbuf_pos, VBUF_SIZE, vbytes);
    if (!vp) return;
    memcpy(vp, v, vbytes);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vbuf, 0);
    UINT voff = vbuf_pos;
    vbuf_pos += (vbytes + 255) & ~255u;
    void *ip = ring_map(ibuf, &ibuf_pos, IBUF_SIZE, ibytes);
    if (!ip) return;
    memcpy(ip, idx, ibytes);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)ibuf, 0);
    UINT ioff = ibuf_pos;
    ibuf_pos += (ibytes + 15) & ~15u;

    UINT stride = sizeof(KVertex);
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vbuf, &stride, &voff);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ibuf, DXGI_FORMAT_R32_UINT, ioff);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, topo == KTOPO_TRIS ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST :
                                                    topo == KTOPO_LINES ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST :
                                                                          D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &vs_cb);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &ps_cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, srvs);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 4, smps);
    ID3D11DeviceContext_DrawIndexed(ctx, ni, 0, 0);
}

/* ---------------- clear / flip ---------------- */

static void clear_impl(uint32_t flags) {
    if (!ready()) return;
    {   /* GT_CLEARLOG=1: log clears (flags, rect, color, target) */
        static int cl = -1;
        if (cl < 0) { const char *e = getenv("GT_CLEARLOG"); cl = e && *e == '1'; }
        if (cl) LOG("[clear] frame %u flags %X rect %08X %08X color %08X rt %08X%c", frame_no, flags, KREG(K_CLEAR_RECT_H),
                    KREG(K_CLEAR_RECT_V), KREG(K_COLOR_CLEAR), KREG(K_COLOR_OFFSET), 10);
    }
    bind_targets();
    if (flags & 0xF0) {
        float c[4];
        argb_to_f4(KREG(K_COLOR_CLEAR), c);
        uint32_t h = KREG(K_CLEAR_RECT_H), v = KREG(K_CLEAR_RECT_V);
        int x0 = h & 0xFFF, x1 = (h >> 16) & 0xFFF, y0 = v & 0xFFF, y1 = (v >> 16) & 0xFFF;
        if (x1 >= cur_rt->w - 1 && y1 >= cur_rt->h - 1 && x0 == 0 && y0 == 0) {
            ID3D11DeviceContext_ClearRenderTargetView(ctx, cur_rt->rtv, c);
        } else {
            ID3D11DeviceContext1 *c1 = NULL;
            if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **)&c1))) {
                int off = (cur_rt->hw - cur_rt->w * GPU_SCALE) / 2;   /* widescreen: the 4:3 area is centered */
                D3D11_RECT r = {off + x0 * GPU_SCALE, y0 * GPU_SCALE, off + (x1 + 1) * GPU_SCALE, (y1 + 1) * GPU_SCALE};
                if (cur_rt->k > 1 && x0 <= 32 && x1 >= cur_rt->w - 33) { r.left = 0; r.right = cur_rt->hw; }
                ID3D11DeviceContext1_ClearView(c1, (ID3D11View *)cur_rt->rtv, c, &r, 1);
                ID3D11DeviceContext1_Release(c1);
            } else ID3D11DeviceContext_ClearRenderTargetView(ctx, cur_rt->rtv, c);
        }
    }
    if ((flags & 3) && cur_zs) {
        uint32_t zv = KREG(K_ZSTENCIL_CLEAR);
        uint32_t zfmt = (KREG(K_SURF_FORMAT) >> 4) & 0xF;
        float z = zfmt == 1 ? (zv & 0xFFFF) / 65535.f : (zv >> 8) / 16777215.f;
        UINT f = ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, cur_zs->dsv, f, z, (UINT8)(zv & 0xFF));
    }
}

static DWORD last_flip_tick;

/* Clear the back buffer and draw srv into the 4:3 letterboxed screen area; (x,y,dw,dh) is the destination
 * in 640x480 screen space. */
static void present_srv(ID3D11ShaderResourceView *srv, float x, float y, float dw, float dh) {
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    UINT cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    if (cw && ch && (cw != bb_w || ch != bb_h)) {
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        ID3D11RenderTargetView_Release(bb_rtv);
        IDXGISwapChain_ResizeBuffers(swap, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
        make_bb_rtv();
        LOG("[gpu] back buffer %ux%u\n", bb_w, bb_h);
    }
    float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, bb_rtv, black);
    float w = (float)bb_w, h = (float)bb_h, vw = w, vh = w * 3 / 4;
    if (vh > h) { vh = h; vw = h * 4 / 3; }
    float sx = vw / 640, sy = vh / 480;
    D3D11_VIEWPORT vp = {(w - vw) / 2 + x * sx, (h - vh) / 2 + y * sy, dw * sx, dh * sy, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &bb_rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    {   /* scissor to the 4:3 screen area: a cropped (overscan) destination extends past it */
        static ID3D11RasterizerState *rs_scissor;
        if (!rs_scissor) {
            D3D11_RASTERIZER_DESC d = {0};
            d.FillMode = D3D11_FILL_SOLID; d.CullMode = D3D11_CULL_NONE; d.DepthClipEnable = TRUE; d.ScissorEnable = TRUE;
            ID3D11Device_CreateRasterizerState(dev, &d, &rs_scissor);
        }
        D3D11_RECT sr = {(LONG)((w - vw) / 2), (LONG)((h - vh) / 2), (LONG)((w + vw) / 2), (LONG)((h + vh) / 2)};
        ID3D11DeviceContext_RSSetState(ctx, rs_scissor);
        ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sr);
    }
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &blit_sampler);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &post_cb);
    if (!srv) return;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11ShaderResourceView *null_srv = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &null_srv);
}

/* Widescreen present: the target's 480 lines (or the 448-line safe area with overscan) fill the window height,
 * square pixels, centered horizontally. */
static void present_wide(ID3D11ShaderResourceView *srv, float k, int overscan) {
    present_srv(NULL, 0, 0, 0, 0);   /* resize + clear + bind the back buffer */
    float w = (float)bb_w, h = (float)bb_h;
    float dh = overscan ? h * 480.f / 448.f : h, dy = overscan ? -h * 16.f / 448.f : 0, dw = dh * 640.f * k / 480.f;
    D3D11_VIEWPORT vp = {(w - dw) / 2, dy, dw, dh, 0, 1};
    D3D11_RECT sr = {0, 0, (LONG)bb_w, (LONG)bb_h};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sr);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11ShaderResourceView *null_srv = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &null_srv);
}

/* GT_SHOT=N: write out/shots/fNNNNN.bmp every N frames (the flipped surface, guest resolution x GPU_SCALE). */
static void screenshot_tex(ID3D11Texture2D *src);
static void screenshot(Surface *s) { screenshot_tex(s->tex); }
/* GT_SHOTBB=1: GT_SHOT saves the presented back buffer (window size, after post-processing) instead. */
static void screenshot_tex(ID3D11Texture2D *src) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(src, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        char path[MAX_PATH];
        const char *dir = getenv("GT_SHOTDIR") ? getenv("GT_SHOTDIR") : "out\\shots";   /* GT_SHOTDIR=<dir> */
        CreateDirectoryA(dir, NULL);
        snprintf(path, sizeof path, "%s\\f%05u_t%05.1f.bmp", dir, frame_no, sched_now_100ns() / 1e7);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint32_t w = d.Width, h = d.Height, sz = w * h * 4;
            uint8_t hdr[54] = {'B', 'M'};
            uint32_t v;
            v = 54 + sz; memcpy(hdr + 2, &v, 4);
            v = 54; memcpy(hdr + 10, &v, 4);
            v = 40; memcpy(hdr + 14, &v, 4);
            memcpy(hdr + 18, &w, 4);
            int32_t nh = -(int32_t)h; memcpy(hdr + 22, &nh, 4);
            hdr[26] = 1; hdr[28] = 32;
            memcpy(hdr + 34, &sz, 4);
            fwrite(hdr, 1, 54, f);
            for (uint32_t y = 0; y < h; y++) fwrite((uint8_t *)m.pData + y * m.RowPitch, 1, w * 4, f);
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

static void flip_impl(uint32_t addr) {
    if (!ready()) return;
    frame_no++;
    if (frame_no % 30 == 0 && hdtex_over_budget()) {
        /* HD textures past the VRAM budget: drop cache entries idle for 3 s so their HD views can be freed */
        for (int i = 0; i < TEX_CACHE; i++) {
            TexEntry *e = &tcache[i];
            if (!e->srv || frame_no - e->last_frame < 90) continue;
            ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL;
            if (e->tex) { ID3D11Texture2D_Release(e->tex); e->tex = NULL; }
            e->hd = -1;
        }
        hdtex_trim(dev);
    }
    Surface *s = find_rt(addr);
    if (s) surface_resolve(s);
    static int shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("GT_SHOT"); shot_every = e ? atoi(e) : 0; }
    static int shot_bb = -1;
    if (shot_bb < 0) { const char *e = getenv("GT_SHOTBB"); shot_bb = e && *e == '1'; }
    if (s && shot_every > 0 && frame_no % shot_every == 0 && !shot_bb) screenshot(s);
    if (s) {
        /* TV overscan: the game draws 2D (HUD, loading screens, cutscene bars) inside the title-safe viewport
         * x 32..611, y 16..463 and its clears cover only that rect, so the border keeps stale frames that a TV hides.
         * Show the safe area scaled to the window; GT_OVERSCAN=0 shows the whole 640x480 surface. */
        static int overscan = -1;
        if (overscan < 0) { const char *e = getenv("GT_OVERSCAN"); overscan = !(e && *e == '0'); }
        if (s->k > 1)
            present_wide(s->srv, s->k, overscan);
        else if (overscan && s->w == 640 && s->h == 480)
            present_srv(s->srv, -32.f * 640 / 580, -16.f * 480 / 448, 640.f * 640 / 580, 480.f * 480 / 448);
        else
            present_srv(s->srv, 0, 0, 640, 480);
    } else {
        static int warned;
        if (warned++ < 4) LOG("[gpu] flip: no surface at %08X\n", addr);
    }
    if (s && shot_bb && shot_every > 0 && frame_no % shot_every == 0) {
        ID3D11Texture2D *bb = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
            screenshot_tex(bb);
            ID3D11Texture2D_Release(bb);
        }
    }
    IDXGISwapChain_Present(swap, 0, 0);
    last_flip_tick = GetTickCount();
    if (frame_no % 300 == 0) {
        static DWORD t0;
        DWORD t = GetTickCount();
        LOG("[gpu] frame %u: %.1f fps (%d pixel shaders, %d render targets)\n", frame_no,
            t0 ? 300000.0 / (double)(t - t0) : 0.0, n_ps, n_rts);
        t0 = t;
    }
}

/* ---------------- overlay (WMV video plane) ---------------- */

static ID3D11Texture2D *ov_tex;
static ID3D11ShaderResourceView *ov_srv;
static uint32_t ov_w, ov_h, ov_last_frames;

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void vblank_impl(void) {
    NvOverlay o;
    if (!nv2a_overlay(&o)) return;
    if (GetTickCount() - last_flip_tick < 250) return;      /* 3D frames own the screen */
    if (o.frames == ov_last_frames) return;
    ov_last_frames = o.frames;
    if (!ready()) return;
    if (o.w > 2048 || o.h > 2048 || (uint64_t)o.addr + (uint64_t)o.pitch * o.h > RAM_SIZE) return;
    if (!ov_tex || ov_w != o.w || ov_h != o.h) {
        if (ov_tex) { ID3D11ShaderResourceView_Release(ov_srv); ID3D11Texture2D_Release(ov_tex); }
        D3D11_TEXTURE2D_DESC d = {0};
        d.Width = o.w; d.Height = o.h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateTexture2D(dev, &d, NULL, &ov_tex);
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)ov_tex, NULL, &ov_srv);
        ov_w = o.w; ov_h = o.h;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)ov_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    for (uint32_t y = 0; y < o.h; y++) {
        const uint8_t *src = g_mem + o.addr + y * o.pitch;     /* YUY2: Y0 U Y1 V */
        uint8_t *dst = (uint8_t *)m.pData + y * m.RowPitch;
        for (uint32_t x = 0; x + 1 < o.w; x += 2, src += 4, dst += 8) {
            int u = src[1] - 128, v = src[3] - 128;
            for (int k = 0; k < 2; k++) {
                int c = (src[k * 2] - 16) * 298;
                dst[k * 4 + 0] = clamp8((c + 409 * v + 128) >> 8);
                dst[k * 4 + 1] = clamp8((c - 100 * u - 208 * v + 128) >> 8);
                dst[k * 4 + 2] = clamp8((c + 516 * u + 128) >> 8);
                dst[k * 4 + 3] = 255;
            }
        }
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)ov_tex, 0);
    float ow = (float)(o.ow ? o.ow : 640), oh = (float)(o.oh ? o.oh : 480);
    present_srv(ov_srv, (float)o.ox, (float)o.oy, ow, oh);
    IDXGISwapChain_Present(swap, 0, 0);
    static int shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("GT_SHOT"); shot_every = e ? atoi(e) : 0; }
    if (shot_every > 0 && o.frames % shot_every == 0) {
        /* dump the decoded overlay frame */
        char path[64];
        CreateDirectoryA("out\\shots", NULL);
        snprintf(path, sizeof path, "out\\shots\\ov%05u.ppm", o.frames);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", o.w, o.h);
            for (uint32_t y = 0; y < o.h; y++) {
                const uint8_t *src = g_mem + o.addr + y * o.pitch;
                for (uint32_t x = 0; x + 1 < o.w; x += 2, src += 4) {
                    int u = src[1] - 128, v = src[3] - 128;
                    for (int k = 0; k < 2; k++) {
                        int c = (src[k * 2] - 16) * 298;
                        uint8_t px[3] = {clamp8((c + 409 * v + 128) >> 8), clamp8((c - 100 * u - 208 * v + 128) >> 8),
                                         clamp8((c + 516 * u + 128) >> 8)};
                        fwrite(px, 1, 3, f);
                    }
                }
            }
            fclose(f);
        }
    }
}

/* ---------------- public entry points (serialized: pusher thread and DPC thread) ---------------- */

static SRWLOCK gpu_lock = SRWLOCK_INIT;

void gpu_draw(int topo, const KVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni) {
    AcquireSRWLockExclusive(&gpu_lock); draw_impl(topo, v, nv, idx, ni); ReleaseSRWLockExclusive(&gpu_lock);
}
void gpu_clear(uint32_t flags) {
    AcquireSRWLockExclusive(&gpu_lock); clear_impl(flags); ReleaseSRWLockExclusive(&gpu_lock);
}
void gpu_flip(uint32_t addr) {
    AcquireSRWLockExclusive(&gpu_lock); flip_impl(addr); ReleaseSRWLockExclusive(&gpu_lock);
}
void gpu_vblank(void) {
    AcquireSRWLockExclusive(&gpu_lock); vblank_impl(); ReleaseSRWLockExclusive(&gpu_lock);
}
