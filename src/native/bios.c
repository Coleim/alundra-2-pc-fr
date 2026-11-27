/*
 * bios.c - the BIOS functions the game calls (tables A0, B0, C0), natively.
 *
 * Only what the game and the library code it still runs need: cache flush, heap set-up,
 * console output, kernel events, exception and interrupt plumbing (inert: interrupts are
 * delivered by the engine), memory-card and pad set-up (handled by the native libpad and
 * libmcrd). Anything else is reported once and answers 0.
 */
#include <stdio.h>
#include <string.h>

#include "engine.h"
#include "native_modules.h"

/* ---- kernel events (OpenEvent / TestEvent / DeliverEvent ...) ---- */

typedef struct {
    uint32_t cls, spec, mode, func;
    int used, enabled, ready;
} Event;
static Event s_ev[32];

static uint32_t open_event(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func)
{
    for (int i = 0; i < 32; ++i) {
        if (!s_ev[i].used) {
            s_ev[i] = (Event){ cls, spec, mode, func, 1, 0, 0 };
            return 0xF1000000u | (uint32_t)i;
        }
    }
    return 0xFFFFFFFFu;
}

static Event *event_of(uint32_t h)
{
    const uint32_t i = h & 0xFFFFu;
    return (h & 0xFFFF0000u) == 0xF1000000u && i < 32 && s_ev[i].used ? &s_ev[i] : NULL;
}

void a2n_deliver_event(uint32_t cls, uint32_t spec)
{
    for (int i = 0; i < 32; ++i) {
        Event *e = &s_ev[i];
        if (!e->used || !e->enabled || e->cls != cls || e->spec != spec) continue;
        if (e->mode == 0x1000 && e->func) a2n_call_interrupt(e->func, 0, 0, 0);
        else e->ready = 1;
    }
}

/* ---- console output ---- */

static void out_text(uint32_t addr, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i) fputc((int)a2n_r8(addr + i), stderr);
}

/* ---- the tables ---- */

static void unknown(uint32_t table, uint32_t fn)
{
    static uint8_t seen[3][256];
    const int t = table == 0xA0 ? 0 : table == 0xB0 ? 1 : 2;
    if (!seen[t][fn & 0xFF]) {
        seen[t][fn & 0xFF] = 1;
        fprintf(stderr, "a2: BIOS %02X:%02X not provided (ra %08x)\n", table, fn, a2n_cpu.gpr[31]);
    }
}

static uint32_t s_heap, s_heap_size;

static uint32_t a0(CPUState *cpu, uint32_t fn)
{
    switch (fn) {
    case 0x06: case 0x3A:                                   /* exit, SystemErrorExit */
        a2n_fatal("the game called exit(%d)", (int)cpu->gpr[4]);
        return 0;
    case 0x39:                                              /* InitHeap */
        s_heap = cpu->gpr[4];
        s_heap_size = cpu->gpr[5];
        return 0;
    case 0x3C:                                              /* std_out_putchar */
        fputc((int)(cpu->gpr[4] & 0xFF), stderr);
        return cpu->gpr[4];
    case 0x3E: {                                            /* std_out_puts */
        uint32_t a = cpu->gpr[4];
        for (uint8_t c; (c = (uint8_t)a2n_r8(a)) != 0; ++a) fputc(c, stderr);
        return 0;
    }
    case 0x44:                                              /* FlushCache */
    case 0x56: case 0x72:                                   /* CdRemove */
    case 0x45: case 0x9F:
        return 0;
    case 0x49: case 0x4A: case 0x4B: case 0x48:             /* GPU helpers: native libgpu */
        return 0;
    case 0xAB: case 0xAC:                                   /* card info / directory: native libmcrd */
        return 0;
    default:
        unknown(0xA0, fn);
        return 0;
    }
}

static uint32_t b0(CPUState *cpu, uint32_t fn)
{
    switch (fn) {
    case 0x07:                                              /* DeliverEvent */
        a2n_deliver_event(cpu->gpr[4], cpu->gpr[5]);
        return 0;
    case 0x08:                                              /* OpenEvent */
        return open_event(cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
    case 0x09: {                                            /* CloseEvent */
        Event *e = event_of(cpu->gpr[4]);
        if (e) e->used = 0;
        return 1;
    }
    case 0x0A: case 0x0B: {                                 /* WaitEvent, TestEvent */
        Event *e = event_of(cpu->gpr[4]);
        if (!e) return 0;
        if (fn == 0x0A) while (!e->ready) a2n_idle();
        const int r = e->ready;
        e->ready = 0;
        return (uint32_t)r;
    }
    case 0x0C: case 0x0D: {                                 /* EnableEvent, DisableEvent */
        Event *e = event_of(cpu->gpr[4]);
        if (e) e->enabled = fn == 0x0C;
        return 1;
    }
    case 0x20:                                              /* UnDeliverEvent */
        for (int i = 0; i < 32; ++i)
            if (s_ev[i].used && s_ev[i].cls == cpu->gpr[4] && s_ev[i].spec == cpu->gpr[5]) s_ev[i].ready = 0;
        return 0;
    case 0x12: case 0x13: case 0x14: case 0x15:             /* pads: native libpad */
    case 0x17: case 0x18: case 0x19:                        /* exceptions: the engine's */
    case 0x4A: case 0x4B: case 0x4C: case 0x50:             /* memory cards: native libmcrd */
    case 0x5B:                                              /* ChangeClearPad */
        return 1;
    case 0x35:                                              /* FileWrite: console */
        if (cpu->gpr[4] == 1) out_text(cpu->gpr[5], cpu->gpr[6]);
        return cpu->gpr[6];
    case 0x3D:                                              /* std_out_putchar */
        fputc((int)(cpu->gpr[4] & 0xFF), stderr);
        return cpu->gpr[4];
    case 0x3F: {                                            /* std_out_puts */
        uint32_t a = cpu->gpr[4];
        for (uint8_t c; (c = (uint8_t)a2n_r8(a)) != 0; ++a) fputc(c, stderr);
        return 0;
    }
    case 0x51:                                              /* Krom2RawAdd: no kanji ROM */
        return 0xFFFFFFFFu;
    case 0x56: case 0x57:                                   /* GetC0Table, GetB0Table */
        return 0;
    default:
        unknown(0xB0, fn);
        return 0;
    }
}

static uint32_t c0(CPUState *cpu, uint32_t fn)
{
    (void)cpu;
    switch (fn) {
    case 0x02: case 0x03:                                   /* SysEnqIntRP, SysDeqIntRP */
    case 0x0A:                                              /* ChangeClearRCnt */
    case 0x0D:                                              /* SetIrqAutoAck */
        return 0;
    default:
        unknown(0xC0, fn);
        return 0;
    }
}

void a2n_bios_call(CPUState *cpu, uint32_t table)
{
    const uint32_t fn = cpu->gpr[9] & 0xFFu;
    const uint32_t r = table == 0xA0 ? a0(cpu, fn) : table == 0xB0 ? b0(cpu, fn) : c0(cpu, fn);
    cpu->gpr[2] = r;
}

void a2n_bios_init(void)
{
    memset(s_ev, 0, sizeof s_ev);
}
