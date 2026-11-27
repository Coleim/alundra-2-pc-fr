/*
 * Frame pacing and the frame's display lists (GAME00), rewritten from the original MIPS code.
 *
 * The game builds a frame while the GPU draws the previous one: two display buffers (VRAM
 * y = 0 and y = 256), two sets of ordering tables and two packet buffers swap at the end of
 * every frame. The field game asks for one frame every two vertical blanks (VSync(2): 25
 * frames a second in PAL), see func_80089AE8 (the GAME00 main loop) and GAME_VSYNC_MODE.
 *
 * Data:
 *   DISPLAY 0x800FA898  two buffers of 0x70 bytes: DRAWENV (0x5C) then DISPENV (0x14)
 *             buffer 0 draws at VRAM y = 0 and shows y = 256, buffer 1 the opposite
 *     +0xE0  buffer to show next          +0xE4  the other one
 *     +0xE8  frames shown                 +0xEC  frames shown & 1
 *   OTS     0x801179A0  two sets of ordering tables of 0x20A0 bytes, then the set pointers:
 *     +0x4140 set drawn this frame (its layers)   +0x4144 the set being built
 *     +0x4148 set drawn this frame (overlay list) +0x414C the other one
 *     each set: +0x00 overlay list (9 entries), then the layers, drawn in this order:
 *       +0x2098 back (2)   +0x94 depth-sorted 3D (0x801)   +0x88 (3)   +0x44 (17)   +0x38 (3)
 *       +0x24 front (5, left out while HIDE_FRONT_LAYER is set)
 *   PACKETS 0x80112988  packet buffers: +0x5000 / +0x5004 this frame's and the other's,
 *     +0x5008 / +0x500C a second pair, +0x5010 write pointer, +0x5014 limit
 *
 * Timing: overlay code ran in the interpreter; these functions charge the spans of the
 * original's blocks on their path (tools/block_costs.py --module game00) through the
 * interpreter estimate a2_charge_interp, absorbed by the VSync wait of every frame.
 */
#include "../a2_rewrite.h"

#define DISPLAY          0x800FA898u
#define DISPLAY_BUF_SIZE 0x70u
#define OTS              0x801179A0u
#define OT_SET_SIZE      0x20A0u
#define PACKETS          0x80112988u
#define SCREEN_ADJUST    0x80066150u   /* options: s16 x, s16 y (D_80065910 + 0x840) */
#define HIDE_FRONT_LAYER 0x800FA83Cu
#define FRAME_RNG        0x8012BDE0u   /* u16, stepped once a frame */
#define VSYNC_LINES_DRAW 0x800FA7F4u   /* VSync(1) after the frame was built (debug meter) */
#define VSYNC_LINES_GPU  0x800FA7F8u   /* VSync(1) after the GPU finished */
#define CLUT_UPLOADS     0x8012FD08u   /* bits: palettes to send to VRAM at the end of the frame */
#define DEBUG_OFF        0x8005D21Cu   /* nonzero: no debug features (frame step, perf meter) */
#define FRAME_FLAG_11BAF0 0x8011BAF0u
#define PAD2_HELD        0x80060C5Au   /* game input record of pad 2: held, pressed */
#define PAD2_PRESSED     0x80060C5Cu
#define BTN_R2           0x0002u
#define BTN_R1           0x0008u

#define FN_SET_DEF_DRAWENV 0x80036608u
#define FN_SET_DEF_DISPENV 0x800366C8u
#define FN_PUT_DISPENV     0x80034184u
#define FN_PUT_DRAWENV     0x80033FB8u
#define FN_CLEAR_OTAG_R    0x80033E40u
#define FN_DRAW_OTAG       0x80033F48u
#define FN_DRAW_SYNC       0x80033954u
#define FN_LOAD_IMAGE      0x80033C00u
#define FN_VSYNC           0x8003B1C8u
#define FN_FATAL           0x8001BC68u
#define FN_INPUT_POLL      0x8001BBF0u
#define FN_INPUT_UPDATE    0x8001C038u
#define FN_CLOSE_LISTS     0x8001D87Cu
#define FN_CLOSE_LISTS_ENV 0x8001DEE0u
#define FN_STREAM_STEP     0x800899CCu
#define FN_SWAP_DISPLAY    0x8008A330u
#define FN_CLEAR_OTS       0x8008A660u
#define FN_DRAW_OTS        0x8008A774u
#define PACKET_OVERFLOW_MSG 0x80085104u

/* func_8008A228: set up the two display buffers (320x232 drawn, 256x232 of the 320 shown,
 * moved by the screen position option). */
static int display_setup(CPUState *cpu)
{
    const uint32_t buf0 = DISPLAY, buf1 = DISPLAY + DISPLAY_BUF_SIZE;
    a2_charge_interp(15);
    a2_call(cpu, 0x8008A25Cu, FN_SET_DEF_DRAWENV, 5, buf0, 0, 0, 320, 232);
    a2_charge_interp(7);
    a2_call(cpu, 0x8008A278u, FN_SET_DEF_DRAWENV, 5, buf1, 0, 256, 320, 232);
    a2_charge_interp(7);
    a2_call(cpu, 0x8008A294u, FN_SET_DEF_DISPENV, 5, buf0 + 0x5C, 0, 256, 320, 256);
    a2_charge_interp(6);
    a2_call(cpu, 0x8008A2ACu, FN_SET_DEF_DISPENV, 5, buf1 + 0x5C, 0, 0, 320, 256);
    for (uint32_t disp = buf0 + 0x5C; disp <= buf1 + 0x5C; disp += DISPLAY_BUF_SIZE) {
        a2_sh(disp + 0x0C, 256);                       /* screen.w */
        a2_sh(disp + 0x0E, 232);                       /* screen.h */
    }
    const uint32_t adjust_x = a2_lhu(SCREEN_ADJUST), adjust_y = a2_lhu(SCREEN_ADJUST + 2);
    a2_sb(buf1 + 0x18, 1);                             /* isbg: clear the buffer first */
    a2_sb(buf0 + 0x18, 1);
    a2_sb(buf1 + 0x17, 1);                             /* dfe: draw to the shown area too */
    a2_sb(buf0 + 0x17, 1);
    a2_sw(DISPLAY + 0xE0, buf0);
    a2_sw(DISPLAY + 0xE4, buf1);
    a2_sw(DISPLAY + 0xE8, 0);
    a2_sw(DISPLAY + 0xEC, 0);
    for (uint32_t disp = buf0 + 0x5C; disp <= buf1 + 0x5C; disp += DISPLAY_BUF_SIZE) {
        a2_sh(disp + 0x08, adjust_x);                  /* screen.x */
        a2_sh(disp + 0x0A, adjust_y + 12);             /* screen.y */
    }
    a2_charge_interp(45);
    return 1;
}
A2_REWRITE_OVERLAY(func_8008A228, 0x8008A228u, display_setup, A2_COST_SELF, A2_VOID,
                   0x27BDFFD0u, 0xAFB00018u, 0x3C108010u, 0x2610A898u)

/* func_8008A330: show the buffer drawn last, draw into the other one. */
static int swap_display(CPUState *cpu)
{
    const uint32_t shown = a2_lw(DISPLAY + 0xE0);
    a2_charge_interp(11);
    a2_call1(cpu, 0x8008A34Cu, FN_PUT_DISPENV, shown + 0x5C);
    a2_charge_interp(2);
    a2_call1(cpu, 0x8008A354u, FN_PUT_DRAWENV, shown);
    const uint32_t other = a2_lw(DISPLAY + 0xE4);
    const uint32_t frames = a2_lw(DISPLAY + 0xE8) + 1;
    a2_sw(DISPLAY + 0xE4, shown);
    a2_sw(DISPLAY + 0xE8, frames);
    a2_sw(DISPLAY + 0xE0, other);
    a2_sw(DISPLAY + 0xEC, frames & 1);
    a2_charge_interp(23);
    return 1;
}
A2_REWRITE_OVERLAY(func_8008A330, 0x8008A330u, swap_display, A2_COST_SELF, A2_VOID,
                   0x27BDFFE0u, 0xAFB00010u, 0x3C108010u, 0x2610A898u)

/* Make `entry` (the first entry of a reverse-cleared table) continue with `next`: the
 * tables of a set are chained, so one DrawOTag draws every layer. */
static void ot_chain(uint32_t entry, uint32_t next)
{
    a2_sw(entry, (a2_lw(entry) & 0xFF000000u) | (next & 0x00FFFFFFu));
}

/* func_8008A660: clear a set of ordering tables and chain its layers back to front; clear
 * the overlay list `overlay` (9 entries). */
static int clear_ots(CPUState *cpu)
{
    const uint32_t set = cpu->gpr[4], overlay = cpu->gpr[5];
    static const struct { uint32_t at, entries, site; } layer[] = {
        { 0x2098, 2, 0x8008A67Cu }, { 0x0094, 0x801, 0x8008A688u }, { 0x0088, 3, 0x8008A694u },
        { 0x0044, 17, 0x8008A6A0u }, { 0x0038, 3, 0x8008A6ACu }, { 0x0024, 5, 0x8008A6B8u },
    };
    a2_charge_interp(9);
    for (unsigned i = 0; i < sizeof layer / sizeof layer[0]; ++i) {
        if (i) a2_charge_interp(3);
        a2_call2(cpu, layer[i].site, FN_CLEAR_OTAG_R, set + layer[i].at, layer[i].entries);
    }
    a2_charge_interp(3);
    a2_call2(cpu, 0x8008A6C4u, FN_CLEAR_OTAG_R, overlay, 9);
    ot_chain(set + 0x2098, set + 0x2094);          /* back -> last of the 3D table */
    ot_chain(set + 0x0094, set + 0x0090);          /* 3D -> layer 0x88 */
    ot_chain(set + 0x0088, set + 0x0084);          /* -> layer 0x44 */
    ot_chain(set + 0x0044, set + 0x0040);          /* -> layer 0x38 */
    a2_charge_interp(42 + 11);
    if (!a2_lw(HIDE_FRONT_LAYER)) {
        ot_chain(set + 0x0038, set + 0x0034);      /* -> front layer 0x24 */
        a2_charge_interp(7);
    }
    return 1;
}
A2_REWRITE_OVERLAY(func_8008A660, 0x8008A660u, clear_ots, A2_COST_SELF, A2_VOID,
                   0x27BDFFE0u, 0xAFB10014u, 0x00808821u, 0xAFB00010u)

/* func_8008A60C: both sets of ordering tables, empty. */
static int init_ots(CPUState *cpu)
{
    const uint32_t set0 = OTS, set1 = OTS + OT_SET_SIZE;
    a2_sw(OTS + 0x4148, set0);
    a2_sw(OTS + 0x4140, set0);
    a2_sw(OTS + 0x414C, set1);
    a2_sw(OTS + 0x4144, set1);
    a2_charge_interp(13);
    a2_call2(cpu, 0x8008A638u, FN_CLEAR_OTS, set0, set0);
    a2_charge_interp(8);
    a2_call2(cpu, 0x8008A648u, FN_CLEAR_OTS, a2_lw(OTS + 0x4144), a2_lw(OTS + 0x414C));
    a2_charge_interp(8);
    return 1;
}
A2_REWRITE_OVERLAY(func_8008A60C, 0x8008A60Cu, init_ots, A2_COST_SELF, A2_VOID,
                   0x27BDFFE8u, 0xAFB00010u, 0x3C108011u, 0x261079A0u)

/* func_8008A774: send the frame just built to the GPU (its layers, then its overlay list),
 * then swap the sets of ordering tables and the packet buffers. The next frame's packets
 * start 0xC000 bytes into its buffer, the limit is at 0xE7CE. */
static int draw_ots(CPUState *cpu)
{
    const uint32_t drawn = a2_lw(OTS + 0x4140);
    a2_charge_interp(11);
    a2_call1(cpu, 0x8008A790u, FN_DRAW_OTAG, drawn + 0x209C);
    a2_charge_interp(5);
    a2_call1(cpu, 0x8008A79Cu, FN_DRAW_OTAG, a2_lw(OTS + 0x4148) + 0x20);

    const uint32_t packets_drawn = a2_lw(PACKETS + 0x5000);
    const uint32_t building = a2_lw(OTS + 0x4144);
    a2_sw(OTS + 0x4144, drawn);
    const uint32_t overlay_drawn = a2_lw(OTS + 0x4148);
    const uint32_t packets_next = a2_lw(PACKETS + 0x5004);
    const uint32_t pair_b = a2_lw(PACKETS + 0x500C);
    a2_sw(PACKETS + 0x5004, packets_drawn);
    const uint32_t pair_a = a2_lw(PACKETS + 0x5008);
    const uint32_t overlay_building = a2_lw(OTS + 0x414C);
    a2_sw(OTS + 0x4140, building);
    a2_sw(PACKETS + 0x5000, packets_next);
    a2_sw(OTS + 0x414C, overlay_drawn);
    a2_sw(OTS + 0x4148, overlay_building);
    a2_sw(PACKETS + 0x5010, packets_next + 0xC000);
    a2_sw(PACKETS + 0x5008, pair_b);
    a2_sw(PACKETS + 0x5014, packets_next + 0xE7CE);
    a2_sw(PACKETS + 0x500C, pair_a);
    a2_charge_interp(51);
    return 1;
}
A2_REWRITE_OVERLAY(func_8008A774, 0x8008A774u, draw_ots, A2_COST_SELF, A2_VOID,
                   0x27BDFFE0u, 0xAFB00010u, 0x3C108011u, 0x261079A0u)

/* Send a 102x1 palette to VRAM (0x310, y) and wait for the transfer. The RECT lives on the
 * guest stack, where the original keeps it. */
static void upload_clut(CPUState *cpu, uint32_t rect, uint32_t y, uint32_t data, uint32_t site)
{
    a2_sh(rect + 0, 0x310);
    a2_sh(rect + 2, y);
    a2_sh(rect + 4, 0x66);
    a2_sh(rect + 6, 1);
    a2_charge_interp(11);
    a2_call2(cpu, site, FN_LOAD_IMAGE, rect, data);
    a2_charge_interp(2);
    a2_call1(cpu, site + 8, FN_DRAW_SYNC, 0);
}

/* func_8008A390(vsync_mode): end of a frame. Close the frame's packet lists, run the CD
 * stream, step the frame's random number, wait for the GPU, send pending palettes, wait for
 * the vertical blank (mode 1 counts as 0: next blank), show the frame and start drawing it.
 * Debug: on pad 2, holding R1 freezes the game, R2 advances one frame. */
static int frame_end(CPUState *cpu)
{
    const uint32_t mode_arg = cpu->gpr[4];
    const uint32_t frame_sp = cpu->gpr[29] - 0x28u;     /* the original's stack frame */

    a2_charge_interp(21);
    uint32_t packets = a2_call2(cpu, 0x8008A3CCu, FN_CLOSE_LISTS, a2_lw(PACKETS + 0x5010),
                                a2_lw(OTS + 0x4148) + 0x10);
    a2_sw(PACKETS + 0x5010, packets);
    const uint32_t parity = a2_lw(DISPLAY + 0xEC);
    a2_charge_interp(21);
    packets = a2_call4(cpu, 0x8008A410u, FN_CLOSE_LISTS_ENV, packets, a2_lw(OTS + 0x4148) + 0x10,
                       DISPLAY + parity * DISPLAY_BUF_SIZE, DISPLAY + (parity ^ 1) * DISPLAY_BUF_SIZE);
    a2_sw(PACKETS + 0x5010, packets);
    a2_charge_interp(2);
    a2_call0(cpu, 0x8008A418u, FN_STREAM_STEP);
    a2_charge_interp(10);
    if (a2_lw(PACKETS + 0x5010) >= a2_lw(PACKETS + 0x5014)) {
        a2_charge_interp(3);
        a2_call1(cpu, 0x8008A43Cu, FN_FATAL, PACKET_OVERFLOW_MSG);
        a2_charge_interp(1);
    }

    a2_sh(FRAME_RNG, a2_lhu(FRAME_RNG) * 154811u);
    a2_charge_interp(16);
    a2_sw(VSYNC_LINES_DRAW, a2_call1(cpu, 0x8008A478u, FN_VSYNC, 1));
    a2_charge_interp(4);
    a2_call1(cpu, 0x8008A488u, FN_DRAW_SYNC, 0);

    a2_charge_interp(7);
    if (a2_lw(CLUT_UPLOADS)) {
        cpu->gpr[29] = frame_sp;
        a2_charge_interp(2);
        if (a2_lw(CLUT_UPLOADS) & 1) upload_clut(cpu, frame_sp + 0x10, 0x1F9, 0x8012FD10u, 0x8008A4D0u);
        a2_charge_interp(7);
        if (a2_lw(CLUT_UPLOADS) & 2) upload_clut(cpu, frame_sp + 0x10, 0x1FA, 0x8012FBA8u, 0x8008A518u);
        a2_sw(CLUT_UPLOADS, 0);
        a2_charge_interp(1);
        cpu->gpr[29] = frame_sp + 0x28u;
    }

    a2_charge_interp(2);
    a2_sw(VSYNC_LINES_GPU, a2_call1(cpu, 0x8008A52Cu, FN_VSYNC, 1));
    const uint32_t mode = mode_arg == 1 ? 0 : mode_arg;
    a2_charge_interp(5 + (mode_arg == 1) + 2);
    a2_call1(cpu, 0x8008A54Cu, FN_VSYNC, mode);

    a2_sw(FRAME_FLAG_11BAF0, 0);
    a2_charge_interp(7);
    if (!a2_lw(DEBUG_OFF)) {
        a2_charge_interp(9);
        if (a2_lhu(PAD2_HELD) & BTN_R1) {
            do {                                       /* frozen until R2 or R1 released */
                a2_charge_interp(2);
                a2_call1(cpu, 0x8008A584u, FN_VSYNC, mode);
                a2_charge_interp(2);
                a2_call0(cpu, 0x8008A58Cu, FN_INPUT_POLL);
                a2_charge_interp(2);
                a2_call0(cpu, 0x8008A594u, FN_INPUT_UPDATE);
                a2_charge_interp(7);
                if (a2_lhu(PAD2_PRESSED) & BTN_R2) break;
                a2_charge_interp(7);
            } while (a2_lhu(PAD2_HELD) & BTN_R1);
        }
    }

    a2_charge_interp(2);
    a2_call0(cpu, 0x8008A5C4u, FN_SWAP_DISPLAY);
    a2_charge_interp(10);
    a2_call2(cpu, 0x8008A5DCu, FN_CLEAR_OTS, a2_lw(OTS + 0x4144), a2_lw(OTS + 0x414C));
    a2_charge_interp(2);
    a2_call0(cpu, 0x8008A5E4u, FN_DRAW_OTS);
    a2_charge_interp(2);
    a2_call0(cpu, 0x8008A5ECu, FN_INPUT_UPDATE);
    a2_charge_interp(14);
    return 1;
}
A2_REWRITE_OVERLAY(func_8008A390, 0x8008A390u, frame_end, A2_COST_SELF, A2_VOID,
                   0x27BDFFD8u, 0xAFB20020u, 0x00809021u, 0x3C020001u)
