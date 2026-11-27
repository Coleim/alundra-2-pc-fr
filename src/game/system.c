/*
 * System layer of the main executable: VBlank callback and loading indicator.
 *
 * Timing: these functions run inside the VBlank interrupt, every frame. They charge the
 * cycles of the original's blocks on the path they take (spans from tools/block_costs.py),
 * at the same points relative to their calls: libetc's VSync wait loop gives up after a
 * fixed number of turns, and in this timing model it ends a few turns short of that limit,
 * so a cheaper interrupt handler is enough to make it time out (see docs/VERIFICATION.md).
 */
#include "../a2_rewrite.h"

#define LOADING_ACTIVE   0x8005D230u   /* s32: 0 = off, counts up to 9 while loading */
#define LOADING_DELAY    0x8005D17Cu   /* s32: frames until the next indicator frame */
#define LOADING_FRAME    0x8005D234u   /* s32: indicator frame 0..9, loops on 4..9 */
#define LOADING_TICKS    0x8005D228u   /* s32: frames shown so far */
#define LOADING_DISPENV  0x8005D188u   /* DISPENV of the indicator */
#define LOADING_FRAMES   0x80052254u   /* {s16 x, s16 y}[10]: VRAM position of each frame */

#define FN_PAD_VBLANK    0x8002A47Cu
#define FN_SOUND_VBLANK  0x8002CA40u
#define FN_VBLANK_22B4C  0x80022B4Cu
#define FN_LOADING       0x8001A028u
#define FN_SET_DEF_DISPENV 0x800366C8u
#define FN_PUT_DISPENV   0x80034184u

/* func_8001A028: while a load runs, show the loading indicator: every one or two frames
 * the display switches to the next 128x32 picture of the animation (VRAM column 0-0x60,
 * rows 0 / 0x80), drawn at screen position (0x74, 0xD4). */
static int loading_indicator(CPUState *cpu)
{
    const int32_t active = (int32_t)a2_lw(LOADING_ACTIVE);
    if (!active) {
        a2_charge(11 + 17);
        return 1;
    }
    a2_charge(11 + 4);
    if (active - 1 < 8) {
        a2_sw(LOADING_ACTIVE, (uint32_t)(active + 1));
        a2_charge(1);
    }
    const int32_t delay = (int32_t)a2_lw(LOADING_DELAY);
    if (delay != 0) {
        a2_sw(LOADING_DELAY, (uint32_t)(delay - 1));
        a2_charge(7 + 1 + 17);
        return 1;
    }
    a2_charge(7 + 24);
    const uint32_t frame = a2_lw(LOADING_FRAME);
    const uint32_t pos = LOADING_FRAMES + frame * 4;
    a2_call(cpu, 0x8001A0B4u, FN_SET_DEF_DISPENV, 5, LOADING_DISPENV, (uint32_t)a2_lh(pos),
            (uint32_t)a2_lh(pos + 2), 0x80u, 0x20u);
    a2_sh(LOADING_DISPENV + 0x08, 0x74);              /* screen.x */
    a2_sh(LOADING_DISPENV + 0x0A, 0xD4);              /* screen.y */
    a2_sh(LOADING_DISPENV + 0x0C, 0x80);              /* screen.w */
    a2_sh(LOADING_DISPENV + 0x0E, 0x20);              /* screen.h */
    a2_charge(10);
    a2_call1(cpu, 0x8001A0DCu, FN_PUT_DISPENV, LOADING_DISPENV);
    const uint32_t next = a2_lw(LOADING_FRAME) + 1;
    a2_sw(LOADING_FRAME, (int32_t)next >= 10 ? 4u : next);
    const uint32_t ticks = a2_lw(LOADING_TICKS) + 1;
    a2_sw(LOADING_TICKS, ticks);
    a2_sw(LOADING_DELAY, (ticks & 0x10) ? 1u : 2u);
    a2_charge(9 + ((int32_t)next >= 10 ? 2 : 0) + 9 + ((ticks & 0x10) ? 0 : 1) + 1 + 17);
    return 1;
}
A2_REWRITE(func_8001A028, 0x8001A028u, loading_indicator, A2_COST_SELF, A2_VOID)

/* func_8001A148: VBlank callback (installed by main with VSyncCallback). */
static int vblank_callback(CPUState *cpu)
{
    a2_charge(4);
    a2_call0(cpu, 0x8001A150u, FN_PAD_VBLANK);
    a2_charge(2);
    a2_call0(cpu, 0x8001A158u, FN_SOUND_VBLANK);
    a2_charge(2);
    a2_call0(cpu, 0x8001A160u, FN_VBLANK_22B4C);
    a2_charge(2);
    a2_call0(cpu, 0x8001A168u, FN_LOADING);
    a2_charge(6);
    return 1;
}
A2_REWRITE(func_8001A148, 0x8001A148u, vblank_callback, A2_COST_SELF, A2_VOID)

#define PLAY_TIME        0x80065910u   /* +4 hours, +6 minutes, +8 frames (s16), +0xA stopped */

/* func_80022B4C: play time, one tick per VBlank: 3000 frames a minute (PAL), stops at
 * 99:59. */
static int play_time_tick(CPUState *cpu)
{
    (void)cpu;
    if (a2_lh(PLAY_TIME + 0x0A) != 0) {
        a2_charge(8 + 2);
        return 1;
    }
    const uint32_t frames = a2_lhu(PLAY_TIME + 0x08);
    a2_sh(PLAY_TIME + 0x08, frames + 1);
    if ((int16_t)(frames + 1) < 3000) {
        a2_charge(8 + 11 + 2);
        return 1;
    }
    a2_sh(PLAY_TIME + 0x08, frames - 2999);
    const uint32_t minutes = a2_lhu(PLAY_TIME + 0x06) + 1;
    a2_sh(PLAY_TIME + 0x06, minutes);
    if ((int16_t)minutes < 60) {
        a2_charge(8 + 11 + 11 + 2);
        return 1;
    }
    a2_sh(PLAY_TIME + 0x06, 0);
    const uint32_t hours = a2_lhu(PLAY_TIME + 0x04) + 1;
    a2_sh(PLAY_TIME + 0x04, hours);
    if ((int16_t)hours < 100) {
        a2_charge(8 + 11 + 11 + 11 + 2);
        return 1;
    }
    a2_sh(PLAY_TIME + 0x04, 99);
    a2_sh(PLAY_TIME + 0x06, 59);
    a2_sh(PLAY_TIME + 0x0A, 0xFFFF);
    a2_charge(8 + 11 + 11 + 11 + 5 + 2);
    return 1;
}
A2_REWRITE(func_80022B4C, 0x80022B4Cu, play_time_tick, A2_COST_SELF, A2_PURE | A2_VOID)
