/* Project Gauntlet runtime entry: map memory, load Default.xbe, start the entry thread. */
#include <windows.h>
#include <setjmp.h>
#include <stdlib.h>
#include "rt.h"
#ifdef __ANDROID__
#include <SDL3/SDL_main.h>   /* SDLActivity calls SDL_main in libmain.so */
#endif

typedef struct { uint32_t va; GuestFunc fn; } FuncEntry;
extern const FuncEntry g_functable[];
extern const uint32_t g_functable_count;

static FILE *g_missed;

static GuestFunc lookup(uint32_t va) {
    uint32_t lo = 0, hi = g_functable_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_functable[mid].va < va) lo = mid + 1;
        else hi = mid;
    }
    return (lo < g_functable_count && g_functable[lo].va == va) ? g_functable[lo].fn : NULL;
}

void x86_unimpl(X86Context *c, uint32_t va, const char *what) {
    rt_fatal("unimplemented at %08X: %s (esp %08X, [esp] %08X)", va, what, c->esp, RD32(c->esp));
}

void x86_call_indirect(X86Context *c, uint32_t va) {
    if (kernel_call(c, va)) return;
    if (dsound_vcall(c, va)) return;
    GuestFunc f = lookup(va);
    if (!f) {
        if (g_missed) { fprintf(g_missed, "%X\n", va); fflush(g_missed); }
        rt_fatal("indirect call to unlifted %08X (return %08X); added to out/missed_targets.txt", va, RD32(c->esp));
    }
    f(c);
}

void x86_int(X86Context *c, uint32_t va, uint32_t vector) {
    if (vector == 3) { LOG("[rt] int3 at %08X ignored\n", va); c->esp += 0; return; }
    rt_fatal("int %u at %08X", vector, va);
}
uint32_t x86_in(X86Context *c, uint32_t port, int size) {
    static uint32_t logged[16];
    static int nlogged;
    (void)c;
    for (int i = 0; i < nlogged; i++) if (logged[i] == port) return 0;
    if (nlogged < 16) logged[nlogged++] = port;
    LOG("[rt] in port %X size %d (logged once)\n", port, size);
    return 0;
}
void x86_out(X86Context *c, uint32_t port, uint32_t v, int size) { (void)c; LOG("[rt] out port %X = %X size %d\n", port, v, size); }
uint64_t x86_rdtsc(X86Context *c) { (void)c; return sched_now_100ns() * 733 / 10; /* 733 MHz */ }

double x87_load80(uint32_t a) {
    uint64_t mant = RD64(a);
    uint16_t se = RD16(a + 8);
    int exp = se & 0x7FFF;
    double sign = (se & 0x8000) ? -1.0 : 1.0;
    if (exp == 0 && mant == 0) return sign * 0.0;
    if (exp == 0x7FFF) return (mant << 1) ? NAN : sign * INFINITY;
    return sign * ldexp((double)mant, exp - 16383 - 63);
}

void x87_store80(uint32_t a, double v) {
    uint16_t se = signbit(v) ? 0x8000 : 0;
    uint64_t mant = 0;
    if (isnan(v)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (isinf(v)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e;
        double m = frexp(fabs(v), &e);
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    WR64(a, mant);
    WR16(a + 8, se);
}

#ifdef _WIN32
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    uintptr_t addr = r->NumberParameters >= 2 ? (uintptr_t)r->ExceptionInformation[1] : 0;
    if (r->ExceptionCode < 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH; /* debug prints etc. */
    HMODULE base = GetModuleHandleA(NULL);
    LOG("FATAL: host exception %08lX at +%llX, data %p (guest %08llX)\n", r->ExceptionCode,
        (unsigned long long)((uintptr_t)r->ExceptionAddress - (uintptr_t)base), (void *)addr,
        (unsigned long long)(addr - (uintptr_t)g_mem));
    uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(sp, &mbi, sizeof mbi);
    uintptr_t *end = (uintptr_t *)((uint8_t *)mbi.BaseAddress + mbi.RegionSize);
    LOG("FATAL: stack:");
    for (int i = 0, n = 0; sp + i < end && n < 24; i++) {
        uintptr_t v = sp[i];
        if (v > (uintptr_t)base && v < (uintptr_t)base + 0x2000000) { LOG(" +%llX", (unsigned long long)(v - (uintptr_t)base)); n++; }
    }
    LOG("\n");
    ExitProcess(3);
}

static HANDLE g_main_thread;

static DWORD WINAPI watchdog(void *p) {
    DWORD secs = (DWORD)(uintptr_t)p;
    for (int sample = 0; sample < 3; sample++) {
        Sleep(sample ? 300 : secs * 1000);
        SuspendThread(g_main_thread);
        CONTEXT ctx = {.ContextFlags = CONTEXT_FULL};
        GetThreadContext(g_main_thread, &ctx);
        HMODULE base = GetModuleHandleA(NULL);
        LOG("[watchdog] sample %d rip +%llX stack:", sample, (unsigned long long)(ctx.Rip - (uintptr_t)base));
        uintptr_t *sp = (uintptr_t *)ctx.Rsp;
        MEMORY_BASIC_INFORMATION mbi; VirtualQuery(sp, &mbi, sizeof mbi);
        uintptr_t *end = (uintptr_t *)((uint8_t *)mbi.BaseAddress + mbi.RegionSize);
        for (int i = 0, n = 0; sp + i < end && n < 20; i++) {
            uintptr_t v = sp[i];
            if (v > (uintptr_t)base && v < (uintptr_t)base + 0x2000000) { LOG(" +%llX", (unsigned long long)(v - (uintptr_t)base)); n++; }
        }
        LOG("\n");
        ResumeThread(g_main_thread);
    }
    ExitProcess(4);
}

void updater_start(void);

/* settings.ini [settings] (written by the launcher): GT_* keys become environment variables unless already set,
 * so the env still overrides for tests. */
static void load_settings(void) {
    wchar_t ini[MAX_PATH], buf[8192];
    if (!GetFullPathNameW(L"settings.ini", MAX_PATH, ini, NULL)) return;
    DWORD n = GetPrivateProfileSectionW(L"settings", buf, 8192, ini);
    for (wchar_t *p = buf; p < buf + n && *p; p += wcslen(p) + 1) {
        wchar_t *eq = wcschr(p, L'=');
        if (!eq || wcsncmp(p, L"GT_", 3)) continue;
        *eq = 0;
        if (!_wgetenv(p)) _wputenv_s(p, eq + 1);
        *eq = L'=';
    }
}
#else
/* Linux: SIGSEGV/SIGBUS/SIGILL/SIGFPE report the faulting guest address (on an alternate stack: a fiber's stack
 * may be the thing that overflowed), the same FATAL line as Windows. */
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <execinfo.h>
static void crash_handler(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    uintptr_t addr = (uintptr_t)si->si_addr;
#ifdef __aarch64__
    void *pc = (void *)uc->uc_mcontext.pc;
#else
    void *pc = (void *)uc->uc_mcontext.gregs[REG_RIP];
#endif
    LOG("FATAL: host signal %d at %p, data %p (guest %08llX)\n", sig, pc,
        (void *)addr, (unsigned long long)(addr - (uintptr_t)g_mem));
    void *bt[24];
    int n = backtrace(bt, 24);
    backtrace_symbols_fd(bt, n, 2);
    _exit(3);
}
static void crash_handler_install(void) {
    static uint8_t alt[256 * 1024];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

void updater_start(void);

/* settings.ini [settings]: GT_* keys become environment variables unless already set (as on Windows). */
static void load_settings(void) {
    FILE *f = fopen("settings.ini", "r");
    if (!f) return;
    char line[1024];
    int in_settings = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line, *e = line + strlen(line);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { in_settings = !strncasecmp(s, "[settings]", 10); continue; }
        char *eq = strchr(s, '=');
        if (!in_settings || !eq || strncmp(s, "GT_", 3)) continue;
        *eq = 0;
        if (!getenv(s)) setenv(s, eq + 1, 0);
    }
    fclose(f);
}
#endif

/* Frame-end wait (lifted 95FB0, see recomp/lift.py SOURCE_PATCHES): 34 ms = the original 29.4 fps cap, GT_FPS=60 -> 17 ms. */
uint32_t g_frame_ms = 34;
uint32_t g_frame_ticks = 2;   /* 60 Hz game ticks per frame: 2 = 30 fps (the original), 1 = 60 fps (GT_FPS=60) */
/* The game's clock (A9310): [14E5AC] = 60 Hz ticks = (GetTickCount - [1526EC]) * 3 / 50 while [1526E8] is set.
 * The frame end waits until GetTickCount - [148218] (the last frame's time) >= this: the next boundary of
 * g_frame_ticks ticks, so each frame advances the game by exactly that many ticks (unless it ran long). */
static int getenv_noidle(void) {
    static int v = -1;
    if (v < 0) v = getenv("GT_NOIDLE") && *getenv("GT_NOIDLE") == '1';
    return v;
}
uint32_t gt_frame_need(void) {
    uint32_t last = RD32(0x148218), t0 = RD32(0x1526EC);
    if (!RD8(0x1526E8) || !t0) return g_frame_ms;
    uint32_t k = (uint32_t)((uint64_t)(uint32_t)(last - t0) * 3 / 50);
    uint32_t due = t0 + (uint32_t)(((uint64_t)(k + g_frame_ticks) * 50 + 2) / 3);   /* first ms of tick k + n */
    /* the game polls GetTickCount in a loop until then: sleep this guest thread instead (others run, and the
     * scheduler idles the host thread when nothing else is busy). GetTickCount = sched_now_100ns / 10000. */
    if (!getenv_noidle()) sched_sleep_until((uint64_t)due * 10000);
    return due - last;
}
/* Culling (lift.py SOURCE_PATCHES on 94A80): g_cull_x widens the game's 4:3 side planes to the widescreen view
 * (set by gpu_d3d11.c), g_cull_far scales its far distance (GT_DRAWDIST = percent, 100..400). */
float g_cull_x = 1.0f, g_cull_far = 1.0f;

/* The in-game "Quit Game?" confirmation: exit the program (the original returns to the title screen, GT_QUIT=title). */
void gt_quit_game(void) {
    const char *q = getenv("GT_QUIT");
    if (q && !_stricmp(q, "title")) return;
    LOG("[rt] Quit Game: exiting\n");
    ExitProcess(0);
}

int main(int argc, char **argv) {
    const char *xbe = argc > 1 ? argv[1] : "game/Default.xbe";
#ifdef _WIN32
    if (argc <= 1 && GetFileAttributesA(xbe) == INVALID_FILE_ATTRIBUTES) {   /* installed: run from the exe's folder */
        char dir[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
        while (n && dir[n - 1] != '\\') n--;
        if (n) { dir[n - 1] = 0; SetCurrentDirectoryA(dir); }
    }
#elif defined(__ANDROID__)
    android_start();   /* sdl_plat.c: the app's files folder, log to out/gauntlet.log, window (on the main stack) */
    linux_set_argv(argv);
#else
    if (argc <= 1 && GetFileAttributesA(xbe) == INVALID_FILE_ATTRIBUTES) {   /* installed: run from the exe's folder */
        char dir[MAX_PATH];
        ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
        while (n > 0 && dir[n - 1] != '/') n--;
        if (n > 1) { dir[n - 1] = 0; if (chdir(dir)) {} }
    }
    linux_set_argv(argv);
#endif
    load_settings();
#ifdef __ANDROID__
    setenv("GT_SCALE", "3", 0);   /* phone defaults (settings.ini in the files folder overrides) */
    setenv("GT_MSAA", "2", 0);
#endif
    if (getenv("GT_FPS") && atoi(getenv("GT_FPS")) == 60) { g_frame_ms = 17; g_frame_ticks = 1; }
    if (getenv("GT_DRAWDIST")) {
        int d = atoi(getenv("GT_DRAWDIST"));
        if (d >= 100 && d <= 400) g_cull_far = d / 100.0f;
    }
    CreateDirectoryA("out", NULL);
#ifndef _WIN32
    /* installed: gauntlet.sh sends stderr to out/gauntlet.log */
    updater_start();
    crash_handler_install();
    g_missed = fopen("out/missed_targets.txt", "a");
#else
    if (!GetConsoleWindow() && (GetStdHandle(STD_ERROR_HANDLE) == NULL ||
                                GetStdHandle(STD_ERROR_HANDLE) == INVALID_HANDLE_VALUE))
        freopen("out\\gauntlet.log", "w", stderr);   /* installed build (windows subsystem): log to a file */
    updater_start();
    ULONG guarantee = 0x10000;
    SetThreadStackGuarantee(&guarantee);
    AddVectoredExceptionHandler(1, crash_handler);
    g_missed = fopen("out/missed_targets.txt", "a");
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    if (getenv("GT_WATCHDOG")) CreateThread(NULL, 0, watchdog, (void *)(uintptr_t)atoi(getenv("GT_WATCHDOG")), 0, NULL);
#endif
    mem_init();
    kernel_init();
    uint32_t entry = xbe_load(xbe);
    LOG("[rt] g_mem %p, entry %08X, kernel thunk %08X\n", g_mem, entry, g_kernel_thunk);
    /* The kernel calls the XBE entry point with no arguments on the initial thread. */
    sched_create_thread(entry, 0, 0, 0, 0x40000, 0, 0);
    sched_run();
    LOG("[rt] all guest threads exited\n");
    return 0;
}
