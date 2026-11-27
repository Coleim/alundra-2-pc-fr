/*
 * libgpu.c - the PsyQ GPU library's drawing entry points, on the software GPU (gpu.c).
 *
 * The original queues command lists for the GPU's DMA; here they are drawn at once:
 * DrawOTag walks the ordering table (each packet: a tag with the next address and the
 * number of words, then the GPU words), DrawSync has nothing to wait for.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "gpu.h"
#include "libaddr.h"
#include "native_modules.h"

static uint8_t s_drawenv[0x5C], s_dispenv[0x14];
static int s_disp_on;
static uint32_t s_drawsync_cb;

static int16_t rd16(uint32_t a) { return (int16_t)a2n_r16(a); }

static long s_dump_at = -2;     /* A2_DUMP_DRAW_AT=vblank: every packet of that frame on stderr */

/* execute the GPU words of one packet */
static void packet(uint32_t addr, uint32_t words)
{
    uint32_t buf[64];
    if (words > 64) words = 64;
    for (uint32_t i = 0; i < words; ++i) buf[i] = a2n_r32(addr + 4u * i);
    if (s_dump_at == -2) s_dump_at = getenv("A2_DUMP_DRAW_AT") ? atol(getenv("A2_DUMP_DRAW_AT")) : -1;
    if (s_dump_at >= 0 && (long)a2n_vblank_count() == s_dump_at) {
        fprintf(stderr, "a2.pkt %08x:", addr);
        for (uint32_t i = 0; i < words; ++i) fprintf(stderr, " %08x", buf[i]);
        fprintf(stderr, "\n");
    }
    int k = 0;
    while (k < (int)words) {
        const int used = gpu_command(buf + k, (int)words - k);
        k += used > 0 ? used : 1;
    }
}

static int s_log_draw = -1;     /* A2_LOG_DRAW=1: primitives drawn per DrawOTag */

void a2n_gpu_draw_list(uint32_t addr)
{
    uint32_t hist[8] = { 0 }, packets = 0;
    if (s_log_draw < 0) s_log_draw = getenv("A2_LOG_DRAW") != NULL;
    for (int guard = 0; guard < 1 << 20; ++guard) {
        const uint32_t tag = a2n_r32(addr);
        const uint32_t n = tag >> 24;
        if (n) {
            packet(addr + 4, n);
            packets++;
            hist[(a2n_r32(addr + 4) >> 29) & 7]++;
        }
        const uint32_t next = tag & 0xFFFFFFu;
        if (next == 0xFFFFFFu) break;
        addr = 0x80000000u | next;
    }
    if (s_log_draw > 0)
        fprintf(stderr, "a2.draw: vblank %llu packets %u (misc %u poly %u line %u rect %u vram %u env %u) area %d,%d-%d,%d off %d,%d\n",
                (unsigned long long)a2n_vblank_count(), packets, hist[0], hist[1], hist[2], hist[3], hist[4] + hist[5] + hist[6],
                hist[7], g_gpu.area_x0, g_gpu.area_y0, g_gpu.area_x1, g_gpu.area_y1, g_gpu.off_x, g_gpu.off_y);
}

static int n_DrawOTag(CPUState *cpu)
{
    a2n_gpu_draw_list(cpu->gpr[4]);
    if (s_drawsync_cb) a2n_event_post(A2N_EV_GPU);
    return a2n_ret(cpu, 0);
}

static int n_DrawPrim(CPUState *cpu)
{
    const uint32_t tag = a2n_r32(cpu->gpr[4]);
    packet(cpu->gpr[4] + 4, tag >> 24);
    return a2n_ret(cpu, 0);
}

static int n_DrawSync(CPUState *cpu) { return a2n_ret(cpu, 0); }

static int n_DrawSyncCallback(CPUState *cpu)
{
    const uint32_t old = s_drawsync_cb;
    s_drawsync_cb = cpu->gpr[4];
    return a2n_ret(cpu, old);
}

void a2n_gpu_event(void)
{
    if (s_drawsync_cb) a2n_call_interrupt(s_drawsync_cb, 0, 0, 0);
}

static int n_LoadImage(CPUState *cpu)
{
    const uint32_t r = cpu->gpr[4], src = cpu->gpr[5];
    const int x = rd16(r), y = rd16(r + 2), w = rd16(r + 4), h = rd16(r + 6);
    if (w > 0 && h > 0) {
        const uint8_t *p = a2n_ptr(src, (uint32_t)(w * h * 2));
        if (p) gpu_load(x, y, w, h, (const uint16_t *)p);
    }
    return a2n_ret(cpu, 0);
}

static int n_StoreImage(CPUState *cpu)
{
    const uint32_t r = cpu->gpr[4], dst = cpu->gpr[5];
    const int x = rd16(r), y = rd16(r + 2), w = rd16(r + 4), h = rd16(r + 6);
    if (w > 0 && h > 0) {
        uint8_t *p = a2n_ptr(dst, (uint32_t)(w * h * 2));
        a2n_check_write(dst, (uint32_t)(w * h * 2), "StoreImage");
        if (p) gpu_store(x, y, w, h, (uint16_t *)p);
    }
    return a2n_ret(cpu, 0);
}

static int n_MoveImage(CPUState *cpu)
{
    const uint32_t r = cpu->gpr[4];
    const int x = rd16(r), y = rd16(r + 2), w = rd16(r + 4), h = rd16(r + 6);
    if (w > 0 && h > 0) gpu_copy(x, y, (int)(cpu->gpr[5] & 0x3FF), (int)(cpu->gpr[6] & 0x1FF), w, h);
    return a2n_ret(cpu, 0);
}

static int n_ClearImage(CPUState *cpu)
{
    const uint32_t r = cpu->gpr[4];
    const uint32_t color = (cpu->gpr[5] & 0xFF) | ((cpu->gpr[6] & 0xFF) << 8) | ((cpu->gpr[7] & 0xFF) << 16);
    gpu_fill(rd16(r), rd16(r + 2), rd16(r + 4), rd16(r + 6), color);
    return a2n_ret(cpu, 0);
}

/* ClearOTag: entries point to the next one; ClearOTagR: to the previous one (reverse) */
static int n_ClearOTag(CPUState *cpu)
{
    const uint32_t ot = cpu->gpr[4], n = cpu->gpr[5];
    for (uint32_t i = 0; i + 1 < n; ++i) a2n_w32(ot + 4 * i, (ot + 4 * (i + 1)) & 0xFFFFFFu);
    if (n) a2n_w32(ot + 4 * (n - 1), 0xFFFFFFu);
    return a2n_ret(cpu, ot);
}

static int n_ClearOTagR(CPUState *cpu)
{
    const uint32_t ot = cpu->gpr[4], n = cpu->gpr[5];
    for (uint32_t i = n; i-- > 1;) a2n_w32(ot + 4 * i, (ot + 4 * (i - 1)) & 0xFFFFFFu);
    if (n) a2n_w32(ot, 0xFFFFFFu);
    return a2n_ret(cpu, ot);
}

/* PutDrawEnv: drawing area, offset, texture window and page, optional clear */
static int n_PutDrawEnv(CPUState *cpu)
{
    const uint32_t e = cpu->gpr[4];
    for (int i = 0; i < 0x5C; ++i) s_drawenv[i] = (uint8_t)a2n_r8(e + (uint32_t)i);
    const int cx = rd16(e), cy = rd16(e + 2), cw = rd16(e + 4), ch = rd16(e + 6);
    g_gpu.area_x0 = cx < 0 ? 0 : cx > 1023 ? 1023 : cx;
    g_gpu.area_y0 = cy < 0 ? 0 : cy > 511 ? 511 : cy;
    int x1 = cx + cw - 1, y1 = cy + ch - 1;
    g_gpu.area_x1 = x1 < 0 ? 0 : x1 > 1023 ? 1023 : x1;
    g_gpu.area_y1 = y1 < 0 ? 0 : y1 > 511 ? 511 : y1;
    g_gpu.off_x = rd16(e + 8);
    g_gpu.off_y = rd16(e + 10);
    const int tx = rd16(e + 0x0C), ty = rd16(e + 0x0E), tw = rd16(e + 0x10), th = rd16(e + 0x12);
    g_gpu.tw_mask_x = ((~(tw - 1)) & 0xFF) >> 3;
    g_gpu.tw_mask_y = ((~(th - 1)) & 0xFF) >> 3;
    g_gpu.tw_off_x = (tx >> 3) & 0x1F;
    g_gpu.tw_off_y = (ty >> 3) & 0x1F;
    if (!tw) g_gpu.tw_mask_x = 0;
    if (!th) g_gpu.tw_mask_y = 0;
    const uint32_t tpage = a2n_r16(e + 0x14);
    gpu_set_texpage((tpage & 0x9FF) | ((uint32_t)a2n_r8(e + 0x16) << 9) | ((uint32_t)a2n_r8(e + 0x17) << 10));
    if (a2n_r8(e + 0x18)) {
        const uint32_t c = a2n_r8(e + 0x19) | (a2n_r8(e + 0x1A) << 8) | (a2n_r8(e + 0x1B) << 16);
        gpu_fill(cx, cy, cw, ch, c);
    }
    return a2n_ret(cpu, e);
}

static int n_GetDrawEnv(CPUState *cpu)
{
    for (int i = 0; i < 0x5C; ++i) a2n_w8(cpu->gpr[4] + (uint32_t)i, s_drawenv[i]);
    return a2n_ret(cpu, cpu->gpr[4]);
}

/* The next three record what the picture needs, then let the library's own code run (return
 * 0): it keeps the library's variables up to date (ResetGraph sets the drawing limits that
 * SetDrawArea clamps to) and its writes to the GPU control port change nothing here. */
static int n_PutDispEnv(CPUState *cpu)
{
    for (int i = 0; i < 0x14; ++i) s_dispenv[i] = (uint8_t)a2n_r8(cpu->gpr[4] + (uint32_t)i);
    return 0;
}

static int n_SetDispMask(CPUState *cpu)
{
    s_disp_on = cpu->gpr[4] != 0;
    return 0;
}

static int n_ResetGraph(CPUState *cpu)
{
    const uint32_t mode = cpu->gpr[4] & 3;
    if (mode == 0 || mode == 3) {
        gpu_reset();
        s_disp_on = 0;
    }
    return 0;
}

const uint16_t *a2n_gpu_vram(void) { return g_vram; }

void a2n_gpu_display(int *x, int *y, int *w, int *h, int *depth24, int *enabled)
{
    int16_t v[8];
    memcpy(v, s_dispenv, sizeof v);
    *x = v[0];
    *y = v[1];
    *w = v[2] ? v[2] : 320;
    *h = v[7] ? v[7] : (v[3] ? v[3] : 240);
    if (*h > 256) *h = 256;
    *depth24 = s_dispenv[0x11] != 0;
    *enabled = s_disp_on;
}

void a2n_gpu_init(void)
{
    gpu_reset();
    a2n_native(LIB_DrawOTag, "DrawOTag", n_DrawOTag);
    a2n_native(LIB_DrawPrim, "DrawPrim", n_DrawPrim);
    a2n_native(LIB_DrawSync, "DrawSync", n_DrawSync);
    a2n_native(LIB_DrawSyncCallback, "DrawSyncCallback", n_DrawSyncCallback);
    a2n_native(LIB_LoadImage, "LoadImage", n_LoadImage);
    a2n_native(LIB_StoreImage, "StoreImage", n_StoreImage);
    a2n_native(LIB_MoveImage, "MoveImage", n_MoveImage);
    a2n_native(LIB_ClearImage, "ClearImage", n_ClearImage);
    a2n_native(LIB_ClearImage2, "ClearImage2", n_ClearImage);
    a2n_native(LIB_ClearOTag, "ClearOTag", n_ClearOTag);
    a2n_native(LIB_ClearOTagR, "ClearOTagR", n_ClearOTagR);
    a2n_native(LIB_PutDrawEnv, "PutDrawEnv", n_PutDrawEnv);
    a2n_native(LIB_GetDrawEnv, "GetDrawEnv", n_GetDrawEnv);
    a2n_native(LIB_PutDispEnv, "PutDispEnv", n_PutDispEnv);
    a2n_native(LIB_SetDispMask, "SetDispMask", n_SetDispMask);
    a2n_native(LIB_ResetGraph, "ResetGraph", n_ResetGraph);
}
