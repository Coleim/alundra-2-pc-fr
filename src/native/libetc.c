/*
 * libetc.c - vertical blank, VSync and the callback system, natively (PsyQ libetc).
 *
 * Time is a virtual clock advanced by the work the game does (its control transfers, 100 ns
 * each: 200 000 make a PAL frame) and by its waits for the vertical blank. A vertical blank
 * comes every 20 ms of this clock, so a loop that busy-waits for a while (a time-out counted
 * in turns of a loop, a CD frame awaited) sees the vertical blanks, CD sectors and sound it
 * would see on the console. In real time the virtual clock never runs ahead of the host
 * clock: the program sleeps until the host catches up, which paces the game at 50 Hz.
 * Deterministic mode (A2_DETERMINISTIC=1) does not sleep: runs are identical.
 */
#include <stdio.h>
#include <stdlib.h>

#include "engine.h"
#include "host.h"
#include "libaddr.h"
#include "native_modules.h"

#define FRAME_NS    20000000ull               /* PAL */
#define LINE_NS     64000ull                  /* 312.5 lines a frame */
#define EDGE_NS     100ull                    /* one control transfer of the game's code */
#define CHECK_EDGES 20000                     /* the clock is looked at every 2 ms of work */

static uint64_t s_vblanks;                    /* vertical blanks so far */
static uint64_t s_last_vsync;                 /* count when VSync last returned */
static int s_deterministic = -1;
static uint32_t s_vsync_cb[8];
static uint32_t s_irq_cb[11], s_dma_cb[8];
static uint32_t s_intr_mask;
static int s_video_mode = 1;                  /* MODE_PAL */
static volatile uint32_t s_events;

int a2n_deterministic(void)
{
    if (s_deterministic < 0) {
        const char *e = getenv("A2_DETERMINISTIC");
        s_deterministic = e && e[0] == '1';
    }
    return s_deterministic;
}

/* ---- control transfers: a2n_budget counts them down to the next look at the clock */
static uint64_t s_edge_base;                  /* transfers before the current countdown */
static int32_t s_budget_start = 0x7FFFFFFF;
static uint64_t s_cd_due_edges;               /* the CD drive's next data sector */

uint64_t a2n_edges(void) { return s_edge_base + (uint64_t)((int64_t)s_budget_start - a2n_budget); }

/* ---- the virtual clock */
static uint64_t s_vtime;                      /* ns */
static uint64_t s_vtime_edges;                /* a2n_edges() when s_vtime was brought up to date */
static uint64_t s_next_vblank = FRAME_NS;     /* virtual time of the next vertical blank */
static int64_t s_host_offset;                 /* host time = virtual time + offset */
static int s_host_synced;

static uint64_t vnow(void)
{
    const uint64_t e = a2n_edges();
    s_vtime += (e - s_vtime_edges) * EDGE_NS;
    s_vtime_edges = e;
    return s_vtime;
}

/* real time: do not run ahead of the host clock (sleep), resynchronise when far behind */
static void throttle(void)
{
    if (a2n_deterministic()) return;
    const int64_t host = (int64_t)a2n_host_now_ns();
    if (!s_host_synced) {
        s_host_synced = 1;
        s_host_offset = host - (int64_t)s_vtime;
        return;
    }
    const int64_t target = (int64_t)s_vtime + s_host_offset;
    if (target > host + 1000000) a2n_host_sleep_until((uint64_t)target);
    else if (host > target + 100000000) s_host_offset = host - (int64_t)s_vtime;
}

static void rearm(void)
{
    const uint64_t now = a2n_edges();
    const uint64_t v = vnow();
    uint64_t d = CHECK_EDGES;
    if (s_next_vblank > v) {
        const uint64_t e = (s_next_vblank - v + EDGE_NS - 1) / EDGE_NS;
        if (e < d) d = e;
    } else {
        d = 1;
    }
    if (s_cd_due_edges) {
        const uint64_t e = s_cd_due_edges > now ? s_cd_due_edges - now : 1;
        if (e < d) d = e;
    }
    if (d < 1) d = 1;
    s_edge_base = now;
    s_budget_start = (int32_t)d;
    a2n_budget = (int32_t)d;
}

/* a vertical blank is due: post it, and move the clock's mark to the next one */
static void post_vblank(void)
{
    s_next_vblank += FRAME_NS;
    s_events |= A2N_EV_VBLANK;
    a2n_pending = 1;
}

void a2n_budget_spent(void)
{
    const uint64_t v = vnow();
    if (v >= s_next_vblank) post_vblank();
    if (s_cd_due_edges && a2n_edges() >= s_cd_due_edges) {
        s_cd_due_edges = 0;
        s_events |= A2N_EV_CDTIMER;
        a2n_pending = 1;
    }
    throttle();
    rearm();
}

/* the CD drive's next data sector, `edges` transfers from now (0: none) */
void a2n_cd_timer_set(uint32_t edges)
{
    s_cd_due_edges = edges ? a2n_edges() + edges : 0;
    rearm();
}

uint64_t a2n_vblank_count(void) { return s_vblanks; }

void a2n_event_post(uint32_t ev)
{
    s_events |= ev;
    a2n_pending = 1;
}

/* one vertical blank: input, the game's callbacks, sound, picture */
static void vblank(void)
{
    s_vblanks++;
    a2n_pad_vblank();
    for (int i = 0; i < 8; ++i)
        if (s_vsync_cb[i]) a2n_call_interrupt(s_vsync_cb[i], 0, 0, 0);
    a2n_cd_vblank();
    a2n_spu_vblank();
    a2n_host_vblank();
}

void a2n_events_run(void)
{
    if (!a2n_interrupts_enabled()) { a2n_pending = 1; return; }
    for (;;) {
        const uint32_t ev = s_events;
        if (!ev) break;
        s_events = 0;
        /* a finished transfer's callback first: on the console it comes long before the
         * next sector or picture */
        if (ev & A2N_EV_DMA) a2n_dma_event();
        if (ev & A2N_EV_GPU) a2n_gpu_event();
        if (ev & A2N_EV_CD) a2n_cd_event();
        if (ev & A2N_EV_CDTIMER) a2n_cd_timer();
        if (ev & A2N_EV_VBLANK) vblank();
    }
}

/* wait for the next vertical blank (and run it): the clock jumps to it */
static void wait_vblank(void)
{
    const uint64_t target = s_vblanks + 1;
    while (s_vblanks < target) {
        const uint64_t v = vnow();
        if (!(s_events & A2N_EV_VBLANK)) {
            if (s_next_vblank > v) s_vtime = s_next_vblank;
            post_vblank();
        }
        throttle();
        a2n_events_run();
    }
    rearm();
}

/* native code waiting for something the game's interrupts bring (BIOS WaitEvent...) */
void a2n_idle(void)
{
    wait_vblank();
}

/* ---- the library ---- */

static int n_VSync(CPUState *cpu)
{
    const int32_t mode = (int32_t)cpu->gpr[4];
    if (mode < 0) return a2n_ret(cpu, (uint32_t)s_vblanks);
    if (mode == 1) {                          /* lines since the last vertical blank */
        const uint64_t v = vnow(), since = v + FRAME_NS > s_next_vblank ? v + FRAME_NS - s_next_vblank : 0;
        const uint64_t lines = since / LINE_NS;
        return a2n_ret(cpu, (uint32_t)(lines > 312 ? 312 : lines));
    }
    const uint64_t start = s_vblanks;
    if (mode > 1)
        while (s_vblanks < s_last_vsync + (uint64_t)mode - 1) wait_vblank();
    wait_vblank();
    const uint32_t lines = (uint32_t)((s_vblanks - start) * 313);
    s_last_vsync = s_vblanks;
    return a2n_ret(cpu, lines & 0xFFFF);
}

/* VSyncCallback(f) is VSyncCallbacks(4, f): slot 0 belongs to the CD library */
static int n_VSyncCallback(CPUState *cpu)
{
    const uint32_t old = s_vsync_cb[4];
    s_vsync_cb[4] = cpu->gpr[4];
    return a2n_ret(cpu, old);
}

static int n_VSyncCallbacks(CPUState *cpu)
{
    const uint32_t i = cpu->gpr[4] & 7u, old = s_vsync_cb[i];
    s_vsync_cb[i] = cpu->gpr[5];
    return a2n_ret(cpu, old);
}

/* ResetCallback: sets the callback system up once; later calls change nothing (the CD
 * library calls it again after the sound driver installed its DMA callback) */
static int n_ResetCallback(CPUState *cpu)
{
    static int done;
    if (!done) {
        done = 1;
        for (int i = 0; i < 8; ++i) s_vsync_cb[i] = s_dma_cb[i] = 0;
        for (int i = 0; i < 11; ++i) s_irq_cb[i] = 0;
    }
    return a2n_ret(cpu, 0);
}

static int n_InterruptCallback(CPUState *cpu)
{
    const uint32_t i = cpu->gpr[4] % 11u, old = s_irq_cb[i];
    s_irq_cb[i] = cpu->gpr[5];
    return a2n_ret(cpu, old);
}

static int n_DMACallback(CPUState *cpu)
{
    const uint32_t i = cpu->gpr[4] & 7u, old = s_dma_cb[i];
    if (cpu->gpr[5] != old) {
        s_dma_cb[i] = cpu->gpr[5];
        a2n_dma_irq_enable((int)i, cpu->gpr[5] != 0);
    }
    return a2n_ret(cpu, old);
}

static int n_CheckCallback(CPUState *cpu) { return a2n_ret(cpu, (uint32_t)a2n_in_interrupt()); }
static int n_StopCallback(CPUState *cpu) { return a2n_ret(cpu, 0); }
static int n_RestartCallback(CPUState *cpu) { return a2n_ret(cpu, 0); }

static int n_SetIntrMask(CPUState *cpu)
{
    const uint32_t old = s_intr_mask;
    s_intr_mask = cpu->gpr[4];
    return a2n_ret(cpu, old);
}

static int n_SetVideoMode(CPUState *cpu)
{
    const int old = s_video_mode;
    s_video_mode = (int)cpu->gpr[4];
    return a2n_ret(cpu, (uint32_t)old);
}

static int n_GetVideoMode(CPUState *cpu) { return a2n_ret(cpu, (uint32_t)s_video_mode); }

uint32_t a2n_dma_callback(int channel) { return s_dma_cb[channel & 7]; }
uint32_t a2n_irq_callback(int irq) { return s_irq_cb[irq % 11]; }

void a2n_libetc_init(void)
{
    s_edge_base = 0;
    s_budget_start = a2n_budget = 0x7FFFFFFF;
    s_cd_due_edges = 0;
    s_vtime = s_vtime_edges = 0;
    s_next_vblank = FRAME_NS;
    s_host_synced = 0;
    rearm();
    a2n_native(LIB_VSync, "VSync", n_VSync);
    a2n_native(LIB_VSyncCallback, "VSyncCallback", n_VSyncCallback);
    a2n_native(LIB_VSyncCallbacks, "VSyncCallbacks", n_VSyncCallbacks);
    a2n_native(LIB_ResetCallback, "ResetCallback", n_ResetCallback);
    a2n_native(LIB_InterruptCallback, "InterruptCallback", n_InterruptCallback);
    a2n_native(LIB_DMACallback, "DMACallback", n_DMACallback);
    a2n_native(LIB_CheckCallback, "CheckCallback", n_CheckCallback);
    a2n_native(LIB_StopCallback, "StopCallback", n_StopCallback);
    a2n_native(LIB_RestartCallback, "RestartCallback", n_RestartCallback);
    a2n_native(LIB_SetIntrMask, "SetIntrMask", n_SetIntrMask);
    a2n_native(LIB_SetVideoMode, "SetVideoMode", n_SetVideoMode);
    a2n_native(LIB_GetVideoMode, "GetVideoMode", n_GetVideoMode);
}
