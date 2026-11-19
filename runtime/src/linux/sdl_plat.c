/* Linux: the game window, keyboard, monitors and XInput on SDL3 (see linux/include/windows.h).
 * The window is an SDL_Window * used as the HWND (DXVK Native's SDL3 WSI takes it as the swap chain's window).
 * Every call here runs on the main thread: all guest threads are fibers on it, and sched_run pumps events. */
#include <windows.h>
#include <SDL3/SDL.h>
#include <unistd.h>

#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)

static SDL_Window *g_win;
static int g_video;

/* ---------------- window ---------------- */
static void toggle_fullscreen(void) {
    int fs = (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(g_win, !fs);   /* borderless desktop fullscreen (no mode change) */
}

/* GT_MONITOR=N picks display N (SDL order). Default as on Windows: the installed game (gauntlet.sh sets
 * GT_INSTALLED=1) opens on the primary display and takes focus; dev/test runs open on the first other display. */
HWND linux_create_window(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_APP_NAME, "Project Gauntlet");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        LOG("[gpu] SDL video unavailable: %s\n", SDL_GetError());
        return NULL;
    }
    g_video = 1;
    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    SDL_DisplayID prim = SDL_GetPrimaryDisplay(), pick = prim;
    int installed = getenv("GT_INSTALLED") && *getenv("GT_INSTALLED") == '1';
    LOG("[gpu] %d display(s)
", n);
    const char *me = getenv("GT_MONITOR");
    if (me && *me && ids && atoi(me) >= 0 && atoi(me) < n) pick = ids[atoi(me)];
    else if (!installed && ids)
        for (int i = 0; i < n; i++) if (ids[i] != prim) { pick = ids[i]; break; }
    SDL_free(ids);
    SDL_Rect r = {0, 0, 1920, 1080};
    SDL_GetDisplayBounds(pick, &r);
    const char *ws = getenv("GT_WIDESCREEN");
    int wh = r.h * 2 / 3;
    int ww = ws && *ws == '0' ? wh * 4 / 3 : (int)((double)wh * r.w / r.h);
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetStringProperty(p, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Gauntlet Dark Legacy (recomp)");
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(pick));
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(pick));
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, ww);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, wh);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, true);
    g_win = SDL_CreateWindowWithProperties(p);
    SDL_DestroyProperties(p);
    if (!g_win) { LOG("[gpu] SDL_CreateWindow failed: %s\n", SDL_GetError()); return NULL; }
    if (getenv("GT_FULLSCREEN") && *getenv("GT_FULLSCREEN") == '1') toggle_fullscreen();
    if (installed) SDL_RaiseWindow(g_win);
    SDL_ShowWindow(g_win);
    SDL_SyncWindow(g_win);
    return (HWND)g_win;
}

void linux_pump_events(void) {
    static uint64_t last;
    if (!g_video) return;
    uint64_t now = SDL_GetTicksNS();
    if (now - last < 2000000ull) return;   /* every 2 ms at most */
    last = now;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            LOG("[gpu] window closed\n");
            ExitProcess(0);
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.key == SDLK_ESCAPE) { LOG("[gpu] escape\n"); ExitProcess(0); }
            if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) toggle_fullscreen();
            break;
        default: break;
        }
    }
}

BOOL GetClientRect(HWND w, RECT *r) {
    int pw = 0, ph = 0;
    if (w) SDL_GetWindowSizeInPixels((SDL_Window *)w, &pw, &ph);
    *r = (RECT){0, 0, pw, ph};
    return w != NULL;
}
HMONITOR MonitorFromWindow(HWND w, DWORD flags) {
    (void)flags;
    SDL_DisplayID d = w ? SDL_GetDisplayForWindow((SDL_Window *)w) : 0;
    return (HMONITOR)(uintptr_t)(d ? d : SDL_GetPrimaryDisplay());
}
BOOL GetMonitorInfoW(HMONITOR m, MONITORINFO *mi) {
    SDL_DisplayID d = (SDL_DisplayID)(uintptr_t)m;
    SDL_Rect b = {0, 0, 1920, 1080}, u;
    SDL_GetDisplayBounds(d, &b);
    /* physical pixels: bounds are in points on scaled Wayland outputs */
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(d);
    if (dm && dm->w > b.w) { b.w = dm->w; b.h = dm->h; }
    u = b;
    mi->rcMonitor = (RECT){b.x, b.y, b.x + b.w, b.y + b.h};
    mi->rcWork = (RECT){u.x, u.y, u.x + u.w, u.y + u.h};
    mi->dwFlags = d == SDL_GetPrimaryDisplay() ? MONITORINFOF_PRIMARY : 0;
    return TRUE;
}
HWND GetForegroundWindow(void) { return g_win && SDL_GetKeyboardFocus() == g_win ? (HWND)g_win : NULL; }
DWORD GetWindowThreadProcessId(HWND w, DWORD *pid) { (void)w; if (pid) *pid = (DWORD)getpid(); return 0; }

/* ---------------- keyboard: virtual keys -> SDL scancodes ---------------- */
static SDL_Scancode scancode_of(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk <= VK_F12) return (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - VK_NUMPAD1));
    switch (vk) {
    case VK_NUMPAD0: return SDL_SCANCODE_KP_0;
    case VK_BACK: return SDL_SCANCODE_BACKSPACE;
    case VK_TAB: return SDL_SCANCODE_TAB;
    case VK_RETURN: return SDL_SCANCODE_RETURN;
    case VK_PAUSE: return SDL_SCANCODE_PAUSE;
    case VK_CAPITAL: return SDL_SCANCODE_CAPSLOCK;
    case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case VK_SPACE: return SDL_SCANCODE_SPACE;
    case VK_PRIOR: return SDL_SCANCODE_PAGEUP;
    case VK_NEXT: return SDL_SCANCODE_PAGEDOWN;
    case VK_END: return SDL_SCANCODE_END;
    case VK_HOME: return SDL_SCANCODE_HOME;
    case VK_LEFT: return SDL_SCANCODE_LEFT;
    case VK_UP: return SDL_SCANCODE_UP;
    case VK_RIGHT: return SDL_SCANCODE_RIGHT;
    case VK_DOWN: return SDL_SCANCODE_DOWN;
    case VK_INSERT: return SDL_SCANCODE_INSERT;
    case VK_DELETE: return SDL_SCANCODE_DELETE;
    case VK_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY;
    case VK_ADD: return SDL_SCANCODE_KP_PLUS;
    case VK_SUBTRACT: return SDL_SCANCODE_KP_MINUS;
    case VK_DECIMAL: return SDL_SCANCODE_KP_PERIOD;
    case VK_DIVIDE: return SDL_SCANCODE_KP_DIVIDE;
    case VK_LSHIFT: return SDL_SCANCODE_LSHIFT;
    case VK_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case VK_LCONTROL: return SDL_SCANCODE_LCTRL;
    case VK_RCONTROL: return SDL_SCANCODE_RCTRL;
    case VK_LMENU: return SDL_SCANCODE_LALT;
    case VK_RMENU: return SDL_SCANCODE_RALT;
    case VK_OEM_1: return SDL_SCANCODE_SEMICOLON;
    case VK_OEM_PLUS: return SDL_SCANCODE_EQUALS;
    case VK_OEM_COMMA: return SDL_SCANCODE_COMMA;
    case VK_OEM_MINUS: return SDL_SCANCODE_MINUS;
    case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD;
    case VK_OEM_2: return SDL_SCANCODE_SLASH;
    case VK_OEM_3: return SDL_SCANCODE_GRAVE;
    case VK_OEM_4: return SDL_SCANCODE_LEFTBRACKET;
    case VK_OEM_5: return SDL_SCANCODE_BACKSLASH;
    case VK_OEM_6: return SDL_SCANCODE_RIGHTBRACKET;
    case VK_OEM_7: return SDL_SCANCODE_APOSTROPHE;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}
SHORT GetAsyncKeyState(int vk) {
    if (!g_video) return 0;
    int n = 0;
    const bool *k = SDL_GetKeyboardState(&n);
    int down;
    switch (vk) {
    case VK_SHIFT: down = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]; break;
    case VK_CONTROL: down = k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL]; break;
    case VK_MENU: down = k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT]; break;
    default: { SDL_Scancode s = scancode_of(vk); down = s != SDL_SCANCODE_UNKNOWN && s < n && k[s]; }
    }
    return down ? (SHORT)0x8000 : 0;
}

/* ---------------- XInput on SDL gamepads (slots 0..3 in connection order) ---------------- */
typedef struct {
    WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} HostPad;
typedef struct { DWORD dwPacketNumber; HostPad Gamepad; } HostState;
static SDL_Gamepad *pads[4];
static uint64_t next_scan;
static DWORD packets[4];

static void scan_pads(void) {
    uint64_t now = SDL_GetTicks();
    if (now < next_scan) return;
    next_scan = now + 1000;
    for (int k = 0; k < 4; k++)
        if (pads[k] && !SDL_GamepadConnected(pads[k])) { SDL_CloseGamepad(pads[k]); pads[k] = NULL; LOG("[input] pad %d removed\n", k); }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; i++) {
        int open = 0;
        for (int k = 0; k < 4; k++) open |= pads[k] && SDL_GetGamepadID(pads[k]) == ids[i];
        if (open) continue;
        for (int k = 0; k < 4; k++)
            if (!pads[k]) {
                if ((pads[k] = SDL_OpenGamepad(ids[i]))) {
                    const char *name = SDL_GetGamepadName(pads[k]);
                    LOG("[input] pad connected: %s, XInput slot %d\n", name ? name : "?", k);
                }
                break;
            }
    }
    SDL_free(ids);
}
static SHORT axis(SDL_Gamepad *g, SDL_GamepadAxis a, int flip) {
    int v = SDL_GetGamepadAxis(g, a);
    if (flip) v = -v - 1;
    return (SHORT)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}
static DWORD WINAPI xinput_get_state(DWORD port, HostState *s) {
    if (!g_video || port >= 4) return 1167;
    scan_pads();
    SDL_Gamepad *g = pads[port];
    if (!g) return 1167;   /* ERROR_DEVICE_NOT_CONNECTED */
    static const struct { SDL_GamepadButton b; WORD bit; } map[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
        {SDL_GAMEPAD_BUTTON_START, 0x0010}, {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000}, {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
    };
    memset(s, 0, sizeof *s);
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GetGamepadButton(g, map[i].b)) s->Gamepad.wButtons |= map[i].bit;
    s->Gamepad.bLeftTrigger = (BYTE)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255 / 32767);
    s->Gamepad.bRightTrigger = (BYTE)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255 / 32767);
    s->Gamepad.sThumbLX = axis(g, SDL_GAMEPAD_AXIS_LEFTX, 0);
    s->Gamepad.sThumbLY = axis(g, SDL_GAMEPAD_AXIS_LEFTY, 1);   /* XInput: up is positive */
    s->Gamepad.sThumbRX = axis(g, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    s->Gamepad.sThumbRY = axis(g, SDL_GAMEPAD_AXIS_RIGHTY, 1);
    s->dwPacketNumber = ++packets[port];
    return 0;
}
static DWORD WINAPI xinput_set_state(DWORD port, WORD *vib) {
    if (!g_video || port >= 4 || !pads[port]) return 1167;
    SDL_RumbleGamepad(pads[port], vib[0], vib[1], 1000);
    return 0;
}

HMODULE LoadLibraryA(const char *name) { return name && strcasestr(name, "xinput") ? (HMODULE)(uintptr_t)1 : NULL; }
void *GetProcAddress(HMODULE m, const char *name) {
    if (m != (HMODULE)(uintptr_t)1) return NULL;
    if (!strcmp(name, "XInputGetState")) return (void *)xinput_get_state;
    if (!strcmp(name, "XInputSetState")) return (void *)xinput_set_state;
    return NULL;
}
