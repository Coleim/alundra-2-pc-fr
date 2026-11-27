/*
 * gpucheck - replays GPU captures of the reference runtime (A2_DIAG_GPUCAP, src/a2_diag.c)
 * through the standalone engine's software GPU (src/native/gpu.c) and compares VRAM.
 *
 *   cc -O2 -Isrc/native tools/gpucheck/gpucheck.c src/native/gpu.c -o build-native/gpucheck
 *   build-native/gpucheck PREFIX FRAME... [--diff out.ppm]
 *
 * For each frame: PREFIX.<frame>.before (VRAM), .cmds (what the game sent), .after (VRAM).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"

static uint16_t s_after[VRAM_W * VRAM_H], s_before[VRAM_W * VRAM_H];
static uint32_t s_owner[VRAM_W * VRAM_H];
static int s_show_op = -1, s_shown;

static int load(const char *path, void *dst, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t r = fread(dst, 1, n, f);
    fclose(f);
    return r == n;
}

/* per command: its first word and the GPU state that matters, for the summary */
typedef struct { uint32_t w0, w[12]; int n; uint8_t dither, semi_mode, depth, mask; int offx, offy, tx, ty; } CmdInfo;
static CmdInfo *s_cmds;
static uint32_t s_n_cmds;

static void packet(const uint32_t *w, int n)
{
    int k = 0;
    while (k < n) {
        const uint32_t idx = g_gpu_cmd_index + 1;
        if (idx >= s_n_cmds) {
            s_n_cmds = idx * 2 + 1024;
            s_cmds = realloc(s_cmds, sizeof *s_cmds * s_n_cmds);
        }
        const int used = gpu_command(w + k, n - k);
        CmdInfo ci = { 0 };
        ci.w0 = w[k];
        ci.n = used > 0 ? (used > 12 ? 12 : used) : 1;
        for (int j = 0; j < ci.n && k + j < n; ++j) ci.w[j] = w[k + j];
        ci.dither = (uint8_t)g_gpu.dither;
        ci.semi_mode = (uint8_t)g_gpu.semi_mode;
        ci.depth = (uint8_t)g_gpu.tex_depth;
        ci.mask = (uint8_t)(g_gpu.mask_set | (g_gpu.mask_check << 1));
        ci.offx = g_gpu.off_x;
        ci.offy = g_gpu.off_y;
        ci.tx = g_gpu.tex_x;
        ci.ty = g_gpu.tex_y;
        s_cmds[idx] = ci;
        k += used > 0 ? used : 1;
    }
}

static void replay(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static uint32_t buf[1 << 20];
    uint32_t hdr[2];
    while (fread(hdr, 4, 2, f) == 2) {
        const uint32_t type = hdr[0], n = hdr[1];
        if (n > (1u << 20) || fread(buf, 4, n, f) != n) break;
        switch (type) {
        case 1: case 6:                                   /* ordering-table packet, DrawPrim */
            packet(buf, (int)n);
            break;
        case 2:                                           /* DRAWENV */
            gpu_apply_drawenv((const uint8_t *)buf);
            break;
        case 3: {                                         /* LoadImage */
            const int x = (int16_t)buf[0], y = (int16_t)(buf[0] >> 16), w = (int16_t)buf[1], h = (int16_t)(buf[1] >> 16);
            if (w > 0 && h > 0) gpu_load(x, y, w, h, (const uint16_t *)(buf + 2));
            break;
        }
        case 4: {                                         /* ClearImage */
            const int x = (int16_t)buf[0], y = (int16_t)(buf[0] >> 16), w = (int16_t)buf[1], h = (int16_t)(buf[1] >> 16);
            gpu_fill(x, y, w, h, (buf[2] & 0xFF) | ((buf[3] & 0xFF) << 8) | ((buf[4] & 0xFF) << 16));
            break;
        }
        case 5: {                                         /* MoveImage */
            const int x = (int16_t)buf[0], y = (int16_t)(buf[0] >> 16), w = (int16_t)buf[1], h = (int16_t)(buf[1] >> 16);
            if (w > 0 && h > 0) gpu_copy(x, y, (int)(buf[2] & 0x3FF), (int)(buf[3] & 0x1FF), w, h);
            break;
        }
        default:
            break;
        }
    }
    fclose(f);
}

/* --probe: for each differing pixel of a 4-bit textured rectangle, which U offset the
 * reference used (searching -4..4 around ours) */
static int s_probe;

static uint16_t texel8(int tx, int ty, int u, int v, int cx, int cy)
{
    const uint16_t w = s_before[((ty + v) & 511) * VRAM_W + ((tx + ((u & 255) >> 1)) & 1023)];
    const int i = (w >> (((u & 255) & 1) * 8)) & 0xFF;
    return s_before[(cy & 511) * VRAM_W + ((cx + i) & 1023)];
}

static uint16_t texel4(int tx, int ty, int u, int v, int cx, int cy)
{
    const uint16_t w = s_before[((ty + v) & 511) * VRAM_W + ((tx + ((u & 255) >> 2)) & 1023)];
    const int i = (w >> (((u & 255) & 3) * 4)) & 0xF;
    return s_before[(cy & 511) * VRAM_W + ((cx + i) & 1023)];
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: gpucheck PREFIX FRAME... [--diff out.ppm]\n");
        return 2;
    }
    const char *prefix = argv[1], *diff = NULL;
    long total = 0, bad_total = 0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--diff") && i + 1 < argc) { diff = argv[++i]; continue; }
        if (!strcmp(argv[i], "--show") && i + 1 < argc) { s_show_op = (int)strtol(argv[++i], NULL, 16); continue; }
        if (!strcmp(argv[i], "--probe")) { s_probe = 1; continue; }
        if (!strcmp(argv[i], "--pixel") && i + 1 < argc) { sscanf(argv[++i], "%d,%d", &g_gpu_dbg_x, &g_gpu_dbg_y); continue; }
        if (!strcmp(argv[i], "--knob") && i + 1 < argc) {
            int k = 0, v = 0;
            sscanf(argv[++i], "%d=%d", &k, &v);
            if (k >= 0 && k < 8) g_gpu_knob[k] = v;
            continue;
        }
        char p[1024];
        gpu_reset();
        memset(s_owner, 0, sizeof s_owner);
        g_gpu_owner = s_owner;
        g_gpu_cmd_index = 0;
        snprintf(p, sizeof p, "%s.%s.before", prefix, argv[i]);
        if (!load(p, g_vram, sizeof g_vram)) { fprintf(stderr, "%s: missing\n", p); continue; }
        memcpy(s_before, g_vram, sizeof s_before);
        snprintf(p, sizeof p, "%s.%s.after", prefix, argv[i]);
        if (!load(p, s_after, sizeof s_after)) { fprintf(stderr, "%s: missing\n", p); continue; }
        snprintf(p, sizeof p, "%s.%s.cmds", prefix, argv[i]);
        replay(p);
        long by_op[256] = { 0 }, by_op_minor[256] = { 0 };
        long bad = 0, minor = 0, over = 0, under = 0, large = 0;
        for (int k = 0; k < VRAM_W * VRAM_H; ++k) {
            if (g_vram[k] == s_after[k]) continue;
            bad++;
            if (s_after[k] == s_before[k]) over++;
            else if (g_vram[k] == s_before[k]) under++;
            const int a = g_vram[k], b = s_after[k];
            const int dr = abs((a & 31) - (b & 31)), dg = abs(((a >> 5) & 31) - ((b >> 5) & 31)),
                      db = abs(((a >> 10) & 31) - ((b >> 10) & 31));
            const int small = dr <= 1 && dg <= 1 && db <= 1 && (a & 0x8000) == (b & 0x8000);
            if (dr > 4 || dg > 4 || db > 4) large++;
            if (small) minor++;
            const uint32_t o = s_owner[k];
            const int op = o && o < s_n_cmds ? (int)(s_cmds[o].w0 >> 24) : 0;
            by_op[op]++;
            if (small) by_op_minor[op]++;
            if (s_probe && (op == 0x64 || op == 0x65) && s_cmds[o].depth == 0) {
                const CmdInfo *ci = &s_cmds[o];
                const int x0 = ((int)(ci->w[1] & 0x7FF) ^ 0x400) - 0x400 + ci->offx;
                const int y0 = ((int)((ci->w[1] >> 16) & 0x7FF) ^ 0x400) - 0x400 + ci->offy;
                const int u0 = (int)(ci->w[2] & 0xFF), v0 = (int)((ci->w[2] >> 8) & 0xFF);
                const int cx = (int)((ci->w[2] >> 16) & 0x3F) * 16, cy = (int)((ci->w[2] >> 22) & 0x1FF);
                const int px = k % VRAM_W, py = k / VRAM_W;
                const int u = u0 + (px - x0), v = v0 + (py - y0);
                int hits = 0, hu = 0, hv = 0;
                for (int dv = -2; dv <= 2; ++dv)
                    for (int du = -2; du <= 2; ++du)
                        if ((texel4(ci->tx, ci->ty, u + du, v + dv, cx, cy) & 0x7FFF) == (s_after[k] & 0x7FFF)) { hits++; hu = du; hv = dv; }
                if (hits == 1) printf("   probe x0 %d y0 %d w %d: du %+d dv %+d (px %d,%d)\n", x0, y0, (int)(ci->w[3] & 0x3FF), hu, hv, px, py);
            }
            if (s_show_op == op && s_shown < 24) {
                s_shown++;
                const CmdInfo *ci = &s_cmds[o];
                printf("   px %4d,%3d before %04x mine %04x ref %04x  semi %d dither %d depth %d off %d,%d cmd", k % VRAM_W,
                       k / VRAM_W, s_before[k], g_vram[k], s_after[k], ci->semi_mode, ci->dither, ci->depth, ci->offx, ci->offy);
                for (int j = 0; j < ci->n; ++j) printf(" %08x", ci->w[j]);
                if ((op & 0xE4) == 0x64) {
                    const int x0 = ((int)(ci->w[1] & 0x7FF) ^ 0x400) - 0x400 + ci->offx;
                    const int y0 = ((int)((ci->w[1] >> 16) & 0x7FF) ^ 0x400) - 0x400 + ci->offy;
                    const int u = (int)(ci->w[2] & 0xFF) + (k % VRAM_W - x0), v = (int)((ci->w[2] >> 8) & 0xFF) + (k / VRAM_W - y0);
                    const int cx = (int)((ci->w[2] >> 16) & 0x3F) * 16, cy = (int)((ci->w[2] >> 22) & 0x1FF);
                    const uint16_t t = ci->depth == 1 ? texel8(ci->tx, ci->ty, u, v, cx, cy) : ci->depth == 0 ? texel4(ci->tx, ci->ty, u, v, cx, cy) : 0;
                    printf("  texel %04x", t);
                }
                printf("\n");
            }
        }
        for (int op = 0; op < 256; ++op)
            if (by_op[op]) printf("   op %02x: %ld (%ld small)\n", op, by_op[op], by_op_minor[op]);
        printf("frame %s: %ld pixels differ (%ld by at most 1 step, %ld by more than 4), %.3f %%\n",
               argv[i], bad, minor, large, 100.0 * (double)bad / (VRAM_W * VRAM_H));
        total += VRAM_W * VRAM_H;
        bad_total += bad;
        if (diff) {
            FILE *o = fopen(diff, "wb");
            fprintf(o, "P6 %d %d 255\n", VRAM_W, VRAM_H * 3);
            for (int pass = 0; pass < 3; ++pass)
                for (int k = 0; k < VRAM_W * VRAM_H; ++k) {
                    const uint16_t c = pass == 0 ? g_vram[k] : pass == 1 ? s_after[k] : 0;
                    uint8_t rgb[3] = { (uint8_t)((c & 31) << 3), (uint8_t)(((c >> 5) & 31) << 3), (uint8_t)(((c >> 10) & 31) << 3) };
                    if (pass == 2) { rgb[0] = g_vram[k] != s_after[k] ? 255 : 0; rgb[1] = rgb[2] = 0; }
                    fwrite(rgb, 1, 3, o);
                }
            fclose(o);
        }
    }
    printf("total: %.4f %% of the pixels differ\n", total ? 100.0 * (double)bad_total / (double)total : 0.0);
    return 0;
}
