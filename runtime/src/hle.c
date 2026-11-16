/* Host implementations of guest library functions (recomp/hle.txt).
 * XPP input: the game sees one gamepad on port 0, fed by the host's first XInput controller
 * (and the keyboard once there is a window).
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

/* stdcall return: pop return address + nargs, set eax */
static void ret_std(X86Context *c, int nargs, uint32_t eax) {
    c->eax = eax;
    c->esp += 4 + 4 * nargs;
}

/* ---- host XInput (loaded dynamically so the exe runs without it) ---- */
typedef struct {
    WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} HostPad;
typedef struct { DWORD dwPacketNumber; HostPad Gamepad; } HostState;
static DWORD (WINAPI *host_xinput_get_state)(DWORD, HostState *);

static void host_xinput_init(void) {
    static int done;
    if (done++) return;
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput9_1_0.dll");
    if (m) host_xinput_get_state = (void *)GetProcAddress(m, "XInputGetState");
}

#define PAD_HANDLE 0x7A000000u   /* port in low bits */
#define ERROR_DEVICE_NOT_CONNECTED_ 1167

static uint32_t packet;

/* ---- scripted input: GT_INPUT="t:button[:hold],..." (t and hold in guest seconds, hold default 0.2) ----
 * buttons: up down left right start back a b x y black white lt rt lthumb rthumb,
 * left stick: su sd sl sr (full deflection) */
typedef struct { double t, hold; uint16_t digital; int analog; int stick; } ScriptEv;
static ScriptEv script[256];
static int n_script = -1;

static void script_init(void) {
    n_script = 0;
    const char *e = getenv("GT_INPUT");
    if (!e) return;
    static char buf[8192];
    strncpy(buf, e, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *tok = strtok(buf, ","); tok && n_script < 256; tok = strtok(NULL, ",")) {
        char name[16] = {0};
        double t = 0, hold = 0.2;
        if (sscanf(tok, "%lf:%15[a-z]:%lf", &t, name, &hold) < 2) continue;
        static const char *dn[] = {"up", "down", "left", "right", "start", "back", "lthumb", "rthumb"};
        static const char *an[] = {"a", "b", "x", "y", "black", "white", "lt", "rt"};
        ScriptEv *ev = &script[n_script];
        static const char *sn[] = {"su", "sd", "sl", "sr"};
        ev->t = t; ev->hold = hold; ev->digital = 0; ev->analog = -1; ev->stick = -1;
        for (int i = 0; i < 8; i++) if (!strcmp(name, dn[i])) ev->digital = (uint16_t)(1u << i);
        for (int i = 0; i < 8; i++) if (!strcmp(name, an[i])) ev->analog = i;
        for (int i = 0; i < 4; i++) if (!strcmp(name, sn[i])) ev->stick = i;
        if (ev->digital || ev->analog >= 0 || ev->stick >= 0) n_script++;
        else LOG("[input] unknown button '%s'\n", name);
    }
    LOG("[input] %d scripted events\n", n_script);
}

/* keyboard, only while one of our windows has focus */
static int our_focus(void) {
    DWORD pid = 0;
    HWND f = GetForegroundWindow();
    if (!f) return 0;
    GetWindowThreadProcessId(f, &pid);
    return pid == GetCurrentProcessId();
}
static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

/* XINPUT_STATE: dwPacketNumber, wButtons, bAnalogButtons[8] (A B X Y BLACK WHITE LT RT), sThumbLX LY RX RY */
static void fill_state(uint32_t dst, int port) {
    HostState h;
    memset(&h, 0, sizeof h);
    host_xinput_init();
    if (host_xinput_get_state) host_xinput_get_state((DWORD)port, &h);
    uint16_t b = h.Gamepad.wButtons;
    uint16_t buttons = (b & 0x000F)              /* dpad up/down/left/right: same bits */
                     | (b & 0x0010 ? 0x10 : 0)    /* START */
                     | (b & 0x0020 ? 0x20 : 0)    /* BACK */
                     | (b & 0x0040 ? 0x40 : 0)    /* LTHUMB */
                     | (b & 0x0080 ? 0x80 : 0);   /* RTHUMB */
    uint8_t analog[8] = {
        b & 0x1000 ? 255 : 0, b & 0x2000 ? 255 : 0, b & 0x4000 ? 255 : 0, b & 0x8000 ? 255 : 0,
        b & 0x0200 ? 255 : 0, b & 0x0100 ? 255 : 0, h.Gamepad.bLeftTrigger, h.Gamepad.bRightTrigger,
    };
    /* keyboard: arrows = dpad, Enter = START, Backspace = BACK, Z/X/C/V = A/B/X/Y, Q/E = LT/RT, WASD = left stick */
    if (port == 0 && our_focus()) {
        if (key(VK_UP)) buttons |= 1;
        if (key(VK_DOWN)) buttons |= 2;
        if (key(VK_LEFT)) buttons |= 4;
        if (key(VK_RIGHT)) buttons |= 8;
        if (key(VK_RETURN)) buttons |= 0x10;
        if (key(VK_BACK)) buttons |= 0x20;
        static const int ak[8] = {'Z', 'X', 'C', 'V', 'F', 'G', 'Q', 'E'};
        for (int i = 0; i < 8; i++) if (key(ak[i])) analog[i] = 255;
        if (key('A')) h.Gamepad.sThumbLX = -32767;
        if (key('D')) h.Gamepad.sThumbLX = 32767;
        if (key('W')) h.Gamepad.sThumbLY = 32767;
        if (key('S')) h.Gamepad.sThumbLY = -32767;
    }
    if (n_script < 0) script_init();
    if (port == 0 && n_script > 0) {
        double now = sched_now_100ns() / 1e7;
        for (int i = 0; i < n_script; i++) {
            if (now < script[i].t || now >= script[i].t + script[i].hold) continue;
            buttons |= script[i].digital;
            if (script[i].analog >= 0) analog[script[i].analog] = 255;
            switch (script[i].stick) {
            case 0: h.Gamepad.sThumbLY = 32767; break;
            case 1: h.Gamepad.sThumbLY = -32767; break;
            case 2: h.Gamepad.sThumbLX = -32767; break;
            case 3: h.Gamepad.sThumbLX = 32767; break;
            }
        }
    }
    WR32(dst, ++packet);
    WR16(dst + 4, buttons);
    memcpy(g_mem + dst + 6, analog, 8);
    WR16(dst + 14, (uint16_t)h.Gamepad.sThumbLX);
    WR16(dst + 16, (uint16_t)h.Gamepad.sThumbLY);
    WR16(dst + 18, (uint16_t)h.Gamepad.sThumbRX);
    WR16(dst + 20, (uint16_t)h.Gamepad.sThumbRY);
}

static int pad_reported;   /* port 0 insertion reported by XGetDeviceChanges */

void x86_hle(X86Context *c, uint32_t va) {
    sched_maybe_yield();
    switch (va) {
    case 0x86390: { /* int PlayerNumCrystals(int player, int type), cdecl (TOWER.OBJ).
                     * Players at 0xBB4F60 (stride 0x6140); count words at +0xF52 indexed by save slot [+0xC] * 0x12A
                     * + type; a negative count means "has all" (the per-type maximum at 0x129B0C).
                     * GT_CRYSTALS=1 (test cheat) sets each queried count to the maximum, which opens the Tower gates
                     * (PlayerHasAllCrystals 863D0 reads the same words). */
        static int cheat = -1;
        if (cheat < 0) { const char *e = getenv("GT_CRYSTALS"); cheat = e ? atoi(e) : 0; }
        uint32_t p = 0xBB4F60 + ARG(0) * 0x6140, type = ARG(1);
        uint32_t w = p + 0xF52 + (RD32(p + 0xC) * 0x12A + type) * 2;
        int max = (int)RD32(0x129B0C + type * 4);
        int v = (int16_t)RD16(w);
        if (cheat && v >= 0 && v < max) WR16(w, (uint16_t)(v = max));
        if (v < 0) v = max;
        c->eax = (uint32_t)v;
        c->esp += 4;
        return;
    }
    case 0xB1B72: /* DWORD GetTickCount(void): polled in timing loops, so it yields (above) */
        c->eax = (uint32_t)(sched_now_100ns() / 10000);
        c->esp += 4;
        return;
    case 0x10F49F: /* VOID XInitDevices(DWORD n, PXDEVICE_PREALLOC_TYPE types) */
        ret_std(c, 2, 0);
        return;
    case 0x10F4A4: /* DWORD XGetDevices(PXPP_DEVICE_TYPE type) */
        ret_std(c, 1, 1);
        return;
    case 0x10F4C6: { /* BOOL XGetDeviceChanges(type, PDWORD inserted, PDWORD removed) */
        uint32_t ins = ARG(1), rem = ARG(2);
        int changed = !pad_reported;
        if (ins) WR32(ins, changed ? 1 : 0);
        if (rem) WR32(rem, 0);
        pad_reported = 1;
        ret_std(c, 3, changed);
        return;
    }
    case 0x10F199: /* HANDLE XInputOpen(type, DWORD port, DWORD slot, PXINPUT_POLLING_PARAMETERS) */
        ret_std(c, 4, ARG(1) == 0 ? PAD_HANDLE : 0);
        return;
    case 0x10F1EF: /* VOID XInputClose(HANDLE) */
        ret_std(c, 1, 0);
        return;
    case 0x10F1FB: { /* DWORD XInputGetCapabilities(HANDLE, PXINPUT_CAPABILITIES) */
        uint32_t caps = ARG(1);
        memset(g_mem + caps, 0, 26);
        WR8(caps, 1);                        /* SubType: gamepad */
        WR16(caps + 4, 0x00FF);              /* In.Gamepad: every digital button */
        memset(g_mem + caps + 6, 0xFF, 8);   /* analog buttons: full range */
        WR16(caps + 14, 0xFFFF); WR16(caps + 16, 0xFFFF); WR16(caps + 18, 0xFFFF); WR16(caps + 20, 0xFFFF);
        WR16(caps + 22, 0xFFFF); WR16(caps + 24, 0xFFFF); /* Out: rumble motors */
        ret_std(c, 2, (ARG(0) & ~3u) == PAD_HANDLE ? 0 : ERROR_DEVICE_NOT_CONNECTED_);
        return;
    }
    case 0x10F3D8: { /* DWORD XInputGetState(HANDLE, PXINPUT_STATE) */
        uint32_t h = ARG(0);
        if ((h & ~3u) != PAD_HANDLE) { ret_std(c, 2, ERROR_DEVICE_NOT_CONNECTED_); return; }
        fill_state(ARG(1), (int)(h & 3));
        ret_std(c, 2, 0);
        return;
    }
    default:
        if (dsound_hle(c, va)) return;
        rt_fatal("no HLE implementation for %08X", va);
    }
}
