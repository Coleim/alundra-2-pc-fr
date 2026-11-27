/*
 * xa.c - XA-ADPCM audio sectors (streamed music and voices) decoded for the sound chip.
 *
 * An audio sector holds 18 groups of 128 bytes; each group has 8 units of 28 four-bit
 * samples (or 4 units of eight-bit samples) with a shift and a prediction filter per unit.
 * Stereo sectors alternate left and right units. The output (37.8 or 18.9 kHz) is
 * resampled to the sound chip's 44.1 kHz and mixed in as its CD input (spu.c).
 */
#include <string.h>

#include "native_modules.h"

static const int K0[4] = { 0, 60, 115, 98 };
static const int K1[4] = { 0, 0, -52, -55 };

static int s_old[2], s_older[2];
static double s_phase;
static int16_t s_hist[2][4];                    /* last input samples, per channel */

void a2n_xa_reset(void)
{
    memset(s_old, 0, sizeof s_old);
    memset(s_older, 0, sizeof s_older);
    memset(s_hist, 0, sizeof s_hist);
    s_phase = 0;
}

static inline int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static void decode_unit(const uint8_t *group, int unit, int eight_bit, int ch, int16_t *out)
{
    const uint8_t param = group[4 + unit];
    int shift = param & 0x0F;
    if (shift > 12) shift = 9;
    const int filter = (param >> 4) & 3;
    for (int j = 0; j < 28; ++j) {
        int s;
        if (eight_bit) s = (int16_t)(group[16 + j * 4 + unit] << 8);
        else s = (int16_t)(((group[16 + j * 4 + (unit >> 1)] >> ((unit & 1) * 4)) & 0xF) << 12);
        s >>= shift;
        s += (s_old[ch] * K0[filter] + s_older[ch] * K1[filter] + 32) >> 6;
        const int16_t v = clamp16(s);
        s_older[ch] = s_old[ch];
        s_old[ch] = v;
        out[j] = v;
    }
}

/* cubic interpolation between p1 and p2 at t in [0, 1) */
static inline int cubic(int p0, int p1, int p2, int p3, double t)
{
    const double a = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
    const double b = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
    const double c = -0.5 * p0 + 0.5 * p2;
    return (int)(((a * t + b) * t + c) * t + p1);
}

void a2n_xa_sector(const uint8_t *raw, int ll, int lr, int rr, int rl)
{
    const uint8_t coding = raw[19];
    const int stereo = coding & 1, half_rate = (coding >> 2) & 1, eight_bit = (coding >> 4) & 1;
    const int units = eight_bit ? 4 : 8;
    static int16_t in[2][4032];
    int n[2] = { 0, 0 };
    const uint8_t *data = raw + 24;
    for (int g = 0; g < 18; ++g) {
        const uint8_t *group = data + g * 128;
        for (int u = 0; u < units; ++u) {
            const int ch = stereo ? (u & 1) : 0;
            decode_unit(group, u, eight_bit, ch, in[ch] + n[ch]);
            n[ch] += 28;
        }
    }
    if (!stereo) { memcpy(in[1], in[0], sizeof(int16_t) * (size_t)n[0]); n[1] = n[0]; }

    /* resample to 44.1 kHz */
    const double rate = half_rate ? 18900.0 : 37800.0;
    const double step = rate / 44100.0;
    static int16_t out[2 * 4096 * 3];
    int frames = 0;
    const int count = n[0];
    while (frames < 4096 * 3) {
        const int i = (int)s_phase;
        if (i >= count) break;
        const double t = s_phase - i;
        int v[2];
        for (int ch = 0; ch < 2; ++ch) {
            const int16_t *h = s_hist[ch];
            const int p0 = i - 1 >= 0 ? in[ch][i - 1] : h[3];
            const int p1 = in[ch][i];
            const int p2 = i + 1 < count ? in[ch][i + 1] : in[ch][i];
            const int p3 = i + 2 < count ? in[ch][i + 2] : p2;
            v[ch] = cubic(p0, p1, p2, p3, t);
        }
        /* CD volume matrix (0x80 = 100 %) */
        out[frames * 2] = clamp16((v[0] * ll + v[1] * rl) >> 7);
        out[frames * 2 + 1] = clamp16((v[1] * rr + v[0] * lr) >> 7);
        frames++;
        s_phase += step;
    }
    s_phase -= count;
    for (int ch = 0; ch < 2; ++ch)
        for (int k = 0; k < 4; ++k) s_hist[ch][k] = count - 4 + k >= 0 ? in[ch][count - 4 + k] : 0;
    a2n_spu_cd_audio(out, frames, 44100);
}
