/*
 * io.c - the few console registers the remaining library code still touches: the DMA
 * controller (CD sectors, sound uploads), the interrupt controller and root counters.
 * Transfers happen at once; their interrupts go to the callbacks libetc keeps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cd.h"
#include "engine.h"
#include "host.h"
#include "native_modules.h"

void a2n_spu_dma(uint32_t addr, uint32_t words, int to_spu);
void a2n_mdec_dma(int channel, uint32_t addr, uint32_t words);

static uint32_t s_dma[8][3];          /* MADR, BCR, CHCR */
static uint32_t s_dpcr = 0x07654321u, s_dicr;
static uint32_t s_istat, s_imask;
static uint32_t s_memctl[16];
static uint16_t s_rcnt_mode[3], s_rcnt_target[3];
static uint64_t s_rcnt_base[3];

static uint32_t transfer_words(uint32_t bcr, uint32_t chcr)
{
    const uint32_t sync = (chcr >> 9) & 3;
    if (sync == 0) return (bcr & 0xFFFF) ? (bcr & 0xFFFF) : 0x10000;
    return (bcr & 0xFFFF) * (bcr >> 16 ? bcr >> 16 : 1);
}

static uint32_t s_dma_done;           /* channels whose callback is due */

/* DMACallback: installing a callback enables the channel's interrupt (and the master
 * enable) in the DMA interrupt register, removing it disables the channel's */
void a2n_dma_irq_enable(int ch, int on)
{
    const uint32_t bit = 1u << (16 + ch);
    s_dicr = (s_dicr & 0x7F000000u) | (((s_dicr & 0x00FFFFFFu) & ~bit) | (on ? bit : 0) | 0x00800000u);
}

/* a transfer finished: the channel is idle again; if its interrupt is enabled, the callback
 * (DMACallback) runs as an interrupt at the game's next control transfer */
void a2n_dma_complete(int ch)
{
    s_dma[ch][2] &= ~0x11000000u;
    if ((s_dicr & (1u << (16 + ch))) && (s_dicr & 0x00800000u)) {
        s_dicr |= 1u << (24 + ch);
        s_dma_done |= 1u << ch;
        a2n_event_post(A2N_EV_DMA);
    }
}

void a2n_dma_event(void)
{
    const uint32_t done = s_dma_done;
    s_dma_done = 0;
    for (int ch = 0; ch < 7; ++ch) {
        if (!(done & (1u << ch))) continue;
        s_dicr &= ~(1u << (24 + ch));               /* acknowledged by the handler */
        const uint32_t cb = a2n_dma_callback(ch);
        if (cb) a2n_call_interrupt(cb, 0, 0, 0);
    }
}

static void dma_start(int ch)
{
    const uint32_t madr = s_dma[ch][0] & 0xFFFFFCu, chcr = s_dma[ch][2];
    const uint32_t words = transfer_words(s_dma[ch][1], chcr);
    static int log = -1;
    if (log < 0) log = getenv("A2_LOG_DMA") != NULL;
    if (log) fprintf(stderr, "a2.dma: ch%d madr %06x words %u chcr %08x\n", ch, madr, words, chcr);
    if (ch == 1 || ch == 3 || (ch == 4 && !(chcr & 1))) a2n_check_write(0x80000000u | madr, words * 4, ch == 1 ? "DMA1" : ch == 3 ? "DMA3" : "DMA4");
    switch (ch) {
    case 0: case 1:                     /* the decoder completes them when it has the data */
        a2n_mdec_dma(ch, 0x80000000u | madr, words);
        return;
    case 3: a2n_cd_dma(0x80000000u | madr, words); break;
    case 4: a2n_spu_dma(0x80000000u | madr, words, (int)(chcr & 1)); break;
    default:
        fprintf(stderr, "a2: DMA channel %d not provided (chcr %08x)\n", ch, chcr);
        break;
    }
    a2n_dma_complete(ch);
}

static uint32_t dicr_value(void) { return s_dicr | ((s_dicr & 0x7F000000u) ? 0x80000000u : 0); }

static uint32_t io_read_word(uint32_t a)
{
    if (a >= 0x1F801080u && a < 0x1F801100u) {
        const uint32_t off = a - 0x1F801080u;
        if (off == 0x70) return s_dpcr;
        if (off == 0x74) return dicr_value();
        if (off < 0x70 && (off & 0xF) < 0xC) return s_dma[off >> 4][(off & 0xF) >> 2];
        return 0;
    }
    if (a == 0x1F801814u) {                         /* GPU status: always ready */
        static uint32_t field;
        field ^= 1u;
        return 0x1C000000u | (field << 31);
    }
    if (a == 0x1F801810u) return 0;
    if (a == 0x1F801070u) return s_istat;
    if (a == 0x1F801074u) return s_imask;
    if (a >= 0x1F801100u && a < 0x1F801130u) {
        const int n = (int)((a - 0x1F801100u) >> 4);
        switch (a & 0xF) {
        case 0: {
            /* counter 0: dot clock, 1: lines, 2: system clock / 8 */
            const uint64_t ns = a2n_host_now_ns() - s_rcnt_base[n];
            const uint64_t ticks = n == 2 ? ns * 33868800ull / 8 / 1000000000ull
                                 : n == 1 ? ns / 64000ull : ns * 6700000ull / 1000000000ull;
            const uint32_t target = s_rcnt_target[n] ? s_rcnt_target[n] : 0x10000u;
            return (uint32_t)((s_rcnt_mode[n] & 0x08) ? ticks % target : ticks & 0xFFFF);
        }
        case 4: return s_rcnt_mode[n];
        default: return s_rcnt_target[n];
        }
    }
    if (a >= 0x1F801000u && a < 0x1F801040u) return s_memctl[(a - 0x1F801000u) >> 2];
    if (a == 0x1F801060u) return 0x00000B88u;
    return 0;
}

/* byte and halfword reads see their lanes of the 32-bit register */
static uint32_t io_read(uint32_t a, int size)
{
    const uint32_t w = io_read_word(a & ~3u) >> ((a & 3u) * 8u);
    return size == 4 ? w : size == 2 ? (w & 0xFFFFu) : (w & 0xFFu);
}

/* the DMA interrupt register: enables and the master enable are written, flags (bits 24-30)
 * are cleared by writing 1; `mask` holds the byte lanes the store wrote */
static void dicr_write(uint32_t v, uint32_t mask)
{
    const uint32_t keep = 0x00FF803Fu & mask;
    s_dicr = (s_dicr & ~keep) | (v & keep);
    s_dicr &= ~(v & mask & 0x7F000000u);
}

static void io_write(uint32_t a, uint32_t v, int size)
{
    const uint32_t shift = (a & 3u) * 8u;
    const uint32_t mask = (size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu) << shift;
    const uint32_t wv = v << shift;
    a &= ~3u;
    if (a >= 0x1F801080u && a < 0x1F801100u) {
        const uint32_t off = a - 0x1F801080u;
        if (off == 0x70) { s_dpcr = (s_dpcr & ~mask) | (wv & mask); return; }
        if (off == 0x74) { dicr_write(wv, mask); return; }
        if (off < 0x70 && (off & 0xF) < 0xC) {
            const int ch = (int)(off >> 4), r = (int)((off & 0xF) >> 2);
            s_dma[ch][r] = (s_dma[ch][r] & ~mask) | (wv & mask);
            if (r == 2 && (s_dma[ch][2] & 0x01000000u) && (mask & 0xFF000000u)) dma_start(ch);
        }
        return;
    }
    if (a == 0x1F801810u || a == 0x1F801814u)       /* GPU ports: drawing goes through the native */
        return;                                     /* libgpu; the library's own control writes */
    if (a == 0x1F801070u) { s_istat &= ~mask | wv; return; }
    if (a == 0x1F801074u) { s_imask = ((s_imask & ~mask) | (wv & mask)) & 0x7FF; return; }
    if (a >= 0x1F801100u && a < 0x1F801130u) {
        const int n = (int)((a - 0x1F801100u) >> 4);
        switch (a & 0xF) {
        case 0: s_rcnt_base[n] = a2n_host_now_ns(); break;
        case 4: s_rcnt_mode[n] = (uint16_t)v; s_rcnt_base[n] = a2n_host_now_ns(); break;
        default: s_rcnt_target[n] = (uint16_t)v; break;
        }
        return;
    }
    if (a >= 0x1F801000u && a < 0x1F801040u) {
        uint32_t *r = &s_memctl[(a - 0x1F801000u) >> 2];
        *r = (*r & ~mask) | (wv & mask);
        return;
    }
}

void a2n_io_init(void)
{
    memset(s_dma, 0, sizeof s_dma);
    a2n_io_register(0x1F801000u, 0x1F801800u, io_read, io_write);
}
