/*
 * libetc.c - vertical blank, VSync and the callback system, natively (PsyQ libetc).
 *
 * The PAL console has a vertical blank every 20 ms. Real-time mode follows the host
 * clock: a vertical blank is due every 20 ms, and the game sees it at its next control
 * transfer or while it waits in VSync. Deterministic mode (A2_DETERMINISTIC=1, used to
 * compare runs) has no clock: a vertical blank happens when the game waits for one.
 */
#include <stdio.h>
#include <stdlib.h>

#include "engine.h"
#include "host.h"
#include "libaddr.h"
#include "native_modules.h"

#define FRAME_NS 20000000ull                 /* PAL */
#define LINE_NS  64000ull                    /* 312.5 lines a frame */
#define FRAME_EDGES 200000                   /* deterministic mode: work worth a frame */

static uint64_t s_vblanks;                    /* vertical blanks so far */
static uint64_t s_last_vsync;                 /* count when VSync last returned */
static uint64_t s_next_ns, s_last_vblank_ns;
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

/* ---- the work clock: control transfers of the translated code (a2n_budget counts them
 * down to the next timer). Deterministic mode: a vertical blank also comes after a frame's
 * worth of work, so busy-waiting code sees time pass. Both modes: data sectors from the CD
 * come a little work apart, so the game's main code runs between two of them. */
static uint64_t s_edge_base;                  /* transfers before the current countdown */
static int32_t s_budget_start = 0x7FFFFFFF;
static uint64_t s_vblank_due_edges, s_cd_due_edges;

uint64_t a2n_edges(void) { return s_edge_base + (uint64_t)((int64_t)s_budget_start - a2n_budget); }

static void rearm(void)
{
    const uint64_t now = a2n_edges();
    uint64_t next = UINT64_MAX;
    if (a2n_deterministic() && s_vblank_due_edges) next = s_vblank_due_edges;
    if (s_cd_due_edges && s_cd_due_edges < next) next = s_cd_due_edges;
    int64_t d = next == UINT64_MAX ? 0x7FFFFFFF : (int64_t)(next - now);
    if (next != UINT64_MAX && next <= now) d = 1;
    if (d > 0x7FFFFFFF) d = 0x7FFFFFFF;
    s_edge_base = now;
    s_budget_start = (int32_t)d;
    a2n_budget = (int32_t)d;
}

void a2n_budget_spent(void)
{
    const uint64_t now = a2n_edges();
    if (a2n_deterministic() && s_vblank_due_edges && now >= s_vblank_due_edges) {
        s_vblank_due_edges = now + FRAME_EDGES;
        s_events |= A2N_EV_VBLANK;
        a2n_pending = 1;
    }
    if (s_cd_due_edges && now >= s_cd_due_edges) {
        s_cd_due_edges = 0;
        s_events |= A2N_EV_CDTIMER;
        a2n_pending = 1;
    }
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
    if (a2n_deterministic()) {
        s_vblank_due_edges = a2n_edges() + FRAME_EDGES;
        rearm();
    }
    s_last_vblank_ns = a2n_deterministic() ? 0 : a2n_host_now_ns();
    a2n_pad_vblank();
    for (int i = 0; i < 8; ++i)
        if (s_vsync_cb[i]) a2n_call_interrupt(s_vsync_cb[i], 0, 0, 0);
    a2n_cd_vblank();
    a2n_spu_vblank();
    a2n_host_vblank();
}

/* host clock: is a vertical blank due? */
static void poll_clock(void)
{
    if (a2n_deterministic()) return;
    const uint64_t now = a2n_host_now_ns();
    if (!s_next_ns) s_next_ns = now + FRAME_NS;
    if (now >= s_next_ns) {
        s_next_ns += FRAME_NS;
        if (now > s_next_ns + 5 * FRAME_NS) s_next_ns = now + FRAME_NS;   /* far behind: resync */
        s_events |= A2N_EV_VBLANK;
    }
}

void a2n_events_run(void)
{
    if (!a2n_interrupts_enabled()) { a2n_pending = 1; return; }
    poll_clock();
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

/* the translated code checks a2n_pending at every control transfer; the host clock raises it */
void a2n_clock_tick(void)
{
    if (!a2n_deterministic()) a2n_pending = 1;
}

/* wait for the next vertical blank (and run it) */
static void wait_vblank(void)
{
    const uint64_t target = s_vblanks + 1;
    while (s_vblanks < target) {
        if (a2n_deterministic()) {
            s_events |= A2N_EV_VBLANK;
        } else {
            if (!s_next_ns) s_next_ns = a2n_host_now_ns() + FRAME_NS;
            a2n_host_sleep_until(s_next_ns);
            poll_clock();
        }
        a2n_events_run();
    }
}

/* ---- the library ---- */

static int n_VSync(CPUState *cpu)
{
    const int32_t mode = (int32_t)cpu->gpr[4];
    if (mode < 0) return a2n_ret(cpu, (uint32_t)s_vblanks);
    if (mode == 1) {
        uint64_t lines = 100;
        if (!a2n_deterministic() && s_last_vblank_ns)
            lines = (a2n_host_now_ns() - s_last_vblank_ns) / LINE_NS;
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
    s_vblank_due_edges = a2n_deterministic() ? FRAME_EDGES : 0;
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
