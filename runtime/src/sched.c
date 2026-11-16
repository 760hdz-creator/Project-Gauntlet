/* Cooperative guest threads on host fibers. One host thread runs everything; a guest thread
 * runs until it waits, sleeps or yields. Guest time follows host time. */
#include <windows.h>
#include <stdlib.h>
#include "rt.h"

/* Guest KPCR layout (subset used by XDK code). */
#define KPCR_SIZE        0x300
#define KPCR_EXCEPTION   0x00
#define KPCR_STACKBASE   0x04
#define KPCR_STACKLIMIT  0x08
#define KPCR_SELF_TIB    0x18
#define KPCR_SELF_PCR    0x1C
#define KPCR_PRCB        0x20
#define KPCR_IRQL        0x24
#define KPCR_CURTHREAD   0x28   /* KPRCB starts here: CurrentThread */
#define KTHREAD_SIZE     0x200
#define KTHREAD_TLSDATA  0x28

enum { T_READY, T_DONE };

struct GThread {
    X86Context ctx;
    void *fiber;
    int state;
    uint32_t kthread, kpcr;
    uint32_t start, arg1, arg2, routine;
    uint32_t exit_status;
    void (*host_fn)(X86Context *c);
    GThread *next;
};

static void *main_fiber;
static GThread *threads, *current;
static LARGE_INTEGER qpf, qpc0;

uint64_t sched_now_100ns(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)(t.QuadPart - qpc0.QuadPart) * 1e7 / (double)qpf.QuadPart);
}

GThread *sched_current(void) { return current; }
uint32_t sched_thread_object(GThread *t) { return t->kthread; }

static void WINAPI fiber_main(void *p) {
    GThread *t = p;
    X86Context *c = &t->ctx;
    if (t->host_fn) {
        t->host_fn(c);
        sched_exit_thread(0);
    }
    /* Guest call: routine(arg1, arg2) stdcall, or start(arg2) when there is no routine. */
    if (t->routine) {
        c->esp -= 4; WR32(c->esp, t->arg2);
        c->esp -= 4; WR32(c->esp, t->arg1);
        c->esp -= 4; WR32(c->esp, THREAD_EXIT_VA);
        x86_call_indirect(c, t->routine);
    } else {
        c->esp -= 4; WR32(c->esp, t->arg2);
        c->esp -= 4; WR32(c->esp, THREAD_EXIT_VA);
        x86_call_indirect(c, t->start);
    }
    sched_exit_thread(c->eax);
}

GThread *sched_create_thread(uint32_t start, uint32_t arg1, uint32_t arg2, uint32_t routine, uint32_t stack_size,
                             uint32_t tls_size, int suspended) {
    (void)suspended;
    GThread *t = calloc(1, sizeof *t);
    if (stack_size < 0x10000) stack_size = 0x10000;
    uint32_t stack = mem_alloc(stack_size, 0x1000);
    if (!stack) rt_fatal("no memory for thread stack");
    t->kthread = karea_alloc(KTHREAD_SIZE);
    t->kpcr = karea_alloc(KPCR_SIZE);
    WR8(t->kthread, 6); /* DISPATCHER_HEADER.Type = ThreadObject */
    WR8(t->kthread + 2, KTHREAD_SIZE / 4);
    if (tls_size) {
        uint32_t tls = mem_alloc(tls_size + 16, 16);
        WR32(t->kthread + KTHREAD_TLSDATA, tls);
    }
    WR32(t->kpcr + KPCR_EXCEPTION, 0xFFFFFFFF);
    WR32(t->kpcr + KPCR_STACKBASE, stack + stack_size);
    WR32(t->kpcr + KPCR_STACKLIMIT, stack);
    WR32(t->kpcr + KPCR_SELF_TIB, t->kpcr);
    WR32(t->kpcr + KPCR_SELF_PCR, t->kpcr);
    WR32(t->kpcr + KPCR_PRCB, t->kpcr + KPCR_CURTHREAD);
    WR32(t->kpcr + KPCR_CURTHREAD, t->kthread);
    t->ctx.esp = stack + stack_size - 16;
    t->ctx.fs_base = t->kpcr;
    t->ctx.fcw = 0x027F;
    t->ctx.mxcsr = 0x1F80;
    t->start = start;
    t->arg1 = arg1;
    t->arg2 = arg2;
    t->routine = routine;
    t->fiber = CreateFiber(256 * 1024, fiber_main, t);
    if (!t->fiber) rt_fatal("CreateFiber failed");
    /* append to keep creation order */
    GThread **pp = &threads;
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    LOG("[sched] thread %08X start=%08X routine=%08X stack=%08X+%X\n", t->kthread, start, routine, stack, stack_size);
    return t;
}

GThread *sched_create_host_thread(void (*fn)(X86Context *c)) {
    GThread *t = sched_create_thread(0, 0, 0, 0, 0x10000, 0, 0);
    t->host_fn = fn;
    return t;
}

void guest_call(X86Context *c, uint32_t va, int nargs, const uint32_t *args) {
    for (int i = nargs - 1; i >= 0; i--) { c->esp -= 4; WR32(c->esp, args[i]); }
    c->esp -= 4;
    WR32(c->esp, THREAD_EXIT_VA);
    x86_call_indirect(c, va);
}

void sched_yield(void) {
    SwitchToFiber(main_fiber);
}

_Noreturn void sched_exit_thread(uint32_t status) {
    current->state = T_DONE;
    current->exit_status = status;
    WR32(current->kthread + 4, 1); /* SignalState: thread objects signal on exit */
    LOG("[sched] thread %08X exited (%u)\n", current->kthread, status);
    SwitchToFiber(main_fiber);
    rt_fatal("dead thread resumed");
}

void sched_run(void) {
    QueryPerformanceFrequency(&qpf);
    if (!qpc0.QuadPart) QueryPerformanceCounter(&qpc0);
    main_fiber = ConvertThreadToFiber(NULL);
    for (;;) {
        int alive = 0;
        for (GThread *t = threads; t; t = t->next) {
            if (t->state == T_DONE) continue;
            alive = 1;
            current = t;
            SwitchToFiber(t->fiber);
            current = NULL;
        }
        if (!alive) return;
        kernel_poll();
        Sleep(0);
    }
}

void sched_sleep_until(uint64_t t100ns) {
    while (sched_now_100ns() < t100ns) sched_yield();
}
