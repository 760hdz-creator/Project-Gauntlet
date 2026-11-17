/* Host implementations of guest library functions (recomp/hle.txt).
 * XPP input: the game sees GT_PADS gamepads (default 1) on ports 0..N-1, fed by the host's XInput controllers
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
static DWORD (WINAPI *host_xinput_set_state)(DWORD, WORD *);   /* XINPUT_VIBRATION: left, right */

static void host_xinput_init(void) {
    static int done;
    if (done++) return;
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput9_1_0.dll");
    if (m) host_xinput_get_state = (void *)GetProcAddress(m, "XInputGetState");
    if (m) host_xinput_set_state = (void *)GetProcAddress(m, "XInputSetState");
}

#define PAD_HANDLE 0x7A000000u   /* port in low bits */
#define ERROR_DEVICE_NOT_CONNECTED_ 1167

static uint32_t packet;

/* ---- scripted input: GT_INPUT="t:button[:hold[:port]],..." (guest seconds; hold default 0.2; port 1-4, default 1) ----
 * buttons: up down left right start back a b x y black white lt rt lthumb rthumb,
 * left stick: su sd sl sr (full deflection); "exit" = test cheat (player 0 stands on an exit, see 0x51100).
 * "@VA[:min]" (VA = a function listed in recomp/hooks.txt) is an anchor: the times of the events after it count
 * from the first call of that function after the previous anchor fired, and at least min seconds after it (or
 * after t=0 for the first anchor); until then those events are inactive. */
typedef struct { double t, hold; uint16_t digital; int analog; int stick; int cheat; int anchor; int port; } ScriptEv;
static ScriptEv script[1024];
static int n_script = -1;
typedef struct { uint32_t va; double t, min; int armed, fired; } Anchor;
static Anchor anchors[64];
static int n_anchors;

static void script_init(void) {
    n_script = 0;
    const char *e = getenv("GT_INPUT");
    if (!e) return;
    static char buf[32768];
    strncpy(buf, e, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    int anchor = -1;
    for (char *tok = strtok(buf, ","); tok && n_script < 1024; tok = strtok(NULL, ",")) {
        char name[16] = {0};
        double t = 0, hold = 0.2;
        unsigned va;
        double min = 0;
        if (tok[0] == '@' && sscanf(tok + 1, "%x:%lf", &va, &min) >= 1 && n_anchors < 64) {
            anchors[n_anchors] = (Anchor){va, 0, min, n_anchors == 0, 0};
            anchor = n_anchors++;
            continue;
        }
        int port = 1;
        if (sscanf(tok, "%lf:%15[a-z]:%lf:%d", &t, name, &hold, &port) < 2) continue;
        static const char *dn[] = {"up", "down", "left", "right", "start", "back", "lthumb", "rthumb"};
        static const char *an[] = {"a", "b", "x", "y", "black", "white", "lt", "rt"};
        ScriptEv *ev = &script[n_script];
        static const char *sn[] = {"su", "sd", "sl", "sr"};
        ev->t = t; ev->hold = hold; ev->digital = 0; ev->analog = -1; ev->stick = -1; ev->cheat = 0;
        ev->anchor = anchor;
        ev->port = port - 1;
        if (!strcmp(name, "exit")) ev->cheat = 1;
        for (int i = 0; i < 8; i++) if (!strcmp(name, dn[i])) ev->digital = (uint16_t)(1u << i);
        for (int i = 0; i < 8; i++) if (!strcmp(name, an[i])) ev->analog = i;
        for (int i = 0; i < 4; i++) if (!strcmp(name, sn[i])) ev->stick = i;
        if (ev->digital || ev->analog >= 0 || ev->stick >= 0 || ev->cheat) n_script++;
        else LOG("[input] unknown button '%s'\n", name);
    }
    LOG("[input] %d scripted events, %d anchors\n", n_script, n_anchors);
}

/* entry hook (recomp/hooks.txt): fires the armed anchor on this VA and arms the next one */
void x86_hook(X86Context *c, uint32_t va) {
    (void)c;
    if (n_script < 0) script_init();
    for (int i = 0; i < n_anchors; i++) {
        if (!anchors[i].armed || anchors[i].fired || anchors[i].va != va) continue;
        double now = sched_now_100ns() / 1e7;
        if (now < (i ? anchors[i - 1].t : 0) + anchors[i].min) continue;
        anchors[i].fired = 1;
        anchors[i].t = sched_now_100ns() / 1e7;
        if (i + 1 < n_anchors) anchors[i + 1].armed = 1;
        LOG("[input] anchor %d (@%X) at t=%.1f\n", i, va, anchors[i].t);
    }
}

static int ev_active(const ScriptEv *ev, double now) {
    double base = 0;
    if (ev->anchor >= 0) {
        if (!anchors[ev->anchor].fired) return 0;
        base = anchors[ev->anchor].t;
    }
    return now >= base + ev->t && now < base + ev->t + ev->hold;
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
    if (n_script > 0) {
        double now = sched_now_100ns() / 1e7;
        for (int i = 0; i < n_script; i++) {
            if (script[i].port != port || !ev_active(&script[i], now)) continue;
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

/* exit test cheat: F8 held (window focused) or a scripted "exit" event active */
static int exit_cheat(void) {
    if (our_focus() && key(VK_F8)) return 1;
    if (n_script < 0) script_init();
    double now = sched_now_100ns() / 1e7;
    for (int i = 0; i < n_script; i++)
        if (script[i].cheat && ev_active(&script[i], now)) return 1;
    return 0;
}

static int pad_reported;   /* insertions reported by XGetDeviceChanges */

/* GT_PADS=N: gamepads on ports 0..N-1 (1..4, default 1) */
static uint32_t pad_mask(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("GT_PADS");
        n = e ? atoi(e) : 1;
        if (n < 1) n = 1;
        if (n > 4) n = 4;
    }
    return (1u << n) - 1;
}
static int pad_ok(uint32_t h) { return (h & ~3u) == PAD_HANDLE && (pad_mask() >> (h & 3) & 1); }

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
    case 0x51100: { /* int player_collis_exit(Player *p), cdecl (ITEMS.OBJ): the exit number the player stands on.
                     * Default -1 when [BD4C10] != 0, else 0. p+0xA10 = touched item; an exit item has type
                     * [[item]] == 9 (subtype [+4] 0x32 excluded) and its number at +0xDC (int16). Items at [BD48F8]
                     * (stride 0xF0, count [BD4ACC], +0xC4 == -1 = unused). The debug sound call under
                     * [BD4D60] & 0x8000 is skipped.
                     * Test cheat (F8 / scripted "exit"): on press, player 0 is moved onto the first active exit.
                     * Items and players keep their position at +0x44 (x, y up, z); players also keep pos + (0, 4.4, 0)
                     * at +0x54 and pos + (0, 2.5, 0) at +0x64, which move by the same delta. Collision then finds the
                     * exit, and ProcessItems / do_players run the real exit sequence. */
        uint32_t p = ARG(0);
        int r = RD32(0xBD4C10) ? -1 : 0;
        uint32_t it = RD32(p + 0xA10);
        if (it) {
            uint32_t d = RD32(it);
            if (RD32(d) == 9 && RD32(d + 4) != 0x32) r = (int16_t)RD16(it + 0xDC);
        }
        if (p == 0xBB4F60 && exit_cheat()) {
            uint32_t a = RD32(0xBD48F8);
            int n = (int)RD32(0xBD4ACC);
            for (int i = 0; i < n; i++, a += 0xF0) {
                if ((int16_t)RD16(a + 0xC4) == -1 || RD32(RD32(a)) != 9 || RD32(RD32(a) + 4) == 0x32) continue;
                WR32(p + 0xA10, a);
                WR32(a + 0xE0, RD32(a + 0xE0) | 1);
                r = (int16_t)RD16(a + 0xDC);
                static int logged;
                if (logged++ % 15 == 0) LOG("[cheat] touch exit %d e8=%d 356=%d c10=%d\n", r, RD32(p + 0xE8),
                                            (int16_t)RD16(p + 0x356), RD32(0xBD4C10));
                break;
            }
        }
        c->eax = (uint32_t)r;
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
        ret_std(c, 1, pad_mask());
        return;
    case 0x10F4C6: { /* BOOL XGetDeviceChanges(type, PDWORD inserted, PDWORD removed) */
        uint32_t ins = ARG(1), rem = ARG(2);
        int changed = !pad_reported;
        if (ins) WR32(ins, changed ? pad_mask() : 0);
        if (rem) WR32(rem, 0);
        pad_reported = 1;
        ret_std(c, 3, changed);
        return;
    }
    case 0x10F447: { /* DWORD XInputSetState(HANDLE, PXINPUT_FEEDBACK): rumble. XINPUT_FEEDBACK_HEADER = dwStatus,
                      * hEvent, Reserved[58]; then wLeftMotorSpeed (+0x42), wRightMotorSpeed (+0x44). The game skips a
                      * new request while dwStatus == ERROR_IO_PENDING (997), so the request completes at once. */
        uint32_t h = ARG(0), fb = ARG(1);
        host_xinput_init();
        if (host_xinput_set_state) {
            WORD v[2] = {RD16(fb + 0x42), RD16(fb + 0x44)};
            host_xinput_set_state((h - PAD_HANDLE) & 3, v);
        }
        WR32(fb, 0);
        ret_std(c, 2, 997);
        return;
    }
    case 0x10F199: /* HANDLE XInputOpen(type, DWORD port, DWORD slot, PXINPUT_POLLING_PARAMETERS) */
        ret_std(c, 4, ARG(1) < 4 && (pad_mask() >> ARG(1) & 1) ? PAD_HANDLE | ARG(1) : 0);
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
        ret_std(c, 2, pad_ok(ARG(0)) ? 0 : ERROR_DEVICE_NOT_CONNECTED_);
        return;
    }
    case 0x10F3D8: { /* DWORD XInputGetState(HANDLE, PXINPUT_STATE) */
        uint32_t h = ARG(0);
        if (!pad_ok(h)) { ret_std(c, 2, ERROR_DEVICE_NOT_CONNECTED_); return; }
        fill_state(ARG(1), (int)(h & 3));
        ret_std(c, 2, 0);
        return;
    }
    default:
        if (dsound_hle(c, va)) return;
        rt_fatal("no HLE implementation for %08X", va);
    }
}
