/* Host implementations of guest library functions (recomp/hle.txt).
 * XPP input: the game sees one gamepad on port 0, fed by the host's first XInput controller
 * (and the keyboard once there is a window).
 */
#include <windows.h>
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
        rt_fatal("no HLE implementation for %08X", va);
    }
}
