/*
 * spu.c - the sound chip: 24 voices playing ADPCM samples from 512 KB of sound RAM, with
 * envelopes, pitch, noise, reverb and the CD audio input; written from the public
 * description of its registers and algorithms. The game's own sound driver programs it
 * directly through its registers; this module answers them and mixes 44.1 kHz stereo,
 * one vertical blank (882 samples) at a time.
 *
 * The interpolation between samples is cubic (the console's Gaussian table is not
 * reproduced): the sound is the same to the ear, not to the bit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "host.h"
#include "native_modules.h"

#define RAM_BYTES 0x80000u

static uint8_t s_ram[RAM_BYTES];
static uint16_t s_reg[0x200];                     /* 0x1F801C00..0x1F801FFF, halfwords */

enum { PH_OFF, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

typedef struct {
    uint32_t addr, repeat, start;                 /* byte addresses in sound RAM */
    int repeat_written;
    uint32_t counter;                             /* pitch counter, 12 fraction bits */
    int16_t buf[28];                              /* decoded block */
    int16_t hist[4];                              /* previous samples for interpolation */
    int pos;                                      /* sample index in buf */
    int old, older;
    int phase, level, env_counter;
    int on;
    int out;                                      /* last output, for pitch modulation */
} Voice;

static Voice s_v[24];
static uint32_t s_endx, s_transfer_addr, s_noise = 1, s_noise_count;
static uint32_t s_rev_addr;
static int16_t s_cd[2 * 44100];                   /* CD audio ring, stereo */
static uint32_t s_cd_w, s_cd_r;

static const int F0[5] = { 0, 60, 115, 98, 122 };
static const int F1[5] = { 0, 0, -52, -55, -60 };

#define REG(off) s_reg[((off) - 0x1C00u) >> 1]

static inline int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* ---- ADPCM ---- */

static void decode_block(Voice *v)
{
    const uint8_t *b = &s_ram[v->addr & (RAM_BYTES - 16)];
    int shift = b[0] & 0x0F;
    if (shift > 12) shift = 9;
    int filter = (b[0] >> 4) & 7;
    if (filter > 4) filter = 4;
    const uint8_t flags = b[1];
    if ((flags & 4) && !v->repeat_written) v->repeat = v->addr;
    for (int i = 0; i < 28; ++i) {
        int s = (int16_t)(((b[2 + i / 2] >> ((i & 1) * 4)) & 0xF) << 12);
        s >>= shift;
        s += (v->old * F0[filter] + v->older * F1[filter] + 32) >> 6;
        const int16_t o = clamp16(s);
        v->older = v->old;
        v->old = o;
        v->buf[i] = o;
    }
}

/* the block's end flags, after its samples were played */
static void next_block(Voice *v, int index)
{
    const uint8_t flags = s_ram[(v->addr + 1) & (RAM_BYTES - 1)];
    if (flags & 1) {                              /* loop end */
        s_endx |= 1u << index;
        v->addr = v->repeat;
        if (!(flags & 2)) {                       /* no repeat: the voice is released, silent */
            v->phase = PH_RELEASE;
            v->level = 0;
        }
    } else {
        v->addr = (v->addr + 16) & (RAM_BYTES - 1);
    }
    decode_block(v);
}

/* ---- envelope ---- */

static void env_step(Voice *v, int index)
{
    const uint32_t a1 = s_reg[(index * 16 + 8) >> 1], a2 = s_reg[(index * 16 + 10) >> 1];
    int exp, dec, shift, step, target = -1;
    switch (v->phase) {
    case PH_ATTACK:
        exp = (a1 >> 15) & 1; dec = 0; shift = (a1 >> 10) & 0x1F; step = 7 - (int)((a1 >> 8) & 3);
        break;
    case PH_DECAY:
        exp = 1; dec = 1; shift = (a1 >> 4) & 0xF; step = -8;
        target = (int)(((a1 & 0xF) + 1) * 0x800);
        break;
    case PH_SUSTAIN:
        exp = (a2 >> 15) & 1; dec = (a2 >> 14) & 1; shift = (a2 >> 8) & 0x1F;
        step = dec ? -8 + (int)((a2 >> 6) & 3) : 7 - (int)((a2 >> 6) & 3);
        break;
    case PH_RELEASE:
        exp = (a2 >> 5) & 1; dec = 1; shift = (a2 & 0x1F); step = -8;
        break;
    default:
        return;
    }
    int cycles = 1 << (shift - 11 > 0 ? shift - 11 : 0);
    int st = step << (11 - shift > 0 ? 11 - shift : 0);
    if (exp && !dec && v->level > 0x6000) cycles *= 4;
    if (exp && dec) st = (st * v->level) >> 15;
    if (--v->env_counter > 0) return;
    v->env_counter = cycles;
    v->level += st;
    if (v->level > 0x7FFF) v->level = 0x7FFF;
    if (v->level < 0) v->level = 0;
    if (v->phase == PH_ATTACK && v->level >= 0x7FFF) { v->phase = PH_DECAY; v->env_counter = 0; }
    else if (v->phase == PH_DECAY && v->level <= target) { v->phase = PH_SUSTAIN; v->env_counter = 0; }
    else if (v->phase == PH_RELEASE && v->level == 0) v->phase = PH_OFF;
}

static void key_on(int i)
{
    Voice *v = &s_v[i];
    v->start = (uint32_t)s_reg[(i * 16 + 6) >> 1] * 8u;
    v->addr = v->start;
    v->repeat_written = 0;
    v->old = v->older = 0;
    v->counter = 0;
    v->pos = 0;
    memset(v->hist, 0, sizeof v->hist);
    v->phase = PH_ATTACK;
    v->level = 0;
    v->env_counter = 0;
    v->on = 1;
    s_endx &= ~(1u << i);
    decode_block(v);
}

static void key_off(int i)
{
    if (s_v[i].phase != PH_OFF) { s_v[i].phase = PH_RELEASE; s_v[i].env_counter = 0; }
}

/* ---- volume registers: fixed (bit 15 = 0) or sweep (kept at its last value) ---- */

static int vol(uint16_t r)
{
    if (!(r & 0x8000)) return (int16_t)(uint16_t)(r << 1);
    return 0x7FFF;
}

/* ---- reverb (22.05 kHz, on its work area in sound RAM) ---- */

static inline int16_t rv(uint32_t off)
{
    const uint32_t base = (uint32_t)REG(0x1DA2) * 8u;
    uint32_t a = s_rev_addr + off;
    if (a >= RAM_BYTES) a = base + (a - RAM_BYTES) % (RAM_BYTES - base ? RAM_BYTES - base : 1);
    int16_t v;
    memcpy(&v, &s_ram[a & (RAM_BYTES - 2)], 2);
    return v;
}
static inline void wv(uint32_t off, int val)
{
    const uint32_t base = (uint32_t)REG(0x1DA2) * 8u;
    uint32_t a = s_rev_addr + off;
    if (a >= RAM_BYTES) a = base + (a - RAM_BYTES) % (RAM_BYTES - base ? RAM_BYTES - base : 1);
    const int16_t v = clamp16(val);
    memcpy(&s_ram[a & (RAM_BYTES - 2)], &v, 2);
}

static void reverb(int in_l, int in_r, int *out_l, int *out_r)
{
    const uint16_t *R = &REG(0x1DC0);
    #define A(i) ((uint32_t)R[i] * 8u)
    #define V(i) ((int)(int16_t)R[i])
    const uint32_t dAPF1 = A(0), dAPF2 = A(1);
    const int vIIR = V(2), vCOMB1 = V(3), vCOMB2 = V(4), vCOMB3 = V(5), vCOMB4 = V(6), vWALL = V(7);
    const int vAPF1 = V(8), vAPF2 = V(9);
    const uint32_t mLSAME = A(10), mRSAME = A(11), mLCOMB1 = A(12), mRCOMB1 = A(13);
    const uint32_t mLCOMB2 = A(14), mRCOMB2 = A(15), dLSAME = A(16), dRSAME = A(17);
    const uint32_t mLDIFF = A(18), mRDIFF = A(19), mLCOMB3 = A(20), mRCOMB3 = A(21);
    const uint32_t mLCOMB4 = A(22), mRCOMB4 = A(23), dLDIFF = A(24), dRDIFF = A(25);
    const uint32_t mLAPF1 = A(26), mRAPF1 = A(27), mLAPF2 = A(28), mRAPF2 = A(29);
    const int vLIN = V(30), vRIN = V(31);
    const int vLOUT = (int16_t)REG(0x1D84), vROUT = (int16_t)REG(0x1D86);
    const int lin = (vLIN * in_l) >> 15, rin = (vRIN * in_r) >> 15;
    if (REG(0x1DAA) & 0x80) {                                  /* reverb master enable */
        wv(mLSAME, ((lin + ((rv(dLSAME) * vWALL) >> 15) - rv(mLSAME - 2)) * vIIR >> 15) + rv(mLSAME - 2));
        wv(mRSAME, ((rin + ((rv(dRSAME) * vWALL) >> 15) - rv(mRSAME - 2)) * vIIR >> 15) + rv(mRSAME - 2));
        wv(mLDIFF, ((lin + ((rv(dRDIFF) * vWALL) >> 15) - rv(mLDIFF - 2)) * vIIR >> 15) + rv(mLDIFF - 2));
        wv(mRDIFF, ((rin + ((rv(dLDIFF) * vWALL) >> 15) - rv(mRDIFF - 2)) * vIIR >> 15) + rv(mRDIFF - 2));
    }
    int l = (vCOMB1 * rv(mLCOMB1) + vCOMB2 * rv(mLCOMB2) + vCOMB3 * rv(mLCOMB3) + vCOMB4 * rv(mLCOMB4)) >> 15;
    int r = (vCOMB1 * rv(mRCOMB1) + vCOMB2 * rv(mRCOMB2) + vCOMB3 * rv(mRCOMB3) + vCOMB4 * rv(mRCOMB4)) >> 15;
    if (REG(0x1DAA) & 0x80) {
        l -= (vAPF1 * rv(mLAPF1 - dAPF1)) >> 15; wv(mLAPF1, l); l = ((l * vAPF1) >> 15) + rv(mLAPF1 - dAPF1);
        r -= (vAPF1 * rv(mRAPF1 - dAPF1)) >> 15; wv(mRAPF1, r); r = ((r * vAPF1) >> 15) + rv(mRAPF1 - dAPF1);
        l -= (vAPF2 * rv(mLAPF2 - dAPF2)) >> 15; wv(mLAPF2, l); l = ((l * vAPF2) >> 15) + rv(mLAPF2 - dAPF2);
        r -= (vAPF2 * rv(mRAPF2 - dAPF2)) >> 15; wv(mRAPF2, r); r = ((r * vAPF2) >> 15) + rv(mRAPF2 - dAPF2);
    }
    *out_l = (clamp16(l) * vLOUT) >> 15;
    *out_r = (clamp16(r) * vROUT) >> 15;
    const uint32_t base = (uint32_t)REG(0x1DA2) * 8u;
    s_rev_addr += 2;
    if (s_rev_addr >= RAM_BYTES) s_rev_addr = base;
    if (s_rev_addr < base) s_rev_addr = base;
    #undef A
    #undef V
}

/* ---- mixing ---- */

static int cubic(int p0, int p1, int p2, int p3, int frac)
{
    const int t = frac;                                     /* 0..4095 */
    const int a = (-p0 + 3 * p1 - 3 * p2 + p3) / 2;
    const int b = (2 * p0 - 5 * p1 + 4 * p2 - p3) / 2;
    const int c = (-p0 + p2) / 2;
    return (int)((((((int64_t)a * t >> 12) + b) * t >> 12) + c) * t >> 12) + p1;
}

static void mix(int16_t *out, int frames)
{
    const uint16_t cnt = REG(0x1DAA);
    const uint32_t fm = REG(0x1D90) | ((uint32_t)REG(0x1D92) << 16);
    const uint32_t noise = REG(0x1D94) | ((uint32_t)REG(0x1D96) << 16);
    const uint32_t eon = REG(0x1D98) | ((uint32_t)REG(0x1D9A) << 16);
    static int rev_l, rev_r, rev_in_l, rev_in_r, rev_tick;
    for (int f = 0; f < frames; ++f) {
        /* noise generator */
        const int nshift = (cnt >> 10) & 0xF, nstep = ((cnt >> 8) & 3) + 4;
        s_noise_count += (uint32_t)nstep << (nshift ^ 0xF);
        while (s_noise_count >= 0x20000u) {
            s_noise_count -= 0x20000u;
            const uint32_t fb = ((s_noise >> 15) ^ (s_noise >> 12) ^ (s_noise >> 11) ^ (s_noise >> 10) ^ 1) & 1;
            s_noise = ((s_noise << 1) | fb) & 0xFFFF;
        }
        int l = 0, r = 0, el = 0, er = 0;
        for (int i = 0; i < 24; ++i) {
            Voice *v = &s_v[i];
            if (!v->on) continue;
            if (v->phase == PH_OFF) { v->out = 0; continue; }
            int sample;
            if (noise & (1u << i)) {
                sample = (int16_t)s_noise;
            } else {
                const int p = v->pos;
                const int p0 = p >= 1 ? v->buf[p - 1] : v->hist[3];
                const int p1 = v->buf[p];
                const int p2 = p + 1 < 28 ? v->buf[p + 1] : v->buf[p];
                const int p3 = p + 2 < 28 ? v->buf[p + 2] : p2;
                sample = clamp16(cubic(p0, p1, p2, p3, (int)(v->counter & 0xFFF)));
            }
            sample = (sample * v->level) >> 15;
            v->out = sample;
            const int16_t *vr = (const int16_t *)&s_reg[i * 8];
            const int vl = vol((uint16_t)vr[0]), vrr = vol((uint16_t)vr[1]);
            const int sl = (sample * vl) >> 15, sr = (sample * vrr) >> 15;
            l += sl;
            r += sr;
            if (eon & (1u << i)) { el += sl; er += sr; }
            env_step(v, i);
            /* advance */
            uint32_t pitch = s_reg[(i * 16 + 4) >> 1];
            if ((fm & (1u << i)) && i > 0) {
                const int factor = s_v[i - 1].out + 0x8000;
                pitch = (uint32_t)(((int64_t)(int16_t)pitch * factor) >> 15) & 0xFFFF;
            }
            if (pitch > 0x3FFF) pitch = 0x3FFF;
            v->counter += pitch;
            while (v->counter >= 0x1000) {
                v->counter -= 0x1000;
                if (++v->pos >= 28) {
                    for (int k = 0; k < 4; ++k) v->hist[k] = v->buf[24 + k];
                    v->pos = 0;
                    next_block(v, i);
                }
            }
        }
        /* CD audio input */
        if (s_cd_r != s_cd_w) {
            const int cl = s_cd[(s_cd_r * 2) % (2 * 44100)], cr = s_cd[(s_cd_r * 2 + 1) % (2 * 44100)];
            s_cd_r = (s_cd_r + 1) % 44100;
            if (cnt & 1) {
                const int vl = (int16_t)REG(0x1DB0), vrr = (int16_t)REG(0x1DB2);
                const int ml = (cl * vl) >> 15, mr = (cr * vrr) >> 15;
                l += ml;
                r += mr;
                if (cnt & 4) { el += ml; er += mr; }
            }
        }
        /* reverb at half rate */
        rev_in_l += el;
        rev_in_r += er;
        if (++rev_tick == 2) {
            rev_tick = 0;
            reverb(clamp16(rev_in_l / 2), clamp16(rev_in_r / 2), &rev_l, &rev_r);
            rev_in_l = rev_in_r = 0;
        }
        if (!(cnt & 0x4000)) l = r = 0;                     /* muted */
        l = (l * vol(REG(0x1D80))) >> 15;
        r = (r * vol(REG(0x1D82))) >> 15;
        if (cnt & 0x8000) { l += rev_l; r += rev_r; }
        out[f * 2] = clamp16(l);
        out[f * 2 + 1] = clamp16(r);
    }
}

void a2n_spu_vblank(void)
{
    static int16_t buf[2 * 882];
    mix(buf, 882);
    a2n_host_audio(buf, 882);
}

void a2n_spu_cd_audio(const int16_t *stereo, int frames, int rate)
{
    (void)rate;
    for (int i = 0; i < frames; ++i) {
        const uint32_t next = (s_cd_w + 1) % 44100;
        if (next == s_cd_r) break;                          /* full: drop */
        s_cd[s_cd_w * 2] = stereo[i * 2];
        s_cd[s_cd_w * 2 + 1] = stereo[i * 2 + 1];
        s_cd_w = next;
    }
}

/* ---- registers ---- */

static uint32_t spu_read(uint32_t a, int size)
{
    const uint32_t off = a & 0x3FEu;
    uint32_t v;
    if (a - 0x1F801C00u < 0x180u && (off & 0xF) == 0xC) {          /* current envelope level */
        v = (uint32_t)s_v[(off >> 4) & 0x1F].level;
    } else if (a == 0x1F801D9Cu) {
        v = s_endx & 0xFFFF;
    } else if (a == 0x1F801D9Eu) {
        v = s_endx >> 16;
    } else if (a == 0x1F801DAEu) {                                  /* SPUSTAT */
        v = REG(0x1DAA) & 0x3F;
    } else {
        v = s_reg[off >> 1];
    }
    if (size == 4) v |= spu_read(a + 2, 2) << 16;
    return v;
}

static void spu_write16(uint32_t a, uint16_t v)
{
    const uint32_t off = a & 0x3FEu;
    if (a < 0x1F801C00u || a >= 0x1F802000u) return;
    s_reg[off >> 1] = v;
    if (a - 0x1F801C00u < 0x180u && (off & 0xF) == 0xE) s_v[(off >> 4) & 0x1F].repeat = (uint32_t)v * 8u,
        s_v[(off >> 4) & 0x1F].repeat_written = 1;
    switch (a) {
    case 0x1F801D88u: for (int i = 0; i < 16; ++i) if (v & (1u << i)) key_on(i); break;
    case 0x1F801D8Au: for (int i = 0; i < 8; ++i) if (v & (1u << i)) key_on(16 + i); break;
    case 0x1F801D8Cu: for (int i = 0; i < 16; ++i) if (v & (1u << i)) key_off(i); break;
    case 0x1F801D8Eu: for (int i = 0; i < 8; ++i) if (v & (1u << i)) key_off(16 + i); break;
    case 0x1F801DA6u: s_transfer_addr = (uint32_t)v * 8u; break;
    case 0x1F801DA8u:                                               /* manual transfer fifo */
        memcpy(&s_ram[s_transfer_addr & (RAM_BYTES - 2)], &v, 2);
        s_transfer_addr = (s_transfer_addr + 2) & (RAM_BYTES - 1);
        break;
    case 0x1F801DA2u: s_rev_addr = (uint32_t)v * 8u; break;
    default: break;
    }
}

static void spu_write(uint32_t a, uint32_t v, int size)
{
    spu_write16(a, (uint16_t)v);
    if (size == 4) spu_write16(a + 2, (uint16_t)(v >> 16));
}

/* DMA channel 4 */
void a2n_spu_dma(uint32_t addr, uint32_t words, int to_spu)
{
    for (uint32_t i = 0; i < words; ++i) {
        if (to_spu) {
            const uint32_t w = a2n_r32(addr + 4 * i);
            memcpy(&s_ram[s_transfer_addr & (RAM_BYTES - 4)], &w, 4);
        } else {
            uint32_t w;
            memcpy(&w, &s_ram[s_transfer_addr & (RAM_BYTES - 4)], 4);
            a2n_w32(addr + 4 * i, w);
        }
        s_transfer_addr = (s_transfer_addr + 4) & (RAM_BYTES - 1);
    }
}

void a2n_spu_init(void)
{
    memset(s_v, 0, sizeof s_v);
    memset(s_reg, 0, sizeof s_reg);
    a2n_io_register(0x1F801C00u, 0x1F802000u, spu_read, spu_write);
}
