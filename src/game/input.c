/*
 * Game input layer (main executable), rewritten from the original MIPS code.
 *
 * Data:
 *   PAD_RECORDS  0x8006AF20  two pad records of 0x10C bytes, filled by the pad poll
 *                            (func_8002A5F0, run from the VBlank callback):
 *                  +0x00 u8   status (0 = pad answering)
 *                  +0x12 u16  ?
 *                  +0x1C u16  buttons held (libpad bits, active high)
 *                  +0x20..+0x28  edge words latched for the game (pressed, released, ...)
 *                  +0x2C..+0x34  the same words accumulated since the last latch
 *                  +0xFC..+0x108 analog sticks (s32)
 *   INPUT_RECORDS 0x80060C40 two game input records of 0x18 bytes, read by the game:
 *                  +0x00 u8   status copy      +0x01 u8   frames since the pad answered (max 4)
 *                  +0x02 u16  held             +0x04 u16  latched edge word 0
 *                  +0x06 u16  latched edge word 1
 *                  +0x08 s32  8-way direction index, -1 = none (table at 0x8001720C)
 *                  +0x0C s32  direction * 4    +0x10 s32  angle = direction * 64 (512 = turn)
 *                  +0x14 u16  1 during the first 4 frames after the pad answered, else status
 *                  +0x16 u16  copy of pad +0x12
 *   PAD_BUSY     0x8006B208  1 while the records are being rebuilt (the VBlank poll skips)
 *   PAD_ENABLED  0x8006B20C
 */
#include "../a2_rewrite.h"

#define PAD_RECORDS      0x8006AF20u
#define PAD_RECORD_SIZE  0x10Cu
#define INPUT_RECORDS    0x80060C40u
#define INPUT_RECORD_SIZE 0x18u
#define DIRECTION_TABLE  0x8001720Cu   /* s32[16], indexed by held >> 12 (d-pad bits) */
#define PAD_BUSY         0x8006B208u
#define PAD_ENABLED      0x8006B20Cu

/* Costs (last argument of A2_REWRITE): guest cycles of the original without its calls,
 * measured with A2_VERIFY on the reference session; 0 = not measured yet. */

#define FN_LATCH_EDGES   0x8002AD6Cu
#define FN_CLEAR_EDGES   0x8002ADB4u
#define FN_BUILD_RECORD  0x8001BF70u

/* func_8001BF70: one game input record from one pad record. */
static int input_build_record(CPUState *cpu)
{
    const uint32_t rec = cpu->gpr[4], pad = cpu->gpr[5];
    a2_sh(rec + 0x04, a2_lhu(pad + 0x20));
    const uint32_t held = a2_lhu(pad + 0x1C);
    a2_sh(rec + 0x02, held);
    a2_sh(rec + 0x06, a2_lhu(pad + 0x24));
    const int32_t dir = (int32_t)a2_lw(DIRECTION_TABLE + ((held >> 12) << 2));
    a2_sw(rec + 0x08, (uint32_t)dir);
    a2_sb(rec + 0x00, a2_lbu(pad + 0x00));
    a2_sh(rec + 0x16, a2_lhu(pad + 0x12));
    uint32_t cycles = 37 + 11;                       /* the original's blocks on this path */
    if (dir != -1) {
        a2_sw(rec + 0x0C, (uint32_t)dir << 2);
        a2_sw(rec + 0x10, (uint32_t)dir << 6);
        cycles += 5;
    } else {
        a2_sw(rec + 0x0C, (uint32_t)dir);
        a2_sw(rec + 0x10, (uint32_t)dir);
        cycles += 2;
    }
    const uint32_t status = a2_lbu(pad + 0x00);
    a2_sh(rec + 0x14, status);
    if (status == 0) {
        const uint32_t frames = a2_lbu(rec + 0x01);
        if (frames < 4) {
            a2_sb(rec + 0x01, frames + 1);
            a2_sh(rec + 0x14, 1);
            cycles += 7 + 4;
        } else {
            a2_sh(rec + 0x14, 0);
            cycles += 7 + 2;
        }
    } else {
        a2_sb(rec + 0x01, 0);
        cycles += 2;
    }
    a2_charge(cycles);
    return 1;
}
A2_REWRITE(func_8001BF70, 0x8001BF70u, input_build_record, A2_COST_SELF, A2_PURE | A2_VOID)

/* func_8002AD6C: for both pads, latch the edge words accumulated since the last frame. */
static int pad_latch_edges(CPUState *cpu)
{
    (void)cpu;
    for (uint32_t pad = PAD_RECORDS; pad < PAD_RECORDS + 2 * PAD_RECORD_SIZE; pad += PAD_RECORD_SIZE) {
        const uint32_t a = a2_lw(pad + 0x2C), b = a2_lw(pad + 0x30), c = a2_lw(pad + 0x34);
        a2_sw(pad + 0x2C, 0);
        a2_sw(pad + 0x30, 0);
        a2_sw(pad + 0x34, 0);
        a2_sw(pad + 0x20, a);
        a2_sw(pad + 0x24, b);
        a2_sw(pad + 0x28, c);
    }
    return 1;
}
A2_REWRITE(func_8002AD6C, 0x8002AD6Cu, pad_latch_edges, 43, A2_PURE | A2_VOID)

/* func_8002ADB4: for both pads, clear the latched and accumulated edge words. */
static int pad_clear_edges(CPUState *cpu)
{
    (void)cpu;
    for (uint32_t pad = PAD_RECORDS; pad < PAD_RECORDS + 2 * PAD_RECORD_SIZE; pad += PAD_RECORD_SIZE) {
        a2_sw(pad + 0x20, 0);
        a2_sw(pad + 0x2C, 0);
        a2_sw(pad + 0x24, 0);
        a2_sw(pad + 0x30, 0);
        a2_sw(pad + 0x28, 0);
        a2_sw(pad + 0x34, 0);
    }
    return 1;
}
A2_REWRITE(func_8002ADB4, 0x8002ADB4u, pad_clear_edges, 25, A2_PURE | A2_VOID)

/* func_8001C038 / func_8001C0A0: rebuild both game input records, with the edges of this
 * frame (latch) or without (clear), while the VBlank poll is held off. */
static void input_rebuild(CPUState *cpu, uint32_t site, uint32_t edges_fn)
{
    a2_sw(PAD_BUSY, 1);
    a2_call0(cpu, site + 0x00, edges_fn);
    a2_call2(cpu, site + 0x18, FN_BUILD_RECORD, INPUT_RECORDS, PAD_RECORDS);
    a2_call2(cpu, site + 0x24, FN_BUILD_RECORD, INPUT_RECORDS + INPUT_RECORD_SIZE,
             PAD_RECORDS + PAD_RECORD_SIZE);
    a2_sw(PAD_BUSY, 0);
}

static int input_update(CPUState *cpu)
{
    input_rebuild(cpu, 0x8001C058u, FN_LATCH_EDGES);
    return 1;
}
A2_REWRITE(func_8001C038, 0x8001C038u, input_update, 34, A2_PURE | A2_VOID)

static int input_update_no_edges(CPUState *cpu)
{
    input_rebuild(cpu, 0x8001C0C0u, FN_CLEAR_EDGES);
    return 1;
}
A2_REWRITE(func_8001C0A0, 0x8001C0A0u, input_update_no_edges, 34, A2_PURE | A2_VOID)

/* func_8002A5E0: PAD_ENABLED = (arg == 0). */
static int pad_set_disabled(CPUState *cpu)
{
    a2_sw(PAD_ENABLED, cpu->gpr[4] == 0);
    return 1;
}
A2_REWRITE(func_8002A5E0, 0x8002A5E0u, pad_set_disabled, 4, A2_PURE | A2_VOID)

/* func_8001BF68: empty. */
static int input_nothing(CPUState *cpu)
{
    (void)cpu;
    return 1;
}
A2_REWRITE(func_8001BF68, 0x8001BF68u, input_nothing, 2, A2_PURE | A2_VOID)

/* ---- pad poll, run from the VBlank callback ---- */

#define PAD_RAW          0x8006AED0u   /* libpad receive buffers, 0x22 bytes per port */
#define PAD_BUTTONS      0x8006AEC0u   /* s32[3]: pad 0, pad 1, pad 0 | pad 1 << 16 */
#define PAD_COMBINED_XOR 0x8006AF14u
#define PAD_COMBINED     0x8006AF18u
#define PAD_POLL_ON      0x8005364Cu   /* set once libpad is started */
#define REPEAT_DELAY     0x80053650u   /* frames before auto-repeat (30) */
#define DOUBLE_TAP_GAP   0x80053654u   /* frames for a double tap (8) */
#define RESET_HOLD       0x80053658u   /* s32[2]: frames the reset combination is held */
#define ACT_ALIGN        0x80053660u   /* actuator alignment table for PadSetActAlign */
#define VIBRATION        0x8006B138u   /* two vibration blocks of two 0x34-byte channels */
#define RESET_COMBO      0x0900u       /* START + SELECT in the game's bit order */

#define FN_PAD_GET_STATE     0x80045A84u
#define FN_PAD_INFO_MODE     0x80045B50u
#define FN_PAD_INFO_ACT      0x80045C48u
#define FN_PAD_INFO_COMB     0x80045D1Cu
#define FN_PAD_SET_ACT       0x80045E44u
#define FN_PAD_SET_ACT_ALIGN 0x80045DC4u
#define FN_STICKS_TO_BUTTONS 0x8001C230u
#define FN_VIBRATION_ALLOWED 0x8002ADF0u
#define FN_RESET_REQUEST     0x8001991Cu
#define FN_PAD_POLL          0x8002A5F0u
#define FN_VIBRATION_UPDATE  0x8002AFB0u

/* func_8001C230(rx, ry, lx, ly): analog sticks as d-pad / face buttons, when enabled. */
static int sticks_to_buttons(CPUState *cpu)
{
    const int32_t rx = (int32_t)cpu->gpr[4], ry = (int32_t)cpu->gpr[5];
    const int32_t lx = (int32_t)cpu->gpr[6], ly = (int32_t)cpu->gpr[7];
    uint32_t bits = 0, cycles = 7 + 6 + 2;       /* the original's blocks on this path */
    if (a2_lw(0x80060C38u)) {                    /* left stick drives the d-pad */
        if (lx >= 0x40) { bits = 0x2000; cycles += 3; }
        else if (lx < -0x3F) { bits = 0x8000; cycles += 4; }
        else cycles += 2;
        if (ly >= 0x40) { bits |= 0x4000; cycles += 3; }
        else if (ly < -0x3F) { bits |= 0x1000; cycles += 4; }
        else cycles += 2;
        cycles += 3 + 2;
    } else {
        cycles += 1;
    }
    if (a2_lw(0x80060C3Cu)) {                    /* right stick drives the face buttons */
        if (rx >= 0x40) { bits |= 0x20; cycles += 3; }
        else if (rx < -0x3F) { bits |= 0x80; cycles += 4; }
        else cycles += 2;
        if (ry >= 0x40) { bits |= 0x40; cycles += 2; }
        else if (ry < -0x3F) { bits |= 0x10; cycles += 3; }
        else cycles += 2;
        cycles += 2 + 2;
    }
    cpu->gpr[2] = bits;
    a2_charge(cycles);
    return 1;
}
A2_REWRITE(func_8001C230, 0x8001C230u, sticks_to_buttons, A2_COST_SELF, A2_PURE)

/* func_8002ADF0: vibration allowed for this pad (always 2 = yes). */
static int vibration_allowed(CPUState *cpu)
{
    cpu->gpr[2] = 2;
    return 1;
}
A2_REWRITE(func_8002ADF0, 0x8002ADF0u, vibration_allowed, 2, A2_PURE)

/* func_8002A5F0: read both pads through libpad, detect them step by step, decode buttons
 * and sticks, then update held / pressed / repeat / double-tap words.
 *
 * Timing: this runs inside the VBlank interrupt every frame, so it charges exactly the
 * cycles of the original's blocks on the path it takes (spans from tools/block_costs.py,
 * block addresses in the comments), before each call and at the end. */
static uint32_t s_cyc;                                /* cycles not charged yet */
static void cyc_flush(void) { a2_charge(s_cyc); s_cyc = 0; }

static int pad_poll(CPUState *cpu)
{
    s_cyc = 21;                                       /* 8002A5F0 */
    a2_sw(PAD_BUTTONS + 8, 0);
    a2_sw(PAD_BUTTONS + 4, 0);
    a2_sw(PAD_BUTTONS + 0, 0);
    if (a2_lw(PAD_POLL_ON)) {
        s_cyc += 8;                                   /* 8002A63C */
        for (uint32_t n = 0; n < 2; ++n) {
            const uint32_t p = PAD_RECORDS + n * PAD_RECORD_SIZE, port = n << 4;
            const uint32_t buttons = PAD_BUTTONS + 4 * n;
            a2_sb(p + 0x01, port);
            s_cyc += 6;                               /* 8002A65C */
            cyc_flush();
            const uint32_t state = a2_call1(cpu, 0x8002A66Cu, FN_PAD_GET_STATE, port) & 0xFFFF;
            a2_sh(p + 0x04, state);
            s_cyc += 5;                               /* 8002A674 */
            if (state < 7) s_cyc += 9;                /* 8002A688: jump table */
            switch (state) {
            case 0:                                  /* disconnected */
                a2_sb(p + 0x0C, 0);
                a2_sh(p + 0x12, 0);
                a2_sb(p + 0x0D, 0);
                a2_sh(p + 0x14, 0);
                a2_sh(p + 0x06, 0);
                a2_sh(p + 0x08, 0x7F);
                a2_sh(p + 0x0A, 0x7F);
                a2_sb(p + 0x02, 0);
                a2_sb(p + 0x03, 0);
                a2_sh(p + 0x0E, 0);
                a2_sh(p + 0x10, 0);
                a2_sw(p + 0x108, 0);
                a2_sw(p + 0x104, 0);
                a2_sw(p + 0x100, 0);
                a2_sw(p + 0xFC, 0);
                a2_sb(p + 0x00, 0);
                s_cyc += 17;                          /* 8002A6A4 */
                break;
            case 1:                                  /* searching */
                a2_sb(p + 0x0C, 0);
                s_cyc += 2;                           /* 8002A6E8 */
                break;
            case 2:
            case 6:                                  /* found / stable: start the set-up */
                s_cyc += 6;                           /* 8002A6F0 */
                if (a2_lbu(p + 0x0C) == 0) {
                    a2_sb(p + 0x0C, 1);
                    s_cyc += 1;                       /* 8002A700 */
                }
                break;
            default:
                break;
            }
            a2_sw(buttons, 0);
            s_cyc += 8;                               /* 8002A704 */

            /* set-up sequence, one step per frame (+0x0C), 0x63 = ready */
            const uint32_t step = a2_lbu(p + 0x0C);
            if (step < 0x64) s_cyc += 9;              /* 8002A71C: jump table */
            int next = -1;
            switch (step) {
            case 1:
                s_cyc += 6;                           /* 8002A738 */
                cyc_flush();
                a2_sh(p + 0x12, a2_call3(cpu, 0x8002A740u, FN_PAD_INFO_ACT, a2_lbu(p + 1), -1, 0));
                s_cyc += 3;                           /* 8002A748 */
                next = 2;
                break;
            case 2:
                s_cyc += 6;                           /* 8002A754 */
                cyc_flush();
                a2_sh(p + 0x14, a2_call3(cpu, 0x8002A75Cu, FN_PAD_INFO_COMB, a2_lbu(p + 1), -1, 0));
                s_cyc += 3;                           /* 8002A764 */
                next = 3;
                break;
            case 3:
                s_cyc += 6;                           /* 8002A770 */
                cyc_flush();
                a2_sh(p + 0x06, a2_call3(cpu, 0x8002A778u, FN_PAD_INFO_MODE, a2_lbu(p + 1), 2, 0));
                s_cyc += 3;                           /* 8002A780 */
                next = 4;
                break;
            case 4:
                s_cyc += 6;                           /* 8002A78C */
                if (a2_lh(p + 0x12) > 0) {
                    s_cyc += 5;                       /* 8002A79C */
                    cyc_flush();
                    a2_sh(p + 0x08, a2_call3(cpu, 0x8002A7A0u, FN_PAD_INFO_ACT, a2_lbu(p + 1), 0, 3));
                    s_cyc += 2 + 2;                   /* 8002A7A8, 8002A7B4 */
                } else {
                    a2_sh(p + 0x08, 0x7F);
                    s_cyc += 1 + 2;                   /* 8002A7B0, 8002A7B4 */
                }
                next = 5;
                break;
            case 5:
                s_cyc += 7;                           /* 8002A7BC */
                if (a2_lh(p + 0x12) >= 2) {
                    s_cyc += 5;                       /* 8002A7D0 */
                    cyc_flush();
                    a2_sh(p + 0x0A, a2_call3(cpu, 0x8002A7D4u, FN_PAD_INFO_ACT, a2_lbu(p + 1), 1, 3));
                    s_cyc += 2 + 2;                   /* 8002A7DC, 8002A7E8 */
                } else {
                    a2_sh(p + 0x0A, 0x7F);
                    s_cyc += 1 + 2;                   /* 8002A7E4, 8002A7E8 */
                }
                next = 6;
                break;
            case 6:
                s_cyc += 6;                           /* 8002A7F0 */
                cyc_flush();
                a2_call3(cpu, 0x8002A7F8u, FN_PAD_SET_ACT, a2_lbu(p + 1), p + 2, 2);
                s_cyc += 2;                           /* 8002A800 */
                next = 7;
                break;
            case 7:
                s_cyc += 6;                           /* 8002A808 */
                cyc_flush();
                a2_call2(cpu, 0x8002A810u, FN_PAD_SET_ACT_ALIGN, a2_lbu(p + 1), ACT_ALIGN);
                s_cyc += 2;                           /* 8002A818 */
                next = 8;
                break;
            case 8: {
                a2_sb(p + 0x0D, 1);
                s_cyc += 7;                           /* 8002A820 */
                cyc_flush();
                const uint32_t mode = a2_call3(cpu, 0x8002A82Cu, FN_PAD_INFO_MODE, a2_lbu(p + 1), 4, 1);
                a2_sh(p + 0x16, mode);
                s_cyc += 6;                           /* 8002A834 */
                if ((int16_t)mode == 7) s_cyc += 2;   /* 8002A84C */
                next = (int16_t)mode == 7 ? 9 : 0x63;
                break;
            }
            case 9:
                s_cyc += 1;                           /* 8002A854 */
                next = 0x63;
                break;
            default:
                break;
            }
            if (next >= 0) {
                a2_sb(p + 0x0C, (uint32_t)next);
                s_cyc += 1;                           /* 8002A858 */
            }

            /* buttons and sticks from the libpad buffer */
            const uint32_t raw = PAD_RAW + n * 0x22;
            a2_sb(p + 0x00, a2_lbu(raw + 1) & 0xF0);
            s_cyc += 15;                              /* 8002A85C */
            if (a2_lbu(p + 0x0C) != 0x63) {
                s_cyc += 1 + 6;                       /* 8002AA0C, 8002AA10 */
                continue;
            }
            const uint32_t type = a2_lbu(p + 0x00);
            const uint32_t bits = ~((a2_lbu(raw + 2) << 8) | a2_lbu(raw + 3));
            s_cyc += 6;                               /* 8002A888 */
            if (type == 0x40) {                      /* digital pad: no L3 / R3 */
                a2_sw(buttons, bits & 0xF9FF);
                a2_sw(p + 0x108, 0);
                a2_sw(p + 0x104, 0);
                a2_sw(p + 0x100, 0);
                a2_sw(p + 0xFC, 0);
                s_cyc += 11 + 4;                      /* 8002A8A8, 8002A934 */
            } else if (type == 0x70) {               /* analog pad */
                a2_sw(buttons, bits & 0xFFFF);
                a2_sw(p + 0xFC, a2_lbu(raw + 4) - 0x80);
                a2_sw(p + 0x100, a2_lbu(raw + 5) - 0x80);
                const uint32_t lx = a2_lbu(raw + 6) - 0x80;
                a2_sw(p + 0x104, lx);
                const uint32_t ly = a2_lbu(raw + 7) - 0x80;
                a2_sw(p + 0x108, ly);
                s_cyc += 2 + 39;                      /* 8002A898, 8002A8C4 */
                cyc_flush();
                const uint32_t stick = a2_call4(cpu, 0x8002A918u, FN_STICKS_TO_BUTTONS,
                                                a2_lw(p + 0xFC), a2_lw(p + 0x100), lx, ly);
                a2_sw(buttons, a2_lw(buttons) | stick);
                s_cyc += 7;                           /* 8002A920 */
            } else {
                a2_sw(buttons, 0);
                a2_sw(p + 0x108, 0);
                a2_sw(p + 0x104, 0);
                a2_sw(p + 0x100, 0);
                a2_sw(p + 0xFC, 0);
                s_cyc += 2 + 2 + 4;                   /* 8002A898, 8002A8A0, 8002A934 */
            }

            /* vibration levels to send (+0x02, +0x03) */
            s_cyc += 2;                               /* 8002A944 */
            cyc_flush();
            const uint32_t allowed = a2_call1(cpu, 0x8002A944u, FN_VIBRATION_ALLOWED, n);
            s_cyc += 3;                               /* 8002A94C */
            if (allowed != 2) {
                s_cyc += 1 + 6;                       /* 8002AA0C, 8002AA10 */
                continue;
            }
            s_cyc += 6;                               /* 8002A958 */
            if (a2_lw(p + 0x18) == 0) {
                a2_sb(p + 0x03, 0);
                a2_sb(p + 0x02, 0);
                s_cyc += 2 + 1 + 6;                   /* 8002AA04, 8002AA0C, 8002AA10 */
                continue;
            }
            s_cyc += 6;                               /* 8002A968 */
            if (a2_lhu(p + 0x06) == 0) {             /* no actuator table: fixed big motor */
                a2_sb(p + 0x02, 0x40);
                a2_sb(p + 0x03, a2_lh(p + 0x0E) != 0);
                s_cyc += 7 + 1 + 6;                   /* 8002A978, 8002AA0C, 8002AA10 */
                continue;
            }
            s_cyc += 6;                               /* 8002A98C */
            if (a2_lh(p + 0x12) <= 0) {
                s_cyc += 1 + 6;                       /* 8002AA0C, 8002AA10 */
                continue;
            }
            s_cyc += 2;                               /* 8002A99C */
            for (int32_t i = 0; i < a2_lh(p + 0x12); ++i) {
                const uint32_t act = a2_lhu(p + 0x08 + 2 * i);
                const int32_t level = a2_lh(p + 0x0E + 2 * i);
                s_cyc += 6;                           /* 8002A9A4 */
                if (act == 0x7F) {
                    a2_sb(p + 0x02 + i, 0);
                    s_cyc += 1;                       /* 8002A9E0 */
                } else if (level == 0) {
                    a2_sb(p + 0x02 + i, 0);
                    s_cyc += 6 + 1;                   /* 8002A9B4, 8002A9E0 */
                } else {
                    a2_sb(p + 0x02 + i, act ? a2_lbu(p + 0x0E + 2 * i) : 1);
                    s_cyc += 6 + 2 + (act ? 5 : 2);   /* 8002A9B4, 8002A9C4, 8002A9CC / 8002A9D8 */
                }
                s_cyc += 8;                           /* 8002A9E4 */
            }
            s_cyc += 2 + 6;                           /* 8002A9FC, 8002AA10 */
        }
    }

    /* held / pressed words. Pad 0's word holds both pads (pad 1 in the high half). */
    s_cyc += 50;                                      /* 8002AA28 */
    const uint32_t both = a2_lw(PAD_BUTTONS + 0) | (a2_lw(PAD_BUTTONS + 4) << 16);
    const uint32_t p0 = PAD_RECORDS, p1 = PAD_RECORDS + PAD_RECORD_SIZE;
    a2_sw(PAD_BUTTONS + 8, both);
    const uint32_t old0 = a2_lw(p0 + 0x1C);
    a2_sw(p0 + 0x38, old0);
    a2_sw(PAD_COMBINED, both);
    const uint32_t old1 = a2_lw(p1 + 0x1C);
    a2_sw(p0 + 0x1C, both);
    const uint32_t held1 = (uint32_t)((int32_t)both >> 16);
    a2_sw(p1 + 0x38, old1);
    a2_sw(p0 + 0x2C, a2_lw(p0 + 0x2C) | (both & (both ^ old0)));
    a2_sw(p1 + 0x1C, held1);
    a2_sw(p1 + 0x2C, a2_lw(p1 + 0x2C) | (held1 & (held1 ^ old1)));
    a2_sw(PAD_COMBINED_XOR, both ^ held1);

    /* per button: auto-repeat (+0x30) and double tap (+0x34) */
    for (uint32_t n = 0; n < 2; ++n) {
        const uint32_t p = PAD_RECORDS + n * PAD_RECORD_SIZE;
        s_cyc += 4;                                   /* 8002AAC0 */
        if (n == 0) {
            s_cyc += 6;                               /* 8002AAD0 */
            if (a2_lhu(p + 0x1C) == RESET_COMBO) {
                a2_sw(RESET_HOLD, a2_lw(RESET_HOLD) + 1);
                s_cyc += 7;                           /* 8002AAE0 */
            } else {
                a2_sw(RESET_HOLD, 0);
                s_cyc += 1;                           /* 8002AAF4 */
            }
            s_cyc += 7;                               /* 8002AAF8 */
            if (a2_lw(PAD_ENABLED)) {
                a2_sw(RESET_HOLD, 0);
                s_cyc += 1;                           /* 8002AB0C */
            }
            s_cyc += 7;                               /* 8002AB10 */
            if ((int32_t)a2_lw(RESET_HOLD) >= 10) {
                s_cyc += 2;                           /* 8002AB24 */
                cyc_flush();
                a2_call0(cpu, 0x8002AB24u, FN_RESET_REQUEST);
                s_cyc += 1;                           /* 8002AB2C */
            }
        } else {
            s_cyc += 1;                               /* 8002AB2C */
        }
        s_cyc += 10;                                  /* 8002AB30 */
        const int32_t delay = (int32_t)a2_lw(REPEAT_DELAY), gap = (int32_t)a2_lw(DOUBLE_TAP_GAP);
        uint32_t tapped = 0;
        for (uint32_t i = 0; i < 16; ++i) {
            const uint32_t bit = 1u << i, b = p + 4 * i;
            s_cyc += 7;                               /* 8002AB48 */
            if (a2_lw(p + 0x1C) & bit) {
                int32_t frames = (int32_t)a2_lw(b + 0x3C) + 1;
                a2_sw(b + 0x3C, (uint32_t)frames);
                s_cyc += 8 + 6 + 7 + 7;               /* 8002AB5C, 8002AB80, 8002ABA0, 8002ABCC */
                if (frames > 0x186A3) {
                    a2_sw(b + 0x3C, 0x186A0);
                    s_cyc += 3;                       /* 8002AB74 */
                }
                if ((int32_t)a2_lw(b + 0x3C) == delay) {
                    a2_sw(p + 0x30, a2_lw(p + 0x30) | bit);
                    s_cyc += 6;                       /* 8002AB90 */
                }
                frames = (int32_t)a2_lw(b + 0x3C);
                if (frames >= delay) {
                    s_cyc += 2;                       /* 8002ABB4 */
                    if (!(frames & 3)) {
                        a2_sw(p + 0x30, a2_lw(p + 0x30) | bit);
                        s_cyc += 6;                   /* 8002ABBC */
                    }
                }
                if (!(a2_lw(p + 0x38) & bit)) {
                    a2_sw(p + 0x30, a2_lw(p + 0x30) | bit);
                    s_cyc += 7;                       /* 8002ABE0 */
                }
            } else {
                a2_sw(b + 0x3C, 0);
                s_cyc += 1;                           /* 8002ABF4 */
            }
            const int held = (a2_lw(p + 0x1C) & bit) != 0;
            const int32_t state = (int32_t)a2_lw(b + 0x7C);
            s_cyc += 6;                               /* 8002ABF8 */
            switch (state) {
            case 0:                                  /* idle: released after a press -> 1 */
                s_cyc += 2 + 2 + 7;                   /* 8002AC08, 8002AC10, 8002AC38 */
                if (a2_lw(p + 0x38) & bit) {
                    a2_sw(b + 0x7C, 1);
                    a2_sw(b + 0xBC, 0);
                    s_cyc += 2 + 1;                   /* 8002AC4C, 8002ACE0 */
                }
                break;
            case 1:                                  /* first press */
                s_cyc += 7;                           /* 8002AC54 */
                if (held) {
                    a2_sw(b + 0xBC, a2_lw(b + 0xBC) + 1);
                    s_cyc += 7;                       /* 8002AC98 */
                } else {
                    const int fast = (int32_t)a2_lw(b + 0xBC) < gap;
                    a2_sw(b + 0x7C, fast ? 2 : 0);
                    a2_sw(b + 0xBC, 0);
                    s_cyc += 7 + (fast ? 2 : 1) + 1;  /* 8002AC68, 8002AC7C / 8002ACDC, 8002ACE0 */
                }
                break;
            case 2:                                  /* released, waiting for the second press */
                s_cyc += 2 + 2 + 7;                   /* 8002AC08, 8002AC20, 8002AC84 */
                if (!held) {
                    a2_sw(b + 0xBC, a2_lw(b + 0xBC) + 1);
                    s_cyc += 7;                       /* 8002AC98 */
                } else {
                    const int fast = (int32_t)a2_lw(b + 0xBC) < gap;
                    a2_sw(b + 0x7C, fast ? 3 : 0);
                    a2_sw(b + 0xBC, 0);
                    s_cyc += 7 + (fast ? 2 : 1) + 1;  /* 8002ACAC, 8002ACC0 / 8002ACDC, 8002ACE0 */
                }
                break;
            case 3:                                  /* double tap held */
                s_cyc += 2 + 2 + 2 + 7;               /* 8002AC08, 8002AC20, 8002AC28, 8002ACC8 */
                if (!held) {
                    a2_sw(b + 0x7C, 0);
                    a2_sw(b + 0xBC, 0);
                    s_cyc += 1 + 1;                   /* 8002ACDC, 8002ACE0 */
                }
                break;
            default:
                s_cyc += state < 0 ? 2 + 2 + 2        /* 8002AC08, 8002AC10, 8002AC18 */
                                   : 2 + 2 + 2 + 2;   /* 8002AC08, 8002AC20, 8002AC28, 8002AC30 */
                break;
            }
            s_cyc += 6;                               /* 8002ACE4 */
            if (a2_lw(b + 0x7C) == 3) {
                tapped |= a2_lw(p + 0x1C) & bit;
                s_cyc += 6;                           /* 8002ACF4 */
            }
            s_cyc += 4;                               /* 8002AD04 */
        }
        a2_sw(p + 0x34, a2_lw(p + 0x34) | (a2_lw(p + 0x1C) & tapped));
        s_cyc += 14;                                  /* 8002AD14 */
    }
    s_cyc += 32;                                      /* 8002AD3C */
    cyc_flush();
    return 1;
}
A2_REWRITE(func_8002A5F0, 0x8002A5F0u, pad_poll, A2_COST_SELF, A2_PURE | A2_VOID)

/* func_8002AFB0(block): advance the two vibration channels of a block (0x34 bytes each)
 * and write the motor levels of both pads. The levels are kept in a local array on the
 * guest stack, as in the original: entries not written by this block keep what the stack
 * held, and the second block's call overwrites them. */
static int vibration_update(CPUState *cpu)
{
    const uint32_t local = cpu->gpr[29] - 0x10;     /* the original's 16-byte frame */
    uint32_t ch = cpu->gpr[4];
    for (uint32_t k = 0; k < 2; ++k, ch += 0x34) {
        const uint32_t slot = local + 4 * k + (a2_lw(ch + 0x10) << 3);
        if (!(a2_lw(ch + 0x00) & 0x8000)) {
            a2_sw(local + 4 * k + (a2_lw(ch + 0x10) << 3), 0);
            continue;
        }
        const uint32_t hold = a2_lw(ch + 0x1C);
        if (hold) a2_sw(ch + 0x1C, hold - 1);
        else a2_sw(ch + 0x14, 0);
        const int32_t steps = (int32_t)a2_lw(ch + 0x18) - 1;
        a2_sw(ch + 0x18, (uint32_t)steps);
        if (steps <= 0) {
            const int32_t repeat = (int32_t)a2_lw(ch + 0x20) - 1;
            a2_sw(ch + 0x20, (uint32_t)repeat);
            if (repeat == 0) {                      /* next step of the pattern */
                const uint32_t s = a2_lw(ch + 0x2C);
                if (a2_lbu(s + 1) == 0) {           /* end of pattern */
                    a2_sw(ch + 0x00, 0);
                    a2_sw(ch + 0x08, 0);
                    continue;
                }
                a2_sw(ch + 0x14, a2_lbu(s + 0) << 8);
                a2_sw(ch + 0x18, a2_lbu(s + 1));
                a2_sw(ch + 0x1C, a2_lbu(s + 2));
                a2_sw(ch + 0x24, (uint32_t)a2_lh(s + 4));
                a2_sw(ch + 0x30, s);
                a2_sw(ch + 0x2C, s + 6);
                a2_sw(ch + 0x20, a2_lbu(s + 3));
            } else {                                /* repeat the current step */
                const uint32_t s = a2_lw(ch + 0x30);
                a2_sw(ch + 0x14, a2_lbu(s + 0) << 8);
                a2_sw(ch + 0x18, a2_lbu(s + 1));
                a2_sw(ch + 0x1C, a2_lbu(s + 2));
                a2_sw(ch + 0x24, (uint32_t)a2_lh(s + 4));
            }
        }
        int32_t level = (int32_t)(a2_lw(ch + 0x14) + a2_lw(ch + 0x24));
        a2_sw(ch + 0x14, (uint32_t)level);
        if (level > 0xFFFF) a2_sw(ch + 0x14, 0xFF00);
        if ((int32_t)a2_lw(ch + 0x14) < 0) a2_sw(ch + 0x14, 0);
        const int32_t scaled = (int32_t)(a2_lw(ch + 0x14) * a2_lw(ch + 0x28));
        int32_t out = scaled / 25600;
        if (out < 0) out = 0;
        if (out >= 0x100) out = 0xFF;
        a2_sw(slot, (uint32_t)out);
    }
    for (uint32_t n = 0; n < 2; ++n) {
        const uint32_t p = PAD_RECORDS + n * PAD_RECORD_SIZE;
        if (a2_lw(p + 0x18) == 3) continue;
        a2_sh(p + 0x0E, a2_lhu(local + 8 * n + 0));
        a2_sh(p + 0x10, a2_lhu(local + 8 * n + 4));
    }
    return 1;
}
A2_REWRITE(func_8002AFB0, 0x8002AFB0u, vibration_update, 94, A2_PURE | A2_VOID)

/* func_8002A47C: VBlank-time pad update, skipped while the game rebuilds its records. */
static int pad_vblank(CPUState *cpu)
{
    if (a2_lw(PAD_BUSY)) return 1;
    a2_sw(PAD_BUSY, 1);
    a2_call0(cpu, 0x8002A4A0u, FN_PAD_POLL);
    a2_call1(cpu, 0x8002A4ACu, FN_VIBRATION_UPDATE, VIBRATION);
    a2_call1(cpu, 0x8002A4B4u, FN_VIBRATION_UPDATE, VIBRATION + 0x68);
    a2_sw(PAD_BUSY, 0);
    return 1;
}
A2_REWRITE(func_8002A47C, 0x8002A47Cu, pad_vblank, 30, A2_PURE | A2_VOID)

/* func_8002AE74(block, id, pad): stop a vibration block (any id when id == -1) and clear the
 * pad's motor levels. Waits for the VBlank poll in the original; if the poll holds the
 * records, the original handles that wait. */
static int vibration_stop(CPUState *cpu)
{
    if (a2_lw(PAD_BUSY)) return 0;
    const uint32_t blk = cpu->gpr[4], id = cpu->gpr[5], pad = cpu->gpr[6];
    a2_sw(PAD_BUSY, 1);
    if (id == 0xFFFFFFFFu || a2_lw(blk + 0x04) == id) {
        a2_sw(blk + 0x00, 0);
        a2_sw(blk + 0x08, 0);
        a2_sw(blk + 0x10, pad);
        a2_sw(blk + 0x04, 0xFFFFFFFFu);
        a2_sw(blk + 0x34, 0);
        a2_sw(blk + 0x3C, 0);
        a2_sw(blk + 0x44, pad);
        a2_sw(blk + 0x38, 0xFFFFFFFFu);
    }
    const uint32_t p = PAD_RECORDS + pad * PAD_RECORD_SIZE;
    if (a2_lw(p + 0x18) != 3) {
        a2_sh(p + 0x0E, 0);
        a2_sh(p + 0x10, 0);
    }
    a2_sw(PAD_BUSY, 0);
    return 1;
}
A2_REWRITE(func_8002AE74, 0x8002AE74u, vibration_stop, 39, A2_PURE | A2_VOID)

/* ---- start-up ---- */

#define FN_PAD_INIT_DIRECT   0x80047E98u
#define FN_PAD_START_COM     0x800459F8u
#define FN_BZERO             0x80031298u
#define FN_PAD_START         0x8002A4D4u
#define FN_PAD_SET_DISABLED  0x8002A5E0u
#define FN_VIBRATION_STOP    0x8002AE74u

/* func_8002A4D4: start libpad on both ports and reset the pad records. If the VBlank poll
 * holds the records, the original handles the wait. */
static int pad_start(CPUState *cpu)
{
    if (a2_lw(PAD_BUSY)) return 0;
    a2_sw(PAD_BUSY, 1);
    a2_sw(PAD_ENABLED, 1);
    a2_sw(PAD_POLL_ON, 0);
    a2_charge(7 + 11);
    a2_call2(cpu, 0x8002A51Cu, FN_PAD_INIT_DIRECT, PAD_RAW, PAD_RAW + 0x22);
    for (uint32_t p = PAD_RECORDS; p < PAD_RECORDS + 2 * PAD_RECORD_SIZE; p += PAD_RECORD_SIZE) {
        a2_sb(p + 0x0C, 0);
        a2_sw(p + 0x18, 1);
        a2_sh(p + 0x12, 0);
        a2_sh(p + 0x14, 0);
        a2_sh(p + 0x06, 0);
        a2_sh(p + 0x08, 0x7F);
        a2_sh(p + 0x0A, 0x7F);
        a2_sb(p + 0x02, 0);
        a2_sb(p + 0x03, 0);
        a2_sh(p + 0x0E, 0);
        a2_sh(p + 0x10, 0);
        a2_sb(p + 0x00, 0);
        a2_sw(p + 0xFC, 0);
        a2_sw(p + 0x100, 0);
        a2_sw(p + 0x104, 0);
        a2_sw(p + 0x108, 0);
        a2_sw(p + 0x1C, 0);
        a2_sw(p + 0x30, 0);
        a2_sw(p + 0x2C, 0);
        a2_sw(p + 0x34, 0);
        for (int i = 15; i >= 0; --i)              /* auto-repeat counters */
            a2_sw(p + 0x3C + 4u * (uint32_t)i, 0);
    }
    a2_charge(5 + 2 * (22 + 16 * 4 + 4) + 2);
    a2_call0(cpu, 0x8002A5B0u, FN_LATCH_EDGES);
    a2_charge(2);
    a2_call0(cpu, 0x8002A5B8u, FN_PAD_START_COM);
    a2_sw(PAD_POLL_ON, 1);
    a2_sw(PAD_BUSY, 0);
    a2_charge(10);
    return 1;
}
A2_REWRITE(func_8002A4D4, 0x8002A4D4u, pad_start, A2_COST_SELF, A2_VOID)

/* func_8001BEA4: input start-up: stop vibrations, start the pads, clear the game records,
 * left stick drives the d-pad, right stick does not drive the face buttons. */
static int input_start(CPUState *cpu)
{
    a2_sw(PAD_BUSY, 0);
    a2_charge(12);
    a2_call3(cpu, 0x8001BECCu, FN_VIBRATION_STOP, VIBRATION, -1, 0);
    a2_charge(4);
    a2_call3(cpu, 0x8001BEDCu, FN_VIBRATION_STOP, VIBRATION + 0x68, -1, 1);
    a2_charge(2);
    a2_call0(cpu, 0x8001BEE4u, FN_PAD_START);
    a2_charge(2);
    a2_call1(cpu, 0x8001BEECu, FN_PAD_SET_DISABLED, 0);
    const uint32_t r0 = INPUT_RECORDS, r1 = INPUT_RECORDS + INPUT_RECORD_SIZE;
    a2_charge(4);
    a2_call2(cpu, 0x8001BEFCu, FN_BZERO, r0, INPUT_RECORD_SIZE);
    a2_charge(4);
    a2_call2(cpu, 0x8001BF0Cu, FN_BZERO, r1, INPUT_RECORD_SIZE);
    a2_sh(r1 + 0x02, 0);
    a2_sh(r0 + 0x02, 0);
    a2_sh(r1 + 0x04, 0);
    a2_sh(r0 + 0x04, 0);
    a2_sw(r1 + 0x08, 0xFFFFFFFFu);
    a2_sw(r0 + 0x08, 0xFFFFFFFFu);
    a2_sw(r1 + 0x0C, 0xFFFFFFFFu);
    a2_sw(r0 + 0x0C, 0xFFFFFFFFu);
    a2_sw(r1 + 0x10, 0xFFFFFFFFu);
    a2_sw(r0 + 0x10, 0xFFFFFFFFu);
    a2_sw(0x80060C3Cu, 0);
    a2_sw(0x80060C38u, 1);
    a2_charge(27);
    return 1;
}
A2_REWRITE(func_8001BEA4, 0x8001BEA4u, input_start, A2_COST_SELF, A2_VOID)
