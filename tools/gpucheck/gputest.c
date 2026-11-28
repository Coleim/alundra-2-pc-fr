/*
 * gputest - checks the software GPU's polygon coverage: a jittered grid of triangles (and of
 * quads), each drawn additively with a 1-step red, must cover every pixel inside the grid
 * exactly once (no gap, no double cover along shared edges).
 *
 *   cc -O2 -Isrc/native tools/gpucheck/gputest.c src/native/gpu.c -o build-native/gputest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"

#define N 12            /* grid cells per side */
#define CELL 17

static int px[N + 1][N + 1], py[N + 1][N + 1];
static int s_gx[1024], s_gy[512];

static uint32_t xy(int x, int y) { return (uint32_t)(x & 0x7FF) | ((uint32_t)(y & 0x7FF) << 16); }

static int check(const char *what, int x0, int y0, int x1, int y1)
{
    int gaps = 0, doubles = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const int r = g_vram[y * VRAM_W + x] & 31;
            if (r == 0) { gaps++; s_gx[x]++; s_gy[y]++; }
            if (r > 1) doubles++;
        }
    printf("%-28s gaps %5d  doubles %5d\n", what, gaps, doubles);
    if (gaps) {
        printf("   gap rows:");
        for (int y = 0; y < 512; ++y) if (s_gy[y]) printf(" %d(%d)", y, s_gy[y]);
        printf("\n   gap columns:");
        for (int x = 0; x < 1024; ++x) if (s_gx[x]) printf(" %d(%d)", x, s_gx[x]);
        printf("\n");
    }
    memset(s_gx, 0, sizeof s_gx);
    memset(s_gy, 0, sizeof s_gy);
    return gaps + doubles;
}

static void setup(void)
{
    gpu_reset();
    memset(g_vram, 0, sizeof g_vram);
    const uint32_t e1 = 0xE1000000u | (1u << 5);       /* semi-transparency mode 1: B + F */
    gpu_command(&e1, 1);
}

int main(void)
{
    srand(1);
    for (int j = 0; j <= N; ++j)
        for (int i = 0; i <= N; ++i) {
            const int edge = i == 0 || j == 0 || i == N || j == N;
            px[j][i] = 20 + i * CELL + (edge ? 0 : rand() % 11 - 5);
            py[j][i] = 20 + j * CELL + (edge ? 0 : rand() % 11 - 5);
        }
    const int x0 = 20, y0 = 20, x1 = 20 + N * CELL, y1 = 20 + N * CELL;
    int bad = 0;

    /* triangles in both windings, flat (0x22: semi-transparent flat triangle) */
    for (int variant = 0; variant < 3; ++variant) {
        setup();
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) {
                int a[2] = { px[j][i], py[j][i] }, b[2] = { px[j][i + 1], py[j][i + 1] };
                int c[2] = { px[j + 1][i], py[j + 1][i] }, d[2] = { px[j + 1][i + 1], py[j + 1][i + 1] };
                const int flip = variant == 0 ? 0 : variant == 1 ? 1 : rand() & 1;
                uint32_t t1[4] = { 0x22000008u, xy(a[0], a[1]), xy(flip ? c[0] : b[0], flip ? c[1] : b[1]),
                                   xy(flip ? b[0] : c[0], flip ? b[1] : c[1]) };
                uint32_t t2[4] = { 0x22000008u, xy(b[0], b[1]), xy(flip ? d[0] : c[0], flip ? d[1] : c[1]),
                                   xy(flip ? c[0] : d[0], flip ? c[1] : d[1]) };
                gpu_command(t1, 4);
                gpu_command(t2, 4);
            }
        char what[64];
        snprintf(what, sizeof what, "triangles (winding %s)", variant == 0 ? "A" : variant == 1 ? "B" : "mixed");
        bad += check(what, x0, y0, x1, y1);
    }

    /* quads (0x2A: semi-transparent flat quad), vertices in the GPU's order a b c d */
    setup();
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            uint32_t q[5] = { 0x2A000008u, xy(px[j][i], py[j][i]), xy(px[j][i + 1], py[j][i + 1]),
                              xy(px[j + 1][i], py[j + 1][i]), xy(px[j + 1][i + 1], py[j + 1][i + 1]) };
            gpu_command(q, 5);
        }
    bad += check("quads", x0, y0, x1, y1);

    /* Gouraud textured quads take the same coverage path: 0x3E with a raw texture of 1-step red */
    setup();
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) g_vram[(256 + y) * VRAM_W + 512 + x] = 0x8001;   /* 15-bit page at 512,256 */
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            const uint32_t tpage = 8u | (1u << 4) | (1u << 5) | (2u << 7);              /* x 512, y 256, B+F, 15-bit */
            uint32_t q[12] = { 0x3E808080u, xy(px[j][i], py[j][i]), 0x00000000u,
                               0x00808080u, xy(px[j][i + 1], py[j][i + 1]), (tpage << 16) | 0x0010u,
                               0x00808080u, xy(px[j + 1][i], py[j + 1][i]), 0x00001000u,
                               0x00808080u, xy(px[j + 1][i + 1], py[j + 1][i + 1]), 0x00001010u };
            gpu_command(q, 12);
        }
    {
        /* the texture page lives in VRAM: count only the grid area */
        bad += check("gouraud textured quads", x0, y0, x1, y1);
    }
    printf("%s\n", bad ? "FAILED" : "all right");
    return bad ? 1 : 0;
}
