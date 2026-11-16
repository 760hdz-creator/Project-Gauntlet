/* NV2A register emulation for the recompiled XDK D3D library.
 * Only the D3D section's memory accesses reach here (lifter emits RD32X/WR32X there).
 * Registers default to RAM semantics (backed by the dummy HW range in guest memory);
 * the cases below override reads and writes where the driver waits on hardware.
 */
#include <stdlib.h>
#include "rt.h"

#define NV_BASE         0xFD000000u
#define NV_SIZE         0x01000000u

#define NV_PMC_BOOT_0   0x000000u
#define NV_PTIMER_TIME0 0x009400u
#define NV_PTIMER_TIME1 0x009410u
#define NV_PFB_WBC      0x100410u     /* bit 16: write-back cache flush pending */
#define NV_USER_PUT     0x800040u     /* channel 0 DMA put (physical) */
#define NV_USER_GET     0x800044u     /* channel 0 DMA get (physical) */

static int nv_log = -1;
static uint8_t *seen;                 /* one bit per register dword, for first-touch logging */

static uint32_t *reg(uint32_t off) { return (uint32_t *)(g_mem + NV_BASE + (off & ~3u)); }

static void touch(uint32_t a, int write, uint32_t v) {
    if (nv_log < 0) {
        const char *e = getenv("GT_NVLOG");
        nv_log = e && *e && *e != '0';
        if (nv_log) seen = calloc(NV_SIZE / 32 + 1, 1);
    }
    if (!nv_log) return;
    uint32_t i = ((a - NV_BASE) & (NV_SIZE - 1)) >> 2;
    uint8_t bit = (uint8_t)(1u << (i & 7));
    if (nv_log > 1 || !(seen[i >> 3] & bit)) {
        if (nv_log == 1) seen[i >> 3] |= bit;
        LOG("[nv2a] %s %08X = %08X\n", write ? "W" : "R", a, v);
    }
}

/* ---- PGRAPH ---- */
static int pb_log = -1;
static uint32_t sub_class[8];          /* object handle bound to each subchannel */

#define NV_PRAMIN 0x700000u

/* Resolve an object handle through RAMHT (linear scan) to its PRAMIN instance offset; 0 if absent. */
static uint32_t ramht_lookup(uint32_t handle) {
    uint32_t ht = *reg(0x002210);
    uint32_t base = ((ht >> 4) & 0x1F) << 12, size = 0x1000u << ((ht >> 16) & 3);
    for (uint32_t e = base; e < base + size; e += 8)
        if (*reg(NV_PRAMIN + e) == handle && *reg(NV_PRAMIN + e + 4))
            return (*reg(NV_PRAMIN + e + 4) & 0xFFFF) << 4;
    return 0;
}

/* Base physical address of a DMA context object, by handle. */
static uint32_t dma_base(uint32_t handle) {
    uint32_t inst = ramht_lookup(handle);
    if (!inst) { LOG("[pb] DMA handle %X not in RAMHT\n", handle); return 0; }
    uint32_t w0 = *reg(NV_PRAMIN + inst), w2 = *reg(NV_PRAMIN + inst + 8);
    if (pb_log) LOG("[pb] DMA %X inst %X: %08X %08X %08X\n", handle, inst, w0, *reg(NV_PRAMIN + inst + 4), w2);
    static int dumped;
    if (pb_log && !dumped++) {
        for (uint32_t e = 0; e < 0x1000; e += 8)
            if (*reg(NV_PRAMIN + e + 4)) LOG("[pb] ramht %03X: %08X %08X\n", e, *reg(NV_PRAMIN + e), *reg(NV_PRAMIN + e + 4));
        for (uint32_t o = 0x1000; o < 0x20000; o += 16)
            if (*reg(NV_PRAMIN + o)) LOG("[pb] inst %05X: %08X %08X %08X %08X\n", o, *reg(NV_PRAMIN + o), *reg(NV_PRAMIN + o + 4),
                                         *reg(NV_PRAMIN + o + 8), *reg(NV_PRAMIN + o + 12));
    }
    return ((w2 & 0xFFFFF000u) | (w0 >> 20)) & 0x03FFFFFF;
}

/* Kelvin state the "GPU" acts on (no rendering yet). */
static uint32_t k_dma_semaphore, k_semaphore_offset;

static void pgraph_method(uint32_t subch, uint32_t method, uint32_t data) {
    if (pb_log) LOG("[pb] sub%u %04X = %08X\n", subch, method, data);
    if (method == 0) { sub_class[subch] = data; return; }
    if (subch != 0) return;   /* subchannel 0 = Kelvin (3D) */
    switch (method) {
    case 0x01A4: k_dma_semaphore = data; break;                         /* SET_CONTEXT_DMA_SEMAPHORE */
    case 0x1D6C: k_semaphore_offset = data; break;                      /* SET_SEMAPHORE_OFFSET */
    case 0x1D70: {                                                      /* BACK_END_WRITE_SEMAPHORE_RELEASE */
        uint32_t a = dma_base(k_dma_semaphore) + k_semaphore_offset;
        WR32(a, data);
        if (pb_log) LOG("[pb] semaphore %08X <- %X (dev %08X [34] %08X)\n", a, data, RD32(0xD0170), RD32(RD32(0xD0170) + 0x34));
        break;
    }
    default: break;
    }
}

/* ---- PFIFO pusher: run the push buffer from GET to PUT ("infinitely fast" GPU) ---- */
static uint32_t pb_get, pb_ret;

static void pfifo_kick(uint32_t put) {
    if (pb_log < 0) { const char *e = getenv("GT_PBLOG"); pb_log = e && *e && *e != '0'; }
    put &= 0x03FFFFFC;
    uint32_t get = pb_get;
    for (int guard = 0; get != put; guard++) {
        if (guard > 4000000) { LOG("[pb] runaway push buffer at %08X (put %08X)\n", get, put); break; }
        uint32_t cmd = RD32(get);
        get += 4;
        if ((cmd & 0xE0000003) == 0x20000000) { get = cmd & 0x03FFFFFC; continue; }     /* old jump */
        if ((cmd & 3) == 1) { get = cmd & 0x03FFFFFC; continue; }                      /* jump */
        if ((cmd & 3) == 2) { pb_ret = get; get = cmd & 0x03FFFFFC; continue; }         /* call */
        if (cmd == 0x00020000) { get = pb_ret; continue; }                             /* return */
        uint32_t kind = cmd & 0xE0030003;
        if (kind != 0 && kind != 0x40000000) {
            LOG("[pb] bad command %08X at %08X, skipping to put\n", cmd, get - 4);
            get = put;
            break;
        }
        uint32_t method = cmd & 0x1FFC, subch = (cmd >> 13) & 7, count = (cmd >> 18) & 0x7FF;
        for (uint32_t i = 0; i < count; i++) {
            pgraph_method(subch, method, RD32(get));
            get += 4;
            if (!kind) method += 4;
        }
    }
    pb_get = get;
    *reg(NV_USER_GET) = get;
}

uint32_t mmio_read(uint32_t a, int size) {
    uint32_t v;
    sched_maybe_yield();   /* register polling loops */
    if (a == 0xFE820010u) {
        /* APU: free-running sample counter (48 kHz), polled by DSOUND init */
        v = (uint32_t)(sched_now_100ns() * 48 / 10000) & ~3u;
    } else if (a < NV_BASE || a >= NV_BASE + NV_SIZE) {
        v = RD32(a);
        if (a < 0xFE800000u || a >= 0xFE880000u) LOG("[mmio] R%d %08X (non-NV2A)\n", size * 8, a);
    } else {
        uint32_t off = a - NV_BASE;
        switch (off & ~3u) {
        case NV_PMC_BOOT_0: v = 0x02A000A3; break;
        case 0x000100:                                    /* PMC_INTR_0: pending sources */
            v = (*reg(0x2100) & *reg(0x2140) ? 1u << 8 : 0) | (*reg(0x400100) & *reg(0x400140) ? 1u << 12 : 0) |
                (*reg(0x600100) & *reg(0x600140) ? 1u << 24 : 0);
            break;
        case NV_PTIMER_TIME0: v = (uint32_t)(sched_now_100ns() * 100) << 5; break;
        case NV_PTIMER_TIME1: v = (uint32_t)((sched_now_100ns() * 100) >> 27); break;
        case NV_PFB_WBC: v = *reg(off) &= ~0x10000u; break;
        /* the GPU is always idle: FIFO caches empty, pusher not busy */
        case 0x002400: v = *reg(off) | 0x10; break;       /* PFIFO_RUNOUT_STATUS: empty */
        case 0x003214: v = *reg(off) | 0x10; break;       /* CACHE1_STATUS: empty */
        case 0x003220: v = *reg(off) & ~0x10u; break;     /* CACHE1_DMA_PUSH: status idle */
        /* PVIDEO overlay (WMV playback): every queued buffer is consumed at once. The overlay
         * registers (0x8900+) keep their values for the host renderer. */
        case 0x008700: v = 0; break;                      /* PVIDEO_BUFFER: none in use */
        default: v = *reg(off); break;
        }
        touch(a, 0, v);
        v >>= (a & 3) * 8;
    }
    if (size == 1) return v & 0xFF;
    if (size == 2) return v & 0xFFFF;
    return v;
}

void mmio_write(uint32_t a, uint32_t v, int size) {
    if (a < NV_BASE || a >= NV_BASE + NV_SIZE) {
        if (a < 0xFE800000u || a >= 0xFE880000u) LOG("[mmio] W%d %08X = %08X (non-NV2A)\n", size * 8, a, v);
        if (size == 4) WR32(a, v); else if (size == 2) WR16(a, (uint16_t)v); else WR8(a, (uint8_t)v);
        return;
    }
    touch(a, 1, v);
    switch ((a - NV_BASE) & ~3u) {
    /* interrupt status registers: write 1 to clear */
    case 0x000100: return;  /* PMC_INTR_0 is derived */
    case 0x002100: case 0x009100: case 0x400100: case 0x600100:
        *reg(a - NV_BASE) &= ~(v << (a & 3) * 8);
        return;
    default: break;
    }
    if (size == 4) WR32(a, v); else if (size == 2) WR16(a, (uint16_t)v); else WR8(a, (uint8_t)v);
    switch ((a - NV_BASE) & ~3u) {
    case NV_USER_PUT: pfifo_kick(v); break;
    case NV_USER_GET: pb_get = v & 0x03FFFFFC; break;
    default: break;
    }
}

/* ---- interrupts ---- */
#define NV_PMC_INTR     0x000100u
#define NV_PCRTC_INTR   0x600100u
#define NV_PCRTC_INTR_EN 0x600140u
#define NV_GPU_IRQ      3

void nv2a_vblank(void) {
    *reg(NV_PCRTC_INTR) |= 1;
    if (*reg(NV_PCRTC_INTR_EN) & 1) kernel_raise_irq(NV_GPU_IRQ);
}
