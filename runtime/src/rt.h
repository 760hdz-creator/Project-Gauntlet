/* Project Gauntlet runtime: shared declarations. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "x86rt.h"

/* ---- guest memory map ---- */
#define RAM_SIZE        0x04000000u   /* 64 MB, also mirrored at 0x80000000 (contiguous/physical alias) */
#define RAM_ALIAS       0x80000000u
#define HEAP_BASE       0x00C00000u   /* above the XBE image (ends 0xBFCDC0) */
#define HEAP_END        0x03F00000u
#define KAREA_BASE      0x8F000000u   /* kernel data exports, KPCRs, kernel objects */
#define KAREA_SIZE      0x00400000u
#define KSENTINEL_BASE  0x8FF00000u   /* kernel function "addresses": base + ordinal*4 */
#define THREAD_EXIT_VA  0x8FFFF000u   /* return address of a guest thread's start routine */
#define HW_BASE         0xFD000000u   /* NV2A / APU / other MMIO: dummy RAM for now */
#define HW_SIZE         0x03000000u

#define ARG(i) RD32(c->esp + 4 + 4 * (i))

/* logging */
extern int g_log_kernel;
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
_Noreturn void rt_fatal(const char *fmt, ...);

/* memory */
void mem_init(void);
uint32_t mem_alloc(uint32_t size, uint32_t align);   /* guest RAM, zeroed; 0 on failure */
void mem_free(uint32_t va);
uint32_t mem_size(uint32_t va);
uint32_t karea_alloc(uint32_t size);                 /* kernel area, zeroed, never freed */

/* loader */
uint32_t xbe_load(const char *path);                 /* returns entry point */
extern uint32_t g_kernel_thunk;

/* threads (cooperative fibers) */
typedef struct GThread GThread;
GThread *sched_create_thread(uint32_t start, uint32_t arg1, uint32_t arg2, uint32_t routine, uint32_t stack_size,
                             uint32_t tls_size, int suspended);
GThread *sched_current(void);
uint32_t sched_thread_object(GThread *t);            /* guest KTHREAD address */
GThread *sched_create_host_thread(void (*fn)(X86Context *c));
/* Call a guest stdcall/cdecl function from host code running on a guest thread. */
void guest_call(X86Context *c, uint32_t va, int nargs, const uint32_t *args);
void sched_yield(void);                              /* let other threads run */
_Noreturn void sched_exit_thread(uint32_t status);
void sched_run(void);                                 /* main loop; returns when all threads exit */
uint64_t sched_now_100ns(void);                       /* guest time (100 ns units since boot) */
void sched_sleep_until(uint64_t t100ns);

/* kernel */
void kernel_init(void);
void kernel_poll(void);                              /* timers -> DPCs; called by the scheduler */
int kernel_call(X86Context *c, uint32_t va);          /* 1 if va was a kernel sentinel */
uint32_t kernel_wait(uint32_t *objects, int count, int wait_any, int64_t timeout_100ns, int has_timeout);

/* handles (kernel.c) */
typedef struct { int used; int kind; uint32_t obj; void *host; } Handle;
enum { H_OBJECT = 1, H_FILE };
uint32_t handle_new(int kind, uint32_t obj, void *host);
Handle *handle_get(uint32_t h);
uint32_t handle_object(uint32_t h);
typedef uint32_t (*KHandler)(X86Context *c);

/* file system (fs.c) */
void fs_register(KHandler *handlers);
void fs_close(Handle *h);
