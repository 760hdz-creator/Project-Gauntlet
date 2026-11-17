/* Differential test DLL: lifted code + minimal runtime, driven from Python.
 * Exports oracle_init() -> g_mem, oracle_run(va, ctx, err) -> 0 ok / -1 trap / -2 no func / -3 host fault.
 */
#include <windows.h>
#include <setjmp.h>
#include <stdio.h>
#include "x86rt.h"

uint8_t *g_mem;

typedef struct { uint32_t va; GuestFunc fn; } FuncEntry;
extern const FuncEntry g_functable[];
extern const uint32_t g_functable_count;

static jmp_buf g_jb;
static char g_err[512];

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
    (void)c;
    snprintf(g_err, sizeof g_err, "%08X: %s", va, what);
    longjmp(g_jb, 1);
}

void x86_call_indirect(X86Context *c, uint32_t va) {
    GuestFunc f = lookup(va);
    if (!f) x86_unimpl(c, va, "indirect target not lifted");
    f(c);
}

void x86_int(X86Context *c, uint32_t va, uint32_t vector) { (void)vector; x86_unimpl(c, va, "int"); }
uint32_t x86_in(X86Context *c, uint32_t port, int size) { (void)size; x86_unimpl(c, port, "in"); return 0; }
void x86_out(X86Context *c, uint32_t port, uint32_t v, int size) { (void)v; (void)size; x86_unimpl(c, port, "out"); }
uint64_t x86_rdtsc(X86Context *c) { (void)c; return 0; }
void x86_hle(X86Context *c, uint32_t va) { x86_unimpl(c, va, "hle"); }
void x86_hook(X86Context *c, uint32_t va) { (void)c; (void)va; }
uint32_t mmio_read(uint32_t a, int size) { (void)size; return RD32(a); }
void mmio_write(uint32_t a, uint32_t v, int size) { (void)size; WR32(a, v); }

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
        double m = frexp(fabs(v), &e); /* v = m * 2^e, m in [0.5,1) */
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    WR64(a, mant);
    WR16(a + 8, se);
}

__declspec(dllexport) uint8_t *oracle_init(void) {
    if (!g_mem) {
        g_mem = VirtualAlloc(NULL, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS);
        VirtualAlloc(g_mem, 0x04000000, MEM_COMMIT, PAGE_READWRITE);
    }
    return g_mem;
}

__declspec(dllexport) uint32_t oracle_ctx_size(void) { return (uint32_t)sizeof(X86Context); }

static int fault_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    uintptr_t addr = r->NumberParameters >= 2 ? (uintptr_t)r->ExceptionInformation[1] : 0;
    snprintf(g_err, sizeof g_err, "host exception 0x%08lX at %p, data %p (guest 0x%llX)", r->ExceptionCode,
             r->ExceptionAddress, (void *)addr, (unsigned long long)(addr - (uintptr_t)g_mem));
    return EXCEPTION_EXECUTE_HANDLER;
}

static int run_guarded(GuestFunc f, X86Context *c) {
    __try {
        f(c);
    } __except (fault_filter(GetExceptionInformation())) {
        return -3;
    }
    return 0;
}

__declspec(dllexport) int oracle_run(uint32_t va, X86Context *c, char *err) {
    GuestFunc f = lookup(va);
    if (!f) return -2;
    g_err[0] = 0;
    int rc;
    if (setjmp(g_jb)) rc = -1;
    else rc = run_guarded(f, c);
    memcpy(err, g_err, sizeof g_err);
    return rc;
}
