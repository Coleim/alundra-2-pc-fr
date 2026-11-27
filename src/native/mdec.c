/*
 * mdec.c - the motion decoder (MDEC) that turns the movies' compressed macroblocks into
 * pixels, at its registers and DMA channels 0 (in) and 1 (out); the PsyQ library libpress
 * runs unchanged on top of it.
 *
 * Written from the public description of the chip: commands (decode, quantization tables,
 * IDCT table), the run-level code stream (a DC word with the quantization scale, then
 * run/level words, 0xFE00 ending a block), zigzag order, dequantization, the 8x8 inverse
 * DCT with the table the game uploads, YCbCr to RGB, and the 4/8/15/24-bit outputs. The
 * rounding of the IDCT and color steps is the usual one, not necessarily the chip's: the
 * picture is the same to the eye, not to the bit.
 *
 * Decoding happens when the last parameter word arrives; the output waits in a buffer
 * that DMA channel 1 (or reads of the data register) empties.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "native_modules.h"

static struct {
    uint32_t cmd;                 /* command being received */
    uint32_t remaining;           /* parameter words still expected */
    uint32_t *in;                 /* parameters received */
    uint32_t n_in, cap_in;
    uint8_t qt_y[64], qt_c[64];
    int16_t scale[64];
    uint32_t *out;                /* decoded words waiting */
    uint32_t n_out, pos_out, cap_out;
    uint32_t control;             /* DMA enables (bits 30, 29) */
    /* DMA channel 1 waiting for output */
    uint32_t want_addr, want_words;
    int want;
} m;

static uint8_t s_zigzag[64];      /* sequence index -> row * 8 + column */

static void zigzag_init(void)
{
    int k = 0;
    for (int s = 0; s < 15; ++s) {
        for (int i = 0; i <= s; ++i) {
            const int a = (s & 1) ? i : s - i;    /* odd diagonals go down, even ones up */
            const int row = a, col = s - a;
            if (row < 8 && col < 8) s_zigzag[k++] = (uint8_t)(row * 8 + col);
        }
    }
}

void a2n_dma_complete(int channel);              /* io.c */

/* ---------------------------------------------------------------- output */

static void out_word(uint32_t w)
{
    if (m.n_out == m.cap_out) {
        if (m.pos_out) {                          /* drop what was already read */
            memmove(m.out, m.out + m.pos_out, sizeof *m.out * (m.n_out - m.pos_out));
            m.n_out -= m.pos_out;
            m.pos_out = 0;
        }
        if (m.n_out == m.cap_out) {
            m.cap_out = m.cap_out ? m.cap_out * 2 : 65536;
            m.out = realloc(m.out, sizeof *m.out * m.cap_out);
        }
    }
    m.out[m.n_out++] = w;
}

static void serve_dma_out(void)
{
    if (!m.want || m.n_out - m.pos_out < m.want_words) return;
    for (uint32_t i = 0; i < m.want_words; ++i) a2n_w32(m.want_addr + 4 * i, m.out[m.pos_out++]);
    m.want = 0;
    a2n_dma_complete(1);
}

/* ---------------------------------------------------------------- decoding */

typedef struct {
    const uint16_t *p, *end;
} Stream;

static inline int s10(uint16_t v) { return (int)((int16_t)(v << 6) >> 6); }

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* one 8x8 block: run-level codes to coefficients, then the inverse DCT; 0 at the end */
static int block(Stream *s, const uint8_t *qt, int out[64])
{
    int coef[64];
    memset(coef, 0, sizeof coef);
    uint16_t n;
    do {
        if (s->p >= s->end) return 0;
        n = *s->p++;
    } while (n == 0xFE00);
    const int q = n >> 10;
    int k = 0;
    int val = s10(n) * qt[0];
    for (;;) {
        if (q == 0) val = s10(n) * 2;
        val = clampi(val, -0x400, 0x3FF);
        if (q > 0) coef[s_zigzag[k]] = val;
        else coef[k] = val;
        if (s->p >= s->end) break;
        n = *s->p++;
        k += (n >> 10) + 1;
        if (k > 63) break;
        val = (s10(n) * qt[k] * q + 4) / 8;
    }
    /* inverse DCT: two passes with the uploaded table (rows: frequency, columns: position) */
    int64_t tmp[64];
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) {
            int64_t sum = 0;
            for (int z = 0; z < 8; ++z) sum += (int64_t)coef[y + z * 8] * m.scale[x + z * 8];
            tmp[x + y * 8] = (sum + 0x8000) >> 16;
        }
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) {
            int64_t sum = 0;
            for (int z = 0; z < 8; ++z) sum += tmp[y + z * 8] * m.scale[x + z * 8];
            out[x + y * 8] = (int)((sum + 0x8000) >> 16);
        }
    return 1;
}

static void decode(void)
{
    const uint32_t cmd = m.cmd;
    const int depth = (int)((cmd >> 27) & 3), is_signed = (cmd >> 26) & 1, bit15 = (cmd >> 25) & 1;
    Stream s = { (const uint16_t *)m.in, (const uint16_t *)(m.in + m.n_in) };
    if (depth >= 2) {
        int cr[64], cb[64], y[4][64];
        for (;;) {
            if (!block(&s, m.qt_c, cr) || !block(&s, m.qt_c, cb)) break;
            int ok = 1;
            for (int i = 0; i < 4 && ok; ++i) ok = block(&s, m.qt_y, y[i]);
            if (!ok) break;
            uint8_t rgb[16 * 16 * 3];
            for (int py = 0; py < 16; ++py)
                for (int px = 0; px < 16; ++px) {
                    const int yi = (py >= 8) * 2 + (px >= 8);
                    const int lum = y[yi][(px & 7) + (py & 7) * 8];
                    const int c = (px >> 1) + (py >> 1) * 8;
                    const int r = cr[c], b = cb[c];
                    /* YCbCr to RGB (JFIF factors, 1/4096 fixed point) */
                    int R = lum + ((5743 * r) >> 12);
                    int G = lum + ((-1410 * b - 2925 * r) >> 12);
                    int B = lum + ((7258 * b) >> 12);
                    R = clampi(R, -128, 127);
                    G = clampi(G, -128, 127);
                    B = clampi(B, -128, 127);
                    if (!is_signed) { R += 128; G += 128; B += 128; }
                    uint8_t *o = rgb + (py * 16 + px) * 3;
                    o[0] = (uint8_t)R;
                    o[1] = (uint8_t)G;
                    o[2] = (uint8_t)B;
                }
            if (depth == 2) {                    /* 24-bit: bytes R, G, B */
                for (int i = 0; i < 16 * 16 * 3; i += 4)
                    out_word((uint32_t)rgb[i] | (uint32_t)rgb[i + 1] << 8 | (uint32_t)rgb[i + 2] << 16 |
                             (uint32_t)rgb[i + 3] << 24);
            } else {                             /* 15-bit */
                for (int i = 0; i < 256; i += 2) {
                    uint32_t w = 0;
                    for (int h = 0; h < 2; ++h) {
                        const uint8_t *o = rgb + (i + h) * 3;
                        const uint32_t p = (uint32_t)(o[0] >> 3) | (uint32_t)(o[1] >> 3) << 5 |
                                           (uint32_t)(o[2] >> 3) << 10 | (uint32_t)bit15 << 15;
                        w |= p << (16 * h);
                    }
                    out_word(w);
                }
            }
        }
    } else {                                     /* monochrome: 8x8 blocks of luminance */
        int y[64];
        while (block(&s, m.qt_y, y)) {
            uint8_t v[64];
            for (int i = 0; i < 64; ++i) {
                const int l = clampi(y[i], -128, 127);
                v[i] = (uint8_t)(is_signed ? l : l + 128);
            }
            if (depth == 1) {
                for (int i = 0; i < 64; i += 4)
                    out_word((uint32_t)v[i] | (uint32_t)v[i + 1] << 8 | (uint32_t)v[i + 2] << 16 |
                             (uint32_t)v[i + 3] << 24);
            } else {
                for (int i = 0; i < 64; i += 8) {
                    uint32_t w = 0;
                    for (int h = 0; h < 8; ++h) w |= (uint32_t)(v[i + h] >> 4) << (4 * h);
                    out_word(w);
                }
            }
        }
    }
}

/* a command's parameters are all in */
static void execute(void)
{
    switch (m.cmd >> 29) {
    case 1:
        decode();
        break;
    case 2: {                                     /* quantization tables */
        const uint8_t *b = (const uint8_t *)m.in;
        memcpy(m.qt_y, b, 64);
        if (m.cmd & 1) memcpy(m.qt_c, b + 64, 64);
        break;
    }
    case 3:                                       /* IDCT table */
        memcpy(m.scale, m.in, 128);
        break;
    default:
        break;
    }
    m.n_in = 0;
    serve_dma_out();
}

static void param(uint32_t w)
{
    if (!m.remaining) {                           /* a new command */
        m.cmd = w;
        m.n_in = 0;
        switch (w >> 29) {
        case 1: m.remaining = w & 0xFFFF; break;
        case 2: m.remaining = (w & 1) ? 32 : 16; break;
        case 3: m.remaining = 32; break;
        default: m.remaining = 0; break;
        }
        if (!m.remaining) execute();
        return;
    }
    if (m.n_in == m.cap_in) {
        m.cap_in = m.cap_in ? m.cap_in * 2 : 65536;
        m.in = realloc(m.in, sizeof *m.in * m.cap_in);
    }
    m.in[m.n_in++] = w;
    if (!--m.remaining) execute();
}

/* ---------------------------------------------------------------- registers and DMA */

static uint32_t mdec_read(uint32_t a, int size)
{
    (void)size;
    if ((a & 7) == 0) return m.pos_out < m.n_out ? m.out[m.pos_out++] : 0;
    const uint32_t empty = m.pos_out >= m.n_out;
    const uint32_t cmd = m.cmd;
    return (empty << 31) | (m.remaining ? 1u << 29 : 0) | (m.control & (1u << 30) ? 1u << 28 : 0) |
           (m.control & (1u << 29) && !empty ? 1u << 27 : 0) | (((cmd >> 25) & 0xF) << 23) | (4u << 16) |
           ((m.remaining - 1) & 0xFFFF);
}

static void mdec_write(uint32_t a, uint32_t v, int size)
{
    (void)size;
    if ((a & 7) == 0) { param(v); return; }
    if (v & 0x80000000u) {                        /* reset */
        m.remaining = 0;
        m.n_in = 0;
        m.n_out = m.pos_out = 0;
        m.cmd = 0;
        m.want = 0;
    }
    m.control = v & 0x60000000u;
}

void a2n_mdec_dma(int channel, uint32_t addr, uint32_t words)
{
    if (channel == 0) {
        for (uint32_t i = 0; i < words; ++i) param(a2n_r32(addr + 4 * i));
        a2n_dma_complete(0);
        return;
    }
    m.want_addr = addr;
    m.want_words = words;
    m.want = 1;
    serve_dma_out();
}

void a2n_mdec_init(void)
{
    zigzag_init();
    free(m.in);
    free(m.out);
    memset(&m, 0, sizeof m);
    a2n_io_register(0x1F801820u, 0x1F801828u, mdec_read, mdec_write);
}
