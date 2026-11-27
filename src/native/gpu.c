/*
 * gpu.c - the PlayStation GPU's drawing, in software: VRAM, drawing environment, and the
 * primitives the game sends (polygons, lines, rectangles, fills, VRAM copies).
 *
 * Written from the public description of the GPU command set and of its rendering rules:
 * 15-bit VRAM of 1024x512, 4/8/15-bit textures with palettes, texture window, four
 * semi-transparency modes, mask bit, 4x4 dithering of shaded pixels, drawing area and
 * offset, right and bottom edges of polygons left out.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"

uint16_t g_vram[VRAM_W * VRAM_H];
GpuState g_gpu;
uint32_t *g_gpu_owner;          /* diagnostics (tools/gpucheck): command that wrote each pixel */
uint32_t g_gpu_cmd_index;
int g_gpu_knob[8];              /* experiments (tools/gpucheck --knob i=v) */
int g_gpu_dbg_x = -1, g_gpu_dbg_y = -1;   /* tools/gpucheck --pixel: trace one pixel */

static const int8_t s_dither[4][4] = {
    { -4, 0, -3, 1 },
    { 2, -2, 3, -1 },
    { -3, 1, -4, 0 },
    { 3, -1, 2, -2 },
};

static uint8_t s_dither_lut[4][4][512];        /* 8-bit color + offset -> 5 bits */

void gpu_reset(void)
{
    memset(&g_gpu, 0, sizeof g_gpu);
    g_gpu.area_x1 = VRAM_W - 1;
    g_gpu.area_y1 = VRAM_H - 1;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            for (int c = 0; c < 512; ++c) {
                int v = (c & 0xFF) + s_dither[y][x];
                if (c > 0xFF) v = 0xFF + s_dither[y][x];
                v = v < 0 ? 0 : v > 0xFF ? 0xFF : v;
                s_dither_lut[y][x][c] = (uint8_t)(v >> 3);
            }
}

/* ---- environment (GP0 E1..E6) ---- */

void gpu_set_texpage(uint32_t w)
{
    g_gpu.tex_x = (int)(w & 0xF) * 64;
    g_gpu.tex_y = (int)((w >> 4) & 1) * 256;
    g_gpu.semi_mode = (int)((w >> 5) & 3);
    g_gpu.tex_depth = (int)((w >> 7) & 3);
    g_gpu.dither = (int)((w >> 9) & 1);
    g_gpu.draw_display = (int)((w >> 10) & 1);
    g_gpu.tex_disable = 0;
    g_gpu.flip_x = (int)((w >> 12) & 1);
    g_gpu.flip_y = (int)((w >> 13) & 1);
    g_gpu.texpage_raw = w & 0x3FFF;
}

static void env_command(uint32_t w)
{
    switch (w >> 24) {
    case 0xE1:
        gpu_set_texpage(w);
        break;
    case 0xE2:
        g_gpu.tw_mask_x = (int)(w & 0x1F);
        g_gpu.tw_mask_y = (int)((w >> 5) & 0x1F);
        g_gpu.tw_off_x = (int)((w >> 10) & 0x1F);
        g_gpu.tw_off_y = (int)((w >> 15) & 0x1F);
        break;
    case 0xE3:
        g_gpu.area_x0 = (int)(w & 0x3FF);
        g_gpu.area_y0 = (int)((w >> 10) & 0x3FF);
        break;
    case 0xE4:
        g_gpu.area_x1 = (int)(w & 0x3FF);
        g_gpu.area_y1 = (int)((w >> 10) & 0x3FF);
        break;
    case 0xE5: {
        int x = (int)(w & 0x7FF), y = (int)((w >> 11) & 0x7FF);
        g_gpu.off_x = (x ^ 0x400) - 0x400;               /* 11-bit signed */
        g_gpu.off_y = (y ^ 0x400) - 0x400;
        break;
    }
    case 0xE6:
        g_gpu.mask_set = (int)(w & 1);
        g_gpu.mask_check = (int)((w >> 1) & 1);
        break;
    default:
        break;
    }
}

/* ---- pixels ---- */

static inline int in_area(int x, int y)
{
    return x >= g_gpu.area_x0 && x <= g_gpu.area_x1 && y >= g_gpu.area_y0 && y <= g_gpu.area_y1;
}

static inline uint16_t blend(uint16_t back, uint16_t front, int mode)
{
    int br = back & 31, bg = (back >> 5) & 31, bb = (back >> 10) & 31;
    int fr = front & 31, fg = (front >> 5) & 31, fb = (front >> 10) & 31;
    int r, g, b;
    switch (mode) {
    case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
    case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
    case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
    default: r = br + (fr >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2); break;
    }
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    g = g < 0 ? 0 : g > 31 ? 31 : g;
    b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

/* write a 15-bit color (bit 15 = the texel's semi-transparency/mask bit) */
static inline void plot(int x, int y, uint16_t c, int semi)
{
    uint16_t *p = &g_vram[(y & 511) * VRAM_W + (x & 1023)];
    if (g_gpu.mask_check && (*p & 0x8000)) return;
    if (semi) c = (uint16_t)(blend(*p, c, g_gpu.semi_mode) | (c & 0x8000));
    if (g_gpu.mask_set) c |= 0x8000;
    if (g_gpu_owner && *p != c) g_gpu_owner[p - g_vram] = g_gpu_cmd_index;
    *p = c;
}

/* texel at (u, v) of the current texture page / palette; 0 = transparent */
static inline uint16_t texel(int u, int v, int clut_x, int clut_y)
{
    u = (u & ~(g_gpu.tw_mask_x * 8)) | ((g_gpu.tw_off_x & g_gpu.tw_mask_x) * 8);
    v = (v & ~(g_gpu.tw_mask_y * 8)) | ((g_gpu.tw_off_y & g_gpu.tw_mask_y) * 8);
    u &= 0xFF;
    v &= 0xFF;
    const int ty = (g_gpu.tex_y + v) & 511;
    switch (g_gpu.tex_depth) {
    case 0: {
        const uint16_t w = g_vram[ty * VRAM_W + ((g_gpu.tex_x + (u >> 2)) & 1023)];
        const int i = (w >> ((u & 3) * 4)) & 0xF;
        return g_vram[(clut_y & 511) * VRAM_W + ((clut_x + i) & 1023)];
    }
    case 1: {
        const uint16_t w = g_vram[ty * VRAM_W + ((g_gpu.tex_x + (u >> 1)) & 1023)];
        const int i = (w >> ((u & 1) * 8)) & 0xFF;
        return g_vram[(clut_y & 511) * VRAM_W + ((clut_x + i) & 1023)];
    }
    default:
        return g_vram[ty * VRAM_W + ((g_gpu.tex_x + u) & 1023)];
    }
}

/* texel modulated by an 8-bit color (0x80 = unchanged), with optional dithering */
static inline uint16_t modulate(uint16_t t, int r, int g, int b, int dx, int dy, int dither)
{
    int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
    tr = (tr * r) >> 7;
    tg = (tg * g) >> 7;
    tb = (tb * b) >> 7;
    if (tr > 0x1FF) tr = 0x1FF;
    if (tg > 0x1FF) tg = 0x1FF;
    if (tb > 0x1FF) tb = 0x1FF;
    uint16_t c;
    if (dither) {
        const uint8_t *lut0 = (g_gpu_knob[7] ? s_dither_lut[dx & 3][dy & 3] : s_dither_lut[dy & 3][dx & 3]);
        c = (uint16_t)(lut0[tr] | (lut0[tg] << 5) | (lut0[tb] << 10));
    } else {
        c = (uint16_t)((tr > 0xFF ? 31 : tr >> 3) | ((tg > 0xFF ? 31 : tg >> 3) << 5) |
                       ((tb > 0xFF ? 31 : tb >> 3) << 10));
    }
    return (uint16_t)(c | (t & 0x8000));
}

/* experiment: modulation by a color with 8 fraction bits */
static inline uint16_t modulate_fx(uint16_t t, int rf, int gf, int bf, int dx, int dy, int dither)
{
    int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
    tr = (int)(((int64_t)tr * rf) >> 15);
    tg = (int)(((int64_t)tg * gf) >> 15);
    tb = (int)(((int64_t)tb * bf) >> 15);
    if (tr > 0x1FF) tr = 0x1FF;
    if (tg > 0x1FF) tg = 0x1FF;
    if (tb > 0x1FF) tb = 0x1FF;
    uint16_t c;
    if (dither) {
        const uint8_t *lut0 = (g_gpu_knob[7] ? s_dither_lut[dx & 3][dy & 3] : s_dither_lut[dy & 3][dx & 3]);
        c = (uint16_t)(lut0[tr] | (lut0[tg] << 5) | (lut0[tb] << 10));
    } else {
        c = (uint16_t)((tr > 0xFF ? 31 : tr >> 3) | ((tg > 0xFF ? 31 : tg >> 3) << 5) |
                       ((tb > 0xFF ? 31 : tb >> 3) << 10));
    }
    return (uint16_t)(c | (t & 0x8000));
}

static inline uint16_t shade(int r, int g, int b, int dx, int dy, int dither)
{
    if (dither) {
        const uint8_t *lut0 = (g_gpu_knob[7] ? s_dither_lut[dx & 3][dy & 3] : s_dither_lut[dy & 3][dx & 3]);
        return (uint16_t)(lut0[r] | (lut0[g] << 5) | (lut0[b] << 10));
    }
    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

/* ---- polygons ---- */

typedef struct {
    int x, y;
    int r, g, b;
    int u, v;
} Vtx;

/* attribute gradients across the triangle, in 1/2^32 units */
typedef struct {
    int64_t dx, dy, base;
} Grad;

static void grad(Grad *gr, const Vtx *v0, const Vtx *v1, const Vtx *v2, int a0, int a1, int a2, int64_t area)
{
    const int64_t d1 = a1 - a0, d2 = a2 - a0;
    const int64_t y1 = v1->y - v0->y, y2 = v2->y - v0->y, x1 = v1->x - v0->x, x2 = v2->x - v0->x;
    gr->dx = ((d1 * y2 - d2 * y1) * ((int64_t)1 << 32)) / area;
    gr->dy = ((d2 * x1 - d1 * x2) * ((int64_t)1 << 32)) / area;
    if (g_gpu_knob[1] > 0) {                         /* experiment: gradients with fewer fraction bits */
        const int sh = 32 - g_gpu_knob[1];
        if (g_gpu_knob[5] == 0) {                    /* toward zero */
            gr->dx = gr->dx >= 0 ? (gr->dx >> sh) << sh : -((-gr->dx >> sh) << sh);
            gr->dy = gr->dy >= 0 ? (gr->dy >> sh) << sh : -((-gr->dy >> sh) << sh);
        } else {                                     /* floor */
            gr->dx = (gr->dx >> sh) << sh;
            gr->dy = (gr->dy >> sh) << sh;
        }
    }
    /* values are truncated (closest to the reference runtime on captured frames) */
    gr->base = ((int64_t)a0 << 32) + (g_gpu_knob[0] == 1 ? ((int64_t)1 << 31) : 0);
}

static inline int at(const Grad *gr, int x, int y, int x0, int y0)
{
    return (int)((gr->base + gr->dx * (x - x0) + gr->dy * (y - y0)) >> 32);
}

/* the same with 8 fraction bits kept */
static inline int at8(const Grad *gr, int x, int y, int x0, int y0)
{
    return (int)((gr->base + gr->dx * (x - x0) + gr->dy * (y - y0)) >> 24);
}

static void triangle(const Vtx *a, const Vtx *b, const Vtx *c, int gouraud, int textured, int semi,
                     int raw, int clut_x, int clut_y)
{
    /* too large: the GPU skips it */
    const int minx = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    const int maxx = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    const int miny = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    const int maxy = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;
    int64_t area = (int64_t)(b->x - a->x) * (c->y - a->y) - (int64_t)(c->x - a->x) * (b->y - a->y);
    if (area == 0) return;
    const Vtx *v0 = a, *v1 = b, *v2 = c;
    if (area < 0) { v1 = c; v2 = b; area = -area; }      /* counter-clockwise in screen space */

    int x0 = minx, x1 = maxx - 1, y0 = miny, y1 = maxy - 1;
    if (x0 < g_gpu.area_x0) x0 = g_gpu.area_x0;
    if (y0 < g_gpu.area_y0) y0 = g_gpu.area_y0;
    if (x1 > g_gpu.area_x1) x1 = g_gpu.area_x1;
    if (y1 > g_gpu.area_y1) y1 = g_gpu.area_y1;
    if (x0 > x1 || y0 > y1) return;

    Grad gr, gg, gb, gu, gv;
    if (gouraud) {
        grad(&gr, v0, v1, v2, v0->r, v1->r, v2->r, area);
        grad(&gg, v0, v1, v2, v0->g, v1->g, v2->g, area);
        grad(&gb, v0, v1, v2, v0->b, v1->b, v2->b, area);
    }
    if (textured) {
        grad(&gu, v0, v1, v2, v0->u, v1->u, v2->u, area);
        grad(&gv, v0, v1, v2, v0->v, v1->v, v2->v, area);
    }
    const int dither = g_gpu_knob[4] == 1 ? 0 : g_gpu_knob[4] == 2 ? (g_gpu.dither && gouraud) : (g_gpu.dither && (gouraud || (textured && !raw)));

    /* edge functions with the top-left rule: right and bottom edges are left out */
    const Vtx *e0a = v1, *e0b = v2, *e1a = v2, *e1b = v0, *e2a = v0, *e2b = v1;
    #define EDGE(pa, pb, px, py) ((int64_t)((pb)->x - (pa)->x) * ((py) - (pa)->y) - (int64_t)((pb)->y - (pa)->y) * ((px) - (pa)->x))
    #define TOPLEFT(pa, pb) (((pa)->y == (pb)->y && (pb)->x < (pa)->x) || ((pb)->y < (pa)->y))
    const int tl0 = TOPLEFT(e0a, e0b), tl1 = TOPLEFT(e1a, e1b), tl2 = TOPLEFT(e2a, e2b);

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            int64_t w0, w1, w2;
            if (g_gpu_knob[2]) {                       /* experiment: pixel centres */
                #define EDGE2(pa, pb) ((int64_t)((pb)->x - (pa)->x) * (2 * y + 1 - 2 * (pa)->y) - (int64_t)((pb)->y - (pa)->y) * (2 * x + 1 - 2 * (pa)->x))
                w0 = EDGE2(e0a, e0b); w1 = EDGE2(e1a, e1b); w2 = EDGE2(e2a, e2b);
                #undef EDGE2
            } else {
                w0 = EDGE(e0a, e0b, x, y); w1 = EDGE(e1a, e1b, x, y); w2 = EDGE(e2a, e2b, x, y);
            }
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            if ((w0 == 0 && !tl0) || (w1 == 0 && !tl1) || (w2 == 0 && !tl2)) continue;
            int r = v0->r, g = v0->g, bl = v0->b;
            int rf = r << 8, gf = g << 8, bf = bl << 8;
            if (gouraud) {
                rf = at8(&gr, x, y, v0->x, v0->y);
                gf = at8(&gg, x, y, v0->x, v0->y);
                bf = at8(&gb, x, y, v0->x, v0->y);
                rf = rf < 0 ? 0 : rf > 0xFFFF ? 0xFFFF : rf;
                gf = gf < 0 ? 0 : gf > 0xFFFF ? 0xFFFF : gf;
                bf = bf < 0 ? 0 : bf > 0xFFFF ? 0xFFFF : bf;
                r = rf >> 8;
                g = gf >> 8;
                bl = bf >> 8;
            }
            if (textured) {
                const int u = at(&gu, x, y, v0->x, v0->y), v = at(&gv, x, y, v0->x, v0->y);
                const uint16_t t = texel(u, v, clut_x, clut_y);
                if (x == g_gpu_dbg_x && y == g_gpu_dbg_y)
                    fprintf(stderr, "dbg cmd#%u px %d,%d rgb %d,%d,%d (fx %04x,%04x,%04x) uv %d,%d texel %04x dither %d semi %d bg %04x  verts (%d,%d c%02x%02x%02x uv%d,%d) (%d,%d c%02x%02x%02x uv%d,%d) (%d,%d c%02x%02x%02x uv%d,%d)\n",
                            g_gpu_cmd_index, x, y, r, g, bl, rf, gf, bf, u, v, t, dither, semi, g_vram[(y & 511) * VRAM_W + (x & 1023)],
                            v0->x, v0->y, v0->r, v0->g, v0->b, v0->u, v0->v, v1->x, v1->y, v1->r, v1->g, v1->b, v1->u, v1->v,
                            v2->x, v2->y, v2->r, v2->g, v2->b, v2->u, v2->v);
                if (!t) continue;
                const uint16_t col = raw ? t : g_gpu_knob[6] ? modulate_fx(t, rf, gf, bf, x, y, dither)
                                                             : modulate(t, r, g, bl, x, y, dither);
                plot(x, y, col, semi && (t & 0x8000));
            } else {
                plot(x, y, shade(r, g, bl, x, y, dither), semi);
            }
        }
    }
    #undef EDGE
    #undef TOPLEFT
}

/* ---- lines ---- */

static void line(const Vtx *a, const Vtx *b, int gouraud, int semi)
{
    const int dx = b->x - a->x, dy = b->y - a->y;
    if (abs(dx) >= 1024 || abs(dy) >= 512) return;
    const int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    const int dither = g_gpu.dither && gouraud;
    for (int i = 0; i <= n; ++i) {
        const int x = n ? a->x + (int)(((int64_t)dx * i * 2 + (dx >= 0 ? n : -n)) / (2 * n)) : a->x;
        const int y = n ? a->y + (int)(((int64_t)dy * i * 2 + (dy >= 0 ? n : -n)) / (2 * n)) : a->y;
        int r = a->r, g = a->g, bl = a->b;
        if (gouraud && n) {
            r = a->r + (b->r - a->r) * i / n;
            g = a->g + (b->g - a->g) * i / n;
            bl = a->b + (b->b - a->b) * i / n;
        }
        if (in_area(x, y)) plot(x, y, shade(r, g, bl, x, y, dither), semi);
    }
}

/* ---- rectangles ---- */

static void rect(int x, int y, int w, int h, int r, int g, int b, int textured, int semi, int raw,
                 int u0, int v0, int clut_x, int clut_y)
{
    for (int j = 0; j < h; ++j) {
        const int py = y + j;
        if (py < g_gpu.area_y0 || py > g_gpu.area_y1) continue;
        for (int i = 0; i < w; ++i) {
            const int px = x + i;
            if (px < g_gpu.area_x0 || px > g_gpu.area_x1) continue;
            if (textured) {
                const int u = g_gpu.flip_x && !g_gpu_knob[3] ? u0 - i : u0 + i;
                const int v = g_gpu.flip_y && !g_gpu_knob[3] ? v0 - j : v0 + j;
                const uint16_t t = texel(u, v, clut_x, clut_y);
                if (!t) continue;
                plot(px, py, raw ? t : modulate(t, r, g, b, 0, 0, 0), semi && (t & 0x8000));
            } else {
                plot(px, py, shade(r, g, b, 0, 0, 0), semi);
            }
        }
    }
}

/* ---- the command stream ---- */

static inline int sx(uint32_t w) { return ((int)(w & 0x7FF) ^ 0x400) - 0x400; }
static inline int sy(uint32_t w) { return ((int)((w >> 16) & 0x7FF) ^ 0x400) - 0x400; }

void gpu_fill(int x, int y, int w, int h, uint32_t color)
{
    x &= 0x3F0;
    y &= 0x1FF;
    w = ((w & 0x3FF) + 0xF) & ~0xF;
    h &= 0x1FF;
    const uint16_t c = (uint16_t)(((color >> 3) & 31) | (((color >> 11) & 31) << 5) | (((color >> 19) & 31) << 10));
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            g_vram[((y + j) & 511) * VRAM_W + ((x + i) & 1023)] = c;
}

/* words: one primitive's GP0 words (the command in words[0]); returns the words used */
int gpu_command(const uint32_t *w, int n)
{
    const uint32_t cmd = w[0] >> 24;
    g_gpu_cmd_index++;
    if (cmd == 0x02) {
        if (n >= 3) gpu_fill((int)(w[1] & 0xFFFF), (int)(w[1] >> 16), (int)(w[2] & 0xFFFF), (int)(w[2] >> 16), w[0]);
        return 3;
    }
    if (cmd >= 0x20 && cmd < 0x40) {
        const int gouraud = (cmd >> 4) & 1, quad = (cmd >> 3) & 1, textured = (cmd >> 2) & 1;
        const int semi = (cmd >> 1) & 1, raw = cmd & 1;
        const int nv = quad ? 4 : 3;
        Vtx v[4];
        int clut_x = 0, clut_y = 0;
        /* words: flat [cmd+color][xy][uv]... ; gouraud [cmd+c0][xy][uv] [c1][xy][uv] ... */
        if ((gouraud ? nv * (2 + textured) : 1 + nv * (1 + textured)) > n) return n;
        for (int i = 0; i < nv; ++i) {
            const int base = gouraud ? i * (2 + textured) : 1 + i * (1 + textured);
            const uint32_t cw = gouraud ? w[base] : w[0];
            const uint32_t xy = gouraud ? w[base + 1] : w[base];
            v[i].r = (int)(cw & 0xFF);
            v[i].g = (int)((cw >> 8) & 0xFF);
            v[i].b = (int)((cw >> 16) & 0xFF);
            v[i].x = sx(xy) + g_gpu.off_x;
            v[i].y = sy(xy) + g_gpu.off_y;
            v[i].u = v[i].v = 0;
            if (textured) {
                const uint32_t uv = gouraud ? w[base + 2] : w[base + 1];
                v[i].u = (int)(uv & 0xFF);
                v[i].v = (int)((uv >> 8) & 0xFF);
                if (i == 0) { clut_x = (int)((uv >> 16) & 0x3F) * 16; clut_y = (int)((uv >> 22) & 0x1FF); }
                if (i == 1) gpu_set_texpage(((uv >> 16) & 0x9FF) | (g_gpu.texpage_raw & ~0x9FFu));
            }
        }
        triangle(&v[0], &v[1], &v[2], gouraud, textured, semi, raw, clut_x, clut_y);
        if (quad) triangle(&v[1], &v[2], &v[3], gouraud, textured, semi, raw, clut_x, clut_y);
        return gouraud ? nv * (2 + textured) : 1 + nv * (1 + textured);
    }
    if (cmd >= 0x40 && cmd < 0x60) {
        const int gouraud = (cmd >> 4) & 1, poly = (cmd >> 3) & 1, semi = (cmd >> 1) & 1;
        Vtx prev = { 0 }, cur;
        int k = 1, count = 0;
        uint32_t color = w[0];
        for (;;) {
            if (count > 0 && gouraud) {
                if (k >= n) break;
                if (poly && (w[k] & 0xF000F000u) == 0x50005000u) { k++; break; }
                color = w[k++];
            }
            if (k >= n) break;
            if (count > 0 && poly && !gouraud && (w[k] & 0xF000F000u) == 0x50005000u) { k++; break; }
            const uint32_t xy = w[k++];
            cur.x = sx(xy) + g_gpu.off_x;
            cur.y = sy(xy) + g_gpu.off_y;
            cur.r = (int)(color & 0xFF);
            cur.g = (int)((color >> 8) & 0xFF);
            cur.b = (int)((color >> 16) & 0xFF);
            if (count > 0) line(&prev, &cur, gouraud, semi);
            prev = cur;
            count++;
            if (!poly && count == 2) break;
        }
        return k;
    }
    if (cmd >= 0x60 && cmd < 0x80) {
        const int size = (cmd >> 3) & 3, textured = (cmd >> 2) & 1, semi = (cmd >> 1) & 1, raw = cmd & 1;
        int k = 1;
        const uint32_t xy = w[k++];
        int u0 = 0, v0 = 0, clut_x = 0, clut_y = 0;
        if (textured) {
            const uint32_t uv = w[k++];
            u0 = (int)(uv & 0xFF);
            v0 = (int)((uv >> 8) & 0xFF);
            clut_x = (int)((uv >> 16) & 0x3F) * 16;
            clut_y = (int)((uv >> 22) & 0x1FF);
        }
        int wd, ht;
        if (size == 0) { const uint32_t wh = w[k++]; wd = (int)(wh & 0x3FF); ht = (int)((wh >> 16) & 0x1FF); }
        else if (size == 1) wd = ht = 1;
        else if (size == 2) wd = ht = 8;
        else wd = ht = 16;
        rect(sx(xy) + g_gpu.off_x, sy(xy) + g_gpu.off_y, wd, ht, (int)(w[0] & 0xFF), (int)((w[0] >> 8) & 0xFF),
             (int)((w[0] >> 16) & 0xFF), textured, semi, raw, u0, v0, clut_x, clut_y);
        return k;
    }
    if (cmd >= 0x80 && cmd < 0xA0) {                     /* VRAM to VRAM */
        if (n >= 4) gpu_copy((int)(w[1] & 0x3FF), (int)((w[1] >> 16) & 0x1FF), (int)(w[2] & 0x3FF),
                             (int)((w[2] >> 16) & 0x1FF), (int)(w[3] & 0xFFFF), (int)(w[3] >> 16));
        return 4;
    }
    if (cmd >= 0xE1 && cmd <= 0xE6) {
        env_command(w[0]);
        return 1;
    }
    return 1;                                            /* nop, cache clear, unknown */
}

void gpu_copy(int sx0, int sy0, int dx0, int dy0, int w, int h)
{
    w = ((w - 1) & 0x3FF) + 1;
    h = ((h - 1) & 0x1FF) + 1;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            uint16_t c = g_vram[((sy0 + j) & 511) * VRAM_W + ((sx0 + i) & 1023)];
            uint16_t *d = &g_vram[((dy0 + j) & 511) * VRAM_W + ((dx0 + i) & 1023)];
            if (g_gpu.mask_check && (*d & 0x8000)) continue;
            *d = (uint16_t)(c | (g_gpu.mask_set ? 0x8000 : 0));
        }
}

/* a DRAWENV (libgpu's structure, 0x5C bytes): drawing area, offset, texture window and page,
 * optional clear of the area */
void gpu_apply_drawenv(const uint8_t *e)
{
    int16_t v[10];
    memcpy(v, e, sizeof v);
    const int cx = v[0], cy = v[1], cw = v[2], ch = v[3];
    g_gpu.area_x0 = cx < 0 ? 0 : cx > 1023 ? 1023 : cx;
    g_gpu.area_y0 = cy < 0 ? 0 : cy > 511 ? 511 : cy;
    const int x1 = cx + cw - 1, y1 = cy + ch - 1;
    g_gpu.area_x1 = x1 < 0 ? 0 : x1 > 1023 ? 1023 : x1;
    g_gpu.area_y1 = y1 < 0 ? 0 : y1 > 511 ? 511 : y1;
    g_gpu.off_x = v[4];
    g_gpu.off_y = v[5];
    const int tx = v[6], ty = v[7], tw = v[8], th = v[9];
    g_gpu.tw_mask_x = tw ? ((~(tw - 1)) & 0xFF) >> 3 : 0;
    g_gpu.tw_mask_y = th ? ((~(th - 1)) & 0xFF) >> 3 : 0;
    g_gpu.tw_off_x = (tx >> 3) & 0x1F;
    g_gpu.tw_off_y = (ty >> 3) & 0x1F;
    const uint32_t tpage = (uint32_t)(e[0x14] | (e[0x15] << 8));
    gpu_set_texpage((tpage & 0x9FF) | ((uint32_t)e[0x16] << 9) | ((uint32_t)e[0x17] << 10));
    if (e[0x18]) gpu_fill(cx, cy, cw, ch, (uint32_t)e[0x19] | ((uint32_t)e[0x1A] << 8) | ((uint32_t)e[0x1B] << 16));
}

/* CPU to VRAM (LoadImage) and VRAM to CPU (StoreImage); pixels are 16-bit little-endian */
void gpu_load(int x, int y, int w, int h, const uint16_t *src)
{
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            uint16_t *d = &g_vram[((y + j) & 511) * VRAM_W + ((x + i) & 1023)];
            const uint16_t c = src[j * w + i];
            if (g_gpu.mask_check && (*d & 0x8000)) continue;
            *d = (uint16_t)(c | (g_gpu.mask_set ? 0x8000 : 0));
        }
}

void gpu_store(int x, int y, int w, int h, uint16_t *dst)
{
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            dst[j * w + i] = g_vram[((y + j) & 511) * VRAM_W + ((x + i) & 1023)];
}
