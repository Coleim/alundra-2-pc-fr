/*
 * gte.c - the PlayStation's geometry coprocessor (COP2), in software.
 *
 * Written from the public description of the GTE (register map, command set, fixed-point
 * formats, saturation flags, the reciprocal used by perspective division). The results
 * must be the bit-exact ones of the console: the game's 3D depends on them, and the
 * verification compares them, command by command, with the original's.
 *
 * Storage: data[] and ctrl[] are the 64 registers. Registers the game reaches without a
 * helper (see GTE_DATA_WRITE_HELPER in tools/a2lift.py) hold their raw 32-bit value; the
 * others go through a2gte_read_* / a2gte_write_* and are kept in the form they read back
 * (16-bit registers sign- or zero-extended).
 */
#include "gte.h"

#define FLAG_ERROR_MASK 0x7F87E000u

typedef struct {
    uint32_t *d, *c;
    uint32_t flag;
    int sf, lm;
} Gte;

static uint8_t s_unr[0x101];
static int s_unr_ready;

static void unr_init(void)
{
    for (int i = 0; i <= 0x100; ++i) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        s_unr[i] = (uint8_t)(v < 0 ? 0 : v);
    }
    s_unr_ready = 1;
}

static inline int16_t lo16(uint32_t v) { return (int16_t)(v & 0xFFFFu); }
static inline int16_t hi16(uint32_t v) { return (int16_t)(v >> 16); }

/* ---- register reads and writes, as the CPU sees them ---- */

static uint32_t orgb(const uint32_t *d)
{
    uint32_t out = 0;
    for (int i = 0; i < 3; ++i) {
        int32_t v = (int32_t)(int16_t)d[9 + i] >> 7;
        if (v < 0) v = 0;
        if (v > 0x1F) v = 0x1F;
        out |= (uint32_t)v << (5 * i);
    }
    return out;
}

static uint32_t lzcr(uint32_t v)
{
    uint32_t x = (v & 0x80000000u) ? ~v : v;
    uint32_t n = 0;
    while (n < 32 && !(x & 0x80000000u)) { x <<= 1; ++n; }
    return n;
}

uint32_t a2gte_read_data(const uint32_t *d, int reg)
{
    switch (reg) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11:
        return (uint32_t)(int32_t)(int16_t)d[reg];
    case 7: case 16: case 17: case 18: case 19:
        return d[reg] & 0xFFFFu;
    case 15:
        return d[14];
    case 28: case 29:
        return orgb(d);
    default:
        return d[reg];
    }
}

void a2gte_write_data(uint32_t *d, int reg, uint32_t v)
{
    switch (reg) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11:
        d[reg] = (uint32_t)(int32_t)(int16_t)v;
        break;
    case 7: case 16: case 17: case 18: case 19:
        d[reg] = v & 0xFFFFu;
        break;
    case 15:                                       /* SXYP: push the screen XY fifo */
        d[12] = d[13];
        d[13] = d[14];
        d[14] = v;
        break;
    case 28:                                       /* IRGB: 5-bit color into IR1-3 */
        d[28] = v & 0x7FFFu;
        d[9] = (v & 0x1Fu) << 7;
        d[10] = ((v >> 5) & 0x1Fu) << 7;
        d[11] = ((v >> 10) & 0x1Fu) << 7;
        break;
    case 29:                                       /* ORGB, read only */
    case 31:                                       /* LZCR, read only */
        break;
    case 30:
        d[30] = v;
        d[31] = lzcr(v);
        break;
    default:
        d[reg] = v;
        break;
    }
}

uint32_t a2gte_read_ctrl(const uint32_t *c, int reg)
{
    switch (reg) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30:
        return (uint32_t)(int32_t)(int16_t)c[reg];   /* H (26) reads sign-extended, too */
    case 31: {
        const uint32_t f = c[31] & 0x7FFFF000u;
        return f | ((f & FLAG_ERROR_MASK) ? 0x80000000u : 0u);
    }
    default:
        return c[reg];
    }
}

void a2gte_write_ctrl(uint32_t *c, int reg, uint32_t v)
{
    switch (reg) {
    case 4: case 12: case 20: case 27: case 29: case 30:
        c[reg] = (uint32_t)(int32_t)(int16_t)v;
        break;
    case 26:
        c[reg] = v & 0xFFFFu;
        break;
    case 31:
        c[reg] = v & 0x7FFFF000u;
        break;
    default:
        c[reg] = v;
        break;
    }
}

/* ---- arithmetic with the GTE's saturation and flags ---- */

/* MAC1-3: 44-bit sums; overflow sets a flag and wraps */
static inline int64_t mac_check(Gte *g, int n, int64_t v)
{
    if (v > 0x7FFFFFFFFFFLL) g->flag |= 1u << (31 - n);
    else if (v < -0x80000000000LL) g->flag |= 1u << (28 - n);
    return (int64_t)((uint64_t)v << 20) >> 20;
}

static inline int32_t mac0_check(Gte *g, int64_t v)
{
    if (v > 0x7FFFFFFFLL) g->flag |= 1u << 16;
    else if (v < -0x80000000LL) g->flag |= 1u << 15;
    return (int32_t)v;
}

/* IR1-3 from MAC1-3 */
static inline int16_t sat_ir(Gte *g, int n, int32_t v, int lm)
{
    const int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { g->flag |= 1u << (25 - n); return (int16_t)lo; }
    if (v > 0x7FFF) { g->flag |= 1u << (25 - n); return 0x7FFF; }
    return (int16_t)v;
}

static inline uint32_t sat_color(Gte *g, int n, int32_t v)
{
    if (v < 0) { g->flag |= 1u << (22 - n); return 0; }
    if (v > 0xFF) { g->flag |= 1u << (22 - n); return 0xFF; }
    return (uint32_t)v;
}

static inline uint32_t sat_z(Gte *g, int64_t v)
{
    if (v < 0) { g->flag |= 1u << 18; return 0; }
    if (v > 0xFFFF) { g->flag |= 1u << 18; return 0xFFFF; }
    return (uint32_t)v;
}

static inline int32_t sat_xy(Gte *g, int bit, int64_t v)
{
    if (v < -0x400) { g->flag |= 1u << bit; return -0x400; }
    if (v > 0x3FF) { g->flag |= 1u << bit; return 0x3FF; }
    return (int32_t)v;
}

static inline int16_t sat_ir0(Gte *g, int64_t v)
{
    if (v < 0) { g->flag |= 1u << 12; return 0; }
    if (v > 0x1000) { g->flag |= 1u << 12; return 0x1000; }
    return (int16_t)v;
}

#define IR(n)   ((int32_t)(int16_t)g->d[8 + (n)])
#define MAC(n)  ((int32_t)g->d[24 + (n)])

static inline void set_mac(Gte *g, int n, int64_t v) { g->d[24 + n] = (uint32_t)(int32_t)(v >> (g->sf * 12)); }
static inline void set_ir(Gte *g, int n, int32_t v, int lm) { g->d[8 + n] = (uint32_t)(int32_t)sat_ir(g, n, v, lm); }

/* MAC1-3 = (T << 12 + M * V) >> sf, IR1-3 saturated */
static void mat_vec(Gte *g, const int16_t m[3][3], const int16_t v[3], const int32_t t[3], int lm)
{
    for (int i = 0; i < 3; ++i) {
        int64_t s = mac_check(g, i + 1, ((int64_t)t[i] << 12) + (int64_t)m[i][0] * v[0]);
        s = mac_check(g, i + 1, s + (int64_t)m[i][1] * v[1]);
        s = mac_check(g, i + 1, s + (int64_t)m[i][2] * v[2]);
        set_mac(g, i + 1, s);
    }
    for (int i = 0; i < 3; ++i) set_ir(g, i + 1, MAC(i + 1), lm);
}

static void push_color(Gte *g)
{
    const uint32_t code = g->d[6] & 0xFF000000u;
    g->d[20] = g->d[21];
    g->d[21] = g->d[22];
    g->d[22] = sat_color(g, 1, MAC(1) >> 4) | (sat_color(g, 2, MAC(2) >> 4) << 8) |
               (sat_color(g, 3, MAC(3) >> 4) << 16) | code;
}

static void load_matrix(const uint32_t *c, int base, int16_t m[3][3])
{
    m[0][0] = lo16(c[base + 0]); m[0][1] = hi16(c[base + 0]);
    m[0][2] = lo16(c[base + 1]); m[1][0] = hi16(c[base + 1]);
    m[1][1] = lo16(c[base + 2]); m[1][2] = hi16(c[base + 2]);
    m[2][0] = lo16(c[base + 3]); m[2][1] = hi16(c[base + 3]);
    m[2][2] = lo16(c[base + 4]);
}

static void load_vector(const Gte *g, int n, int16_t v[3])
{
    if (n == 3) { v[0] = (int16_t)IR(1); v[1] = (int16_t)IR(2); v[2] = (int16_t)IR(3); return; }
    v[0] = lo16(g->d[n * 2]);
    v[1] = hi16(g->d[n * 2]);
    v[2] = lo16(g->d[n * 2 + 1]);
}

/* perspective division: H / SZ3 with the console's reciprocal approximation */
static uint32_t divide(Gte *g, uint32_t h, uint32_t z)
{
    if (h >= z * 2) {
        g->flag |= 1u << 17;
        return 0x1FFFF;
    }
    int shift = 0;
    while (shift < 16 && !(z & (0x8000u >> shift))) ++shift;
    const uint32_t n = h << shift;
    uint32_t d = z << shift;
    const uint32_t u = s_unr[(d - 0x7FC0u) >> 7] + 0x101u;
    d = (0x2000080u - d * u) >> 8;
    d = (0x0000080u + d * u) >> 8;
    const uint64_t r = ((uint64_t)n * d + 0x8000u) >> 16;
    return r > 0x1FFFF ? 0x1FFFF : (uint32_t)r;
}

/* RTPS / RTPT on one vertex; depth cue only on the last one */
static void rtp(Gte *g, int n, int last)
{
    int16_t r[3][3], v[3];
    load_matrix(g->c, 0, r);
    load_vector(g, n, v);
    const int32_t tr[3] = { (int32_t)g->c[5], (int32_t)g->c[6], (int32_t)g->c[7] };
    int64_t raw[3];
    for (int i = 0; i < 3; ++i) {
        int64_t s = mac_check(g, i + 1, ((int64_t)tr[i] << 12) + (int64_t)r[i][0] * v[0]);
        s = mac_check(g, i + 1, s + (int64_t)r[i][1] * v[1]);
        s = mac_check(g, i + 1, s + (int64_t)r[i][2] * v[2]);
        raw[i] = s;
        set_mac(g, i + 1, s);
    }
    set_ir(g, 1, MAC(1), g->lm);
    set_ir(g, 2, MAC(2), g->lm);
    /* IR3: saturated from MAC3, but its flag follows MAC3 >> 12 (whatever sf is) */
    {
        const int32_t m3 = MAC(3);
        const int32_t lo = g->lm ? 0 : -0x8000;
        g->d[11] = (uint32_t)(int32_t)(m3 < lo ? lo : m3 > 0x7FFF ? 0x7FFF : m3);
        const int64_t z = raw[2] >> 12;
        if (z < -0x8000 || z > 0x7FFF) g->flag |= 1u << 22;
    }
    /* screen Z */
    g->d[16] = g->d[17];
    g->d[17] = g->d[18];
    g->d[18] = g->d[19];
    g->d[19] = sat_z(g, raw[2] >> 12);
    const uint32_t q = divide(g, g->c[26] & 0xFFFFu, g->d[19]);
    const int64_t sx = (int64_t)q * IR(1) + (int64_t)(int32_t)g->c[24];
    const int64_t sy = (int64_t)q * IR(2) + (int64_t)(int32_t)g->c[25];
    mac0_check(g, sx);
    mac0_check(g, sy);
    g->d[24] = (uint32_t)(int32_t)sy;
    const int32_t x = sat_xy(g, 14, sx >> 16), y = sat_xy(g, 13, sy >> 16);
    g->d[12] = g->d[13];
    g->d[13] = g->d[14];
    g->d[14] = ((uint32_t)x & 0xFFFFu) | ((uint32_t)y << 16);
    if (last) {
        const int64_t dq = (int64_t)q * (int16_t)g->c[27] + (int64_t)(int32_t)g->c[28];
        g->d[24] = (uint32_t)mac0_check(g, dq);
        g->d[8] = (uint32_t)(int32_t)sat_ir0(g, dq >> 12);
    }
}

/* MAC1-3 = color << 4 ... interpolated toward the far color by IR0 */
static void interpolate(Gte *g, int64_t m1, int64_t m2, int64_t m3)
{
    const int64_t m[3] = { m1, m2, m3 };
    for (int i = 0; i < 3; ++i) {
        const int64_t fc = (int64_t)(int32_t)g->c[21 + i] << 12;
        set_mac(g, i + 1, mac_check(g, i + 1, fc - m[i]));
    }
    for (int i = 0; i < 3; ++i) set_ir(g, i + 1, MAC(i + 1), 0);
    for (int i = 0; i < 3; ++i)
        set_mac(g, i + 1, mac_check(g, i + 1, (int64_t)IR(i + 1) * IR(0) + m[i]));
    for (int i = 0; i < 3; ++i) set_ir(g, i + 1, MAC(i + 1), g->lm);
}

static void light(Gte *g, int n)
{
    int16_t llm[3][3], lcm[3][3], v[3];
    load_matrix(g->c, 8, llm);
    load_matrix(g->c, 16, lcm);
    load_vector(g, n, v);
    const int32_t zero[3] = { 0, 0, 0 };
    mat_vec(g, llm, v, zero, g->lm);
    const int16_t ir[3] = { (int16_t)IR(1), (int16_t)IR(2), (int16_t)IR(3) };
    const int32_t bk[3] = { (int32_t)g->c[13], (int32_t)g->c[14], (int32_t)g->c[15] };
    mat_vec(g, lcm, ir, bk, g->lm);
}

static void color_back(Gte *g)          /* (BK << 12 + LCM * IR) >> sf */
{
    int16_t lcm[3][3];
    load_matrix(g->c, 16, lcm);
    const int16_t ir[3] = { (int16_t)IR(1), (int16_t)IR(2), (int16_t)IR(3) };
    const int32_t bk[3] = { (int32_t)g->c[13], (int32_t)g->c[14], (int32_t)g->c[15] };
    mat_vec(g, lcm, ir, bk, g->lm);
}

static void rgb_times_ir(Gte *g, int64_t m[3])   /* [R*IR1, G*IR2, B*IR3] << 4 */
{
    const uint32_t c = g->d[6];
    for (int i = 0; i < 3; ++i)
        m[i] = ((int64_t)((c >> (8 * i)) & 0xFF) * IR(i + 1)) << 4;
}

static void finish_color(Gte *g, const int64_t m[3])   /* >> sf, push, IR = MAC */
{
    for (int i = 0; i < 3; ++i) set_mac(g, i + 1, mac_check(g, i + 1, m[i]));
    push_color(g);
    for (int i = 0; i < 3; ++i) set_ir(g, i + 1, MAC(i + 1), g->lm);
}

static void nc_s(Gte *g, int n)
{
    light(g, n);
    push_color(g);
}

static void ncc_s(Gte *g, int n)
{
    light(g, n);
    int64_t m[3];
    rgb_times_ir(g, m);
    finish_color(g, m);
}

static void ncd_s(Gte *g, int n)
{
    light(g, n);
    int64_t m[3];
    rgb_times_ir(g, m);
    interpolate(g, m[0], m[1], m[2]);
    push_color(g);
}

static void dpc_s(Gte *g, uint32_t rgb)
{
    interpolate(g, (int64_t)(rgb & 0xFF) << 16, (int64_t)((rgb >> 8) & 0xFF) << 16,
                (int64_t)((rgb >> 16) & 0xFF) << 16);
    push_color(g);
}

void a2gte_execute(uint32_t *data, uint32_t *ctrl, uint32_t cmd)
{
    if (!s_unr_ready) unr_init();
    Gte gs = { data, ctrl, 0, (cmd >> 19) & 1, (cmd >> 10) & 1 };
    Gte *g = &gs;
    switch (cmd & 0x3F) {
    case 0x01:                                   /* RTPS */
        rtp(g, 0, 1);
        break;
    case 0x30:                                   /* RTPT */
        rtp(g, 0, 0);
        rtp(g, 1, 0);
        rtp(g, 2, 1);
        break;
    case 0x06: {                                 /* NCLIP */
        const int64_t x0 = lo16(g->d[12]), y0 = hi16(g->d[12]);
        const int64_t x1 = lo16(g->d[13]), y1 = hi16(g->d[13]);
        const int64_t x2 = lo16(g->d[14]), y2 = hi16(g->d[14]);
        g->d[24] = (uint32_t)mac0_check(g, x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1);
        break;
    }
    case 0x0C: {                                 /* OP: outer product with the diagonal */
        const int64_t d1 = lo16(g->c[0]), d2 = lo16(g->c[2]), d3 = lo16(g->c[4]);
        const int64_t i1 = IR(1), i2 = IR(2), i3 = IR(3);
        set_mac(g, 1, mac_check(g, 1, d2 * i3 - d3 * i2));
        set_mac(g, 2, mac_check(g, 2, d3 * i1 - d1 * i3));
        set_mac(g, 3, mac_check(g, 3, d1 * i2 - d2 * i1));
        for (int i = 1; i <= 3; ++i) set_ir(g, i, MAC(i), g->lm);
        break;
    }
    case 0x10:                                   /* DPCS */
        dpc_s(g, g->d[6]);
        break;
    case 0x2A:                                   /* DPCT */
        for (int k = 0; k < 3; ++k) dpc_s(g, g->d[20]);
        break;
    case 0x11:                                   /* INTPL */
        interpolate(g, (int64_t)IR(1) << 12, (int64_t)IR(2) << 12, (int64_t)IR(3) << 12);
        push_color(g);
        break;
    case 0x12: {                                 /* MVMVA */
        const int mx = (cmd >> 17) & 3, vn = (cmd >> 15) & 3, cv = (cmd >> 13) & 3;
        int16_t m[3][3], v[3];
        if (mx == 3) {                           /* no such matrix: what the hardware reads */
            const int16_t r13 = hi16(g->c[1]) , r22 = lo16(g->c[2]);
            m[0][0] = (int16_t)-((int16_t)((g->d[6] & 0xFF) << 4)); m[0][1] = (int16_t)((g->d[6] & 0xFF) << 4);
            m[0][2] = (int16_t)IR(0);
            m[1][0] = m[1][1] = m[1][2] = lo16(g->c[1]);
            m[2][0] = m[2][1] = m[2][2] = r22;
            (void)r13;
        } else {
            load_matrix(g->c, mx * 8, m);
        }
        load_vector(g, vn, v);
        int32_t t[3] = { 0, 0, 0 };
        if (cv == 0) { t[0] = (int32_t)g->c[5]; t[1] = (int32_t)g->c[6]; t[2] = (int32_t)g->c[7]; }
        else if (cv == 1) { t[0] = (int32_t)g->c[13]; t[1] = (int32_t)g->c[14]; t[2] = (int32_t)g->c[15]; }
        if (cv == 2) {
            /* far color: only flags come from FC + first column; the result leaves them out */
            for (int i = 0; i < 3; ++i) {
                const int64_t fc = (int64_t)(int32_t)g->c[21 + i] << 12;
                const int64_t f = mac_check(g, i + 1, fc + (int64_t)m[i][0] * v[0]);
                sat_ir(g, i + 1, (int32_t)(f >> (g->sf * 12)), 0);
                int64_t s = mac_check(g, i + 1, (int64_t)m[i][1] * v[1]);
                s = mac_check(g, i + 1, s + (int64_t)m[i][2] * v[2]);
                set_mac(g, i + 1, s);
            }
            for (int i = 1; i <= 3; ++i) set_ir(g, i, MAC(i), g->lm);
        } else {
            mat_vec(g, m, v, t, g->lm);
        }
        break;
    }
    case 0x13:                                   /* NCDS */
        ncd_s(g, 0);
        break;
    case 0x16:                                   /* NCDT */
        ncd_s(g, 0); ncd_s(g, 1); ncd_s(g, 2);
        break;
    case 0x14: {                                 /* CDP */
        color_back(g);
        int64_t m[3];
        rgb_times_ir(g, m);
        interpolate(g, m[0], m[1], m[2]);
        push_color(g);
        break;
    }
    case 0x1B:                                   /* NCCS */
        ncc_s(g, 0);
        break;
    case 0x3F:                                   /* NCCT */
        ncc_s(g, 0); ncc_s(g, 1); ncc_s(g, 2);
        break;
    case 0x1C: {                                 /* CC */
        color_back(g);
        int64_t m[3];
        rgb_times_ir(g, m);
        finish_color(g, m);
        break;
    }
    case 0x1E:                                   /* NCS */
        nc_s(g, 0);
        break;
    case 0x20:                                   /* NCT */
        nc_s(g, 0); nc_s(g, 1); nc_s(g, 2);
        break;
    case 0x28:                                   /* SQR */
        for (int i = 1; i <= 3; ++i) set_mac(g, i, mac_check(g, i, (int64_t)IR(i) * IR(i)));
        for (int i = 1; i <= 3; ++i) set_ir(g, i, MAC(i), g->lm);
        break;
    case 0x29: {                                 /* DCPL */
        int64_t m[3];
        rgb_times_ir(g, m);
        interpolate(g, m[0], m[1], m[2]);
        push_color(g);
        break;
    }
    case 0x2D: {                                 /* AVSZ3 */
        const int64_t s = (int64_t)(int16_t)g->c[29] *
                          ((int64_t)(g->d[17] & 0xFFFF) + (g->d[18] & 0xFFFF) + (g->d[19] & 0xFFFF));
        g->d[24] = (uint32_t)mac0_check(g, s);
        g->d[7] = sat_z(g, s >> 12);
        break;
    }
    case 0x2E: {                                 /* AVSZ4 */
        const int64_t s = (int64_t)(int16_t)g->c[30] *
                          ((int64_t)(g->d[16] & 0xFFFF) + (g->d[17] & 0xFFFF) + (g->d[18] & 0xFFFF) + (g->d[19] & 0xFFFF));
        g->d[24] = (uint32_t)mac0_check(g, s);
        g->d[7] = sat_z(g, s >> 12);
        break;
    }
    case 0x3D: {                                 /* GPF */
        for (int i = 1; i <= 3; ++i) set_mac(g, i, mac_check(g, i, (int64_t)IR(i) * IR(0)));
        push_color(g);
        for (int i = 1; i <= 3; ++i) set_ir(g, i, MAC(i), g->lm);
        break;
    }
    case 0x3E: {                                 /* GPL */
        for (int i = 1; i <= 3; ++i) {
            const int64_t base = (int64_t)MAC(i) << (g->sf * 12);
            set_mac(g, i, mac_check(g, i, base + (int64_t)IR(i) * IR(0)));
        }
        push_color(g);
        for (int i = 1; i <= 3; ++i) set_ir(g, i, MAC(i), g->lm);
        break;
    }
    default:
        break;
    }
    ctrl[31] = g->flag & 0x7FFFF000u;
}
