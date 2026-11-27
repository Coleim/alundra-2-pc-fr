/*
 * Field camera (GAME00), rewritten from the original MIPS code.
 *
 * The camera follows a point (`pos`) that chases its target (an actor, plus an offset); the
 * eye sits at `eye`, `eye_ofs` = eye - pos. The view matrix and translation are rebuilt from
 * them by func_8008B8F4 (still lifted). Each frame, camera_update calls the handler of the
 * current mode through the table at 0x800EEB10.
 *
 * Positions are 20.12 fixed point (4096 = one unit). Angles: 512 = a turn for the yaw
 * (sine / cosine tables of 512 s16 entries), 4096 = a turn for the GTE rotation (rot.y =
 * yaw * 8).
 *
 * Timing: overlay code ran in the interpreter; the spans of the original's blocks on the
 * path taken are charged through the interpreter estimate a2_charge_interp (absorbed by the
 * frame's VSync wait).
 */
#include "../a2_rewrite.h"

#define CAM              0x8011BAF8u
enum {                                  /* camera fields */
    CAM_MODE        = 0x000,            /* s16: handler index; 1 = recorded path, 2 = ... */
    CAM_TARGET      = 0x004,            /* actor followed (position +0x18/+0x1C/+0x20) */
    CAM_LOOK_TARGET = 0x008,            /* actor the eye moves to while LOOK is set */
    CAM_ROT         = 0x00C,            /* s16 x, y, z: GTE rotation */
    CAM_DEBUG_FREE  = 0x03C,            /* s16: debug free camera (pad driven) */
    CAM_ORBIT_DIST  = 0x050,            /* s32: eye distance in orbit mode (8 fraction bits) */
    CAM_ZOOM_FRAMES = 0x054,            /* s16: frames of zoom left */
    CAM_ZOOM_SPEED  = 0x058,            /* s32: eye moves along eye_ofs by this * unit / 256 */
    CAM_LOOK        = 0x05C,            /* u8: the eye travels to the look target */
    CAM_LOOK_STEP   = 0x060,            /* s32 x3: eye move per frame */
    CAM_LOOK_FRAMES = 0x06C,            /* s32: frames of travel left */
    CAM_LOOK_OFS    = 0x070,            /* s32 x3: eye offset from the look target */
    CAM_TARGET_OFS  = 0x080,            /* s32 x3: where pos wants to be, from the target */
    CAM_POS         = 0x08C,            /* s32 x3 */
    CAM_EYE_OFS     = 0x09C,            /* s32 x3 */
    CAM_EYE         = 0x0AC,            /* s32 x3 */
    CAM_VIEW_OFS    = 0x0BC,            /* s32 x3: eye offset used for the view */
    CAM_EYE_HEIGHT  = 0x0DC,            /* s32 */
    CAM_YAW         = 0x0E0,            /* u16 0..511 */
    CAM_YAW_TARGET  = 0x0E2,            /* u16 */
    CAM_TURN_FRAMES = 0x0E4,            /* s16: frames of turn left (orbit mode) */
    CAM_TURN_STEP   = 0x0E6,            /* u16: yaw added per frame while turning */
    CAM_FLAGS       = 0x104,            /* s32: 1 frozen, 0x20 clamp moves, 0x80 ease in */
    CAM_KIND        = 0x108,            /* s32: 1 orbit around the target, 2 rail */
    CAM_CLAMP       = 0x10C,            /* s32: largest move per frame (flag 0x20) */
    CAM_FLAG_FRAMES = 0x110,            /* s32: frames before the flags clear; ease divisor */
    CAM_TURN_ARMED  = 0x1B0,            /* u8: start a turn when none is running */
};
#define CAM_SMOOTHING    0x8011BC00u    /* s32: 1 or 3 = dead zone and catch-up on pos */
#define CAM_MOVING       0x8011BBFCu
#define GAME_FLAGS       0x800FA7E8u    /* bits 0x83: menus / pause, the camera stands still */
#define SINE             0x80085258u    /* s16[512] */
#define COSINE           0x80085658u    /* s16[512] */
#define HANDLERS         0x800EEB10u    /* camera handler per mode */

#define FN_PATH_STEP     0x8008AB3Cu
#define FN_NORMALIZE     0x800275B0u
#define FN_BUILD_VIEW    0x8008B8F4u
#define FN_FOLLOW        0x8008B044u

static int32_t lw_s(uint32_t a) { return (int32_t)a2_lw(a); }
static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

/* MIPS signed division. The original traps on a zero divisor (break 7), which the game
 * never reaches; the quotient is then what the CPU leaves in LO. */
static int32_t sdiv(int32_t x, int32_t y)
{
    if (y == 0) return x >= 0 ? -1 : 1;
    if (y == -1 && x == INT32_MIN) return x;
    return x / y;
}

/* Dead zone, then catch-up: inside [lo, hi] the camera moves 1/16 of the way, beyond hi
 * it jumps back to hi (x and z also keep the 1/16 step). */
static int32_t smooth_xz(int32_t d, int32_t ad, uint32_t *cyc)
{
    int32_t move = 0;
    *cyc += 8 + 3;
    if ((uint32_t)(ad - 0x3C000) <= 0x14000u) { move = d >> 4; *cyc += 1; }
    if (ad > 0x50000) {
        move = d + (d > 0 ? (d >> 4) - 0x50000 : (d >> 4) + 0x50000);
        *cyc += 2 + (d > 0 ? 2 : 1) + 1;
    }
    return move;
}

/* func_8008B044: move the camera toward its target. Returns 1 when the camera stands still
 * this frame (debug camera, pause, frozen), else 0. */
static int camera_follow(CPUState *cpu)
{
    uint32_t cyc = 12;
    if (a2_lh(CAM + CAM_DEBUG_FREE)) { cpu->gpr[2] = 1; a2_charge_interp(cyc + 11); return 1; }
    cyc += 8;
    if (a2_lw(GAME_FLAGS) & 0x83) { cpu->gpr[2] = 1; a2_charge_interp(cyc + 11); return 1; }
    const uint32_t flags = a2_lw(CAM + CAM_FLAGS);
    cyc += 7;
    if (flags & 1) { cpu->gpr[2] = 1; a2_charge_interp(cyc + 11); return 1; }
    cyc += 2;
    a2_charge_interp(cyc);
    cyc = 2;
    if (a2_call0(cpu, 0x8008B098u, FN_PATH_STEP)) { cpu->gpr[2] = 1; a2_charge_interp(cyc + 11); return 1; }

    /* distance from pos to where it wants to be */
    int32_t extra[3] = { 0, 0, 0 }, d[3];
    const uint32_t target = a2_lw(CAM + CAM_TARGET);
    cyc += 11;
    if (target) {
        cyc += 6 + 37;
        if (lw_s(CAM + CAM_KIND) == 2)
            for (int i = 0; i < 3; ++i) extra[i] = lw_s(target + 0x90 + 4 * i), cyc += 3;
        for (int i = 0; i < 3; ++i)
            d[i] = (lw_s(target + 0x18 + 4 * i) + lw_s(CAM + CAM_TARGET_OFS + 4 * i)) -
                   (lw_s(CAM + CAM_POS + 4 * i) + extra[i]);
    } else {
        cyc += 21;
        for (int i = 0; i < 3; ++i) d[i] = lw_s(CAM + CAM_TARGET_OFS + 4 * i) - lw_s(CAM + CAM_POS + 4 * i);
    }
    int32_t ad[3];
    for (int i = 0; i < 3; ++i) { ad[i] = iabs(d[i]); cyc += 2 + (d[i] < 0); }

    int32_t move[3] = { 0, 0, 0 };
    const int32_t smoothing = lw_s(CAM_SMOOTHING);
    cyc += 7;
    if (smoothing != 1) cyc += 2;
    if (smoothing == 1 || smoothing == 3) {
        move[0] = smooth_xz(d[0], ad[0], &cyc);
        cyc += 7;
        if (a2_lh(CAM + CAM_MODE) == 2) {
            cyc += 6 + 4;
            if ((uint32_t)(ad[1] - 0x38000) < 0x4001u) { move[1] = d[1] >> 4; cyc += 1; }
            if (ad[1] > 0x3C000) { move[1] = d[1] > 0 ? d[1] - 0x3C000 : d[1] + 0x3C000; cyc += 2 + (d[1] > 0 ? 0 : 2); }
        } else {
            cyc += 7 + 3;
            if ((uint32_t)(ad[1] - 0x38000) <= 0x18000u) { move[1] = d[1] >> 4; cyc += 1; }
            if (ad[1] > 0x50000) { move[1] = d[1] > 0 ? d[1] - 0x50000 : d[1] + 0x50000; cyc += 2 + (d[1] > 0 ? 0 : 1); }
        }
        move[2] = smooth_xz(d[2], ad[2], &cyc);
        cyc += 1;
    }

    const int32_t kind = lw_s(CAM + CAM_KIND);
    cyc += 7;
    if (kind == 2) {                                 /* rail: ease in, clamp, or follow at once */
        cyc += 3;
        if (flags & 0x80) {
            const int32_t div = lw_s(CAM + CAM_FLAG_FRAMES);
            for (int i = 0; i < 3; ++i) move[i] = sdiv(d[i], div);
            cyc += 7 + 24;
        } else if (flags & 0x20) {
            const int32_t limit = lw_s(CAM + CAM_CLAMP);
            cyc += 2 + 7 + 2 + 3 + 2;
            for (int i = 0; i < 3; ++i) {
                move[i] = d[i];
                if (ad[i] >= limit) { move[i] = d[i] < 0 ? -limit : limit; cyc += 2; }
            }
            if (!move[0] && !move[1] && !move[2]) { a2_sw(CAM_MOVING, 0); cyc += 6; }
        } else {
            for (int i = 0; i < 3; ++i) move[i] = d[i];
            cyc += 2 + 2 + 1;
        }
    }
    cyc += 1 + 6;

    if (kind == 1) {                                 /* orbit: turn around the target */
        cyc += 6;
        int run = a2_lh(CAM + CAM_TURN_FRAMES) != 0;
        if (!run) {
            cyc += 6;
            if (a2_lbu(CAM + CAM_TURN_ARMED)) { a2_sh(CAM + CAM_TURN_FRAMES, (uint32_t)kind); run = 1; cyc += 1; }
        }
        if (run) {
            const int32_t frames = a2_lh(CAM + CAM_TURN_FRAMES);
            move[0] = sdiv(d[0], frames);
            cyc += 11 + 3 + 3 + 3;
            if ((uint32_t)(ad[1] - 0x38000) <= 0x18000u) { move[1] = d[1] >> 4; cyc += 1; }
            if (ad[1] > 0x50000) { move[1] = d[1] > 0 ? d[1] - 0x50000 : d[1] + 0x50000; cyc += 2 + (d[1] > 0 ? 0 : 2); }
            const uint32_t yaw = (a2_lhu(CAM + CAM_YAW_TARGET) + a2_lhu(CAM + CAM_TURN_STEP)) & 0x1FF;
            a2_sh(CAM + CAM_YAW_TARGET, yaw);
            a2_sh(CAM + CAM_YAW, yaw);
            move[2] = sdiv(d[2], (int16_t)a2_lh(CAM + CAM_TURN_FRAMES));
            a2_sh(CAM + CAM_ROT + 2, ((yaw + 0x80) & 0x1FF) << 3);
            const uint32_t left = a2_lhu(CAM + CAM_TURN_FRAMES) - 1;
            a2_sh(CAM + CAM_TURN_FRAMES, left);
            cyc += 19 + 3 + 11;
            if ((left & 0xFFFF) == 0) { a2_sh(CAM + CAM_TURN_STEP, 0); cyc += 1; }
        }
    }

    /* pos follows; the eye follows pos, travels to the look target, or orbits */
    int32_t pos[3], step[3];
    for (int i = 0; i < 3; ++i) {
        step[i] = move[i] + extra[i];
        pos[i] = lw_s(CAM + CAM_POS + 4 * i) + step[i];
    }
    const uint32_t look = a2_lbu(CAM + CAM_LOOK);
    for (int i = 0; i < 3; ++i) a2_sw(CAM + CAM_POS + 4 * i, (uint32_t)pos[i]);
    cyc += 23;
    if (!look) {
        cyc += 6;
        if (lw_s(CAM + CAM_KIND) == 1) {
            const int32_t dist = lw_s(CAM + CAM_ORBIT_DIST);
            const int32_t r = (dist < 0 ? dist + 0xFF : dist) >> 8;
            const int32_t yaw = a2_lh(CAM + CAM_YAW_TARGET);
            const int32_t ex = (int32_t)((uint32_t)a2_lh(SINE + 2 * (uint32_t)yaw) * (uint32_t)r);
            a2_sw(CAM + CAM_EYE_OFS + 0, (uint32_t)ex);
            const int32_t ez = (int32_t)((uint32_t)a2_lh(COSINE + 2 * (uint32_t)yaw) * (uint32_t)r);
            a2_sw(CAM + CAM_EYE + 0, (uint32_t)(pos[0] + ex));
            a2_sw(CAM + CAM_EYE + 4, (uint32_t)(pos[1] + lw_s(CAM + CAM_EYE_OFS + 4)));
            a2_sw(CAM + CAM_EYE_OFS + 8, (uint32_t)ez);
            a2_sw(CAM + CAM_EYE + 8, (uint32_t)(pos[2] + ez));
            cyc += 15 + (dist < 0) + 24;
        } else {
            for (int i = 0; i < 3; ++i) {
                const int32_t e = lw_s(CAM + CAM_EYE + 4 * i) + step[i];
                a2_sw(CAM + CAM_EYE + 4 * i, (uint32_t)e);
                a2_sw(CAM + CAM_EYE_OFS + 4 * i, (uint32_t)(e - pos[i]));
            }
            cyc += 22;
        }
    } else {
        const uint32_t to = a2_lw(CAM + CAM_LOOK_TARGET);
        cyc += 6;
        if (to) {
            cyc += 6;
            if (!a2_lw(CAM + CAM_LOOK_FRAMES)) { a2_sw(CAM + CAM_LOOK_FRAMES, 1); cyc += 2; }
            for (int i = 0; i < 3; ++i) {
                const int32_t ex = lw_s(to + 0x90 + 4 * i);
                const int32_t want = lw_s(to + 0x18 + 4 * i) + lw_s(CAM + CAM_LOOK_OFS + 4 * i);
                const int32_t gap = want - (lw_s(CAM + CAM_EYE + 4 * i) + ex);
                a2_sw(CAM + CAM_LOOK_STEP + 4 * i, (uint32_t)(sdiv(gap, lw_s(CAM + CAM_LOOK_FRAMES)) + ex));
            }
            cyc += 21 + 3 + 24 + 3 + 18 + 3 + 5;
        }
        int32_t eye[3];
        for (int i = 0; i < 3; ++i) {
            eye[i] = lw_s(CAM + CAM_EYE + 4 * i) + lw_s(CAM + CAM_LOOK_STEP + 4 * i);
            a2_sw(CAM + CAM_EYE + 4 * i, (uint32_t)eye[i]);
        }
        const int32_t height = eye[1] - lw_s(CAM + CAM_POS + 4);
        a2_sw(CAM + CAM_VIEW_OFS + 4, (uint32_t)height);
        a2_sw(CAM + CAM_EYE_HEIGHT, (uint32_t)height);
        cyc += 35;
        const int32_t frames = lw_s(CAM + CAM_LOOK_FRAMES);
        if (frames) {
            a2_sw(CAM + CAM_LOOK_FRAMES, (uint32_t)(frames - 1));
            cyc += 3;
            if (frames - 1 == 0) {
                for (int i = 2; i >= 0; --i) a2_sw(CAM + CAM_LOOK_STEP + 4 * i, 0);
                cyc += 3;
            }
        }
        for (int i = 0; i < 3; ++i)
            a2_sw(CAM + CAM_EYE_OFS + 4 * i, (uint32_t)(lw_s(CAM + CAM_EYE + 4 * i) - lw_s(CAM + CAM_POS + 4 * i)));
        cyc += 26;
    }

    /* zoom: the eye moves along eye_ofs */
    cyc += 8;
    if (a2_lh(CAM + CAM_ZOOM_FRAMES)) {
        const int32_t speed = lw_s(CAM + CAM_ZOOM_SPEED);
        const uint32_t frame_sp = cpu->gpr[29] - 0x30u, dir = frame_sp + 0x10u;   /* the original's frame */
        cyc += 5;
        a2_charge_interp(cyc);
        cyc = 0;
        cpu->gpr[29] = frame_sp;
        a2_call2(cpu, 0x8008B804u, FN_NORMALIZE, CAM + CAM_EYE_OFS, dir);
        cpu->gpr[29] = frame_sp + 0x30u;
        int32_t eye[3];
        for (int i = 0; i < 3; ++i) {
            int32_t m = (int32_t)(a2_lw(dir + 4 * i) * (uint32_t)speed);
            if (m < 0) { m += 0xFF; cyc += 1; }
            eye[i] = lw_s(CAM + CAM_EYE + 4 * i) + (m >> 8);
        }
        a2_sw(CAM + CAM_EYE + 0, (uint32_t)eye[0]);
        a2_sw(CAM + CAM_EYE + 4, (uint32_t)eye[1]);
        a2_sw(CAM + CAM_EYE_OFS + 4, (uint32_t)(eye[1] - lw_s(CAM + CAM_POS + 4)));
        a2_sw(CAM + CAM_EYE + 8, (uint32_t)eye[2]);
        a2_sw(CAM + CAM_EYE_OFS + 0, (uint32_t)(eye[0] - lw_s(CAM + CAM_POS + 0)));
        a2_sw(CAM + CAM_EYE_OFS + 8, (uint32_t)(eye[2] - lw_s(CAM + CAM_POS + 8)));
        a2_sh(CAM + CAM_ZOOM_FRAMES, a2_lhu(CAM + CAM_ZOOM_FRAMES) - 1);
        cyc += 8 + 13 + 13 + 26;
    }

    /* the flags last a number of frames */
    cyc += 8;
    const int32_t frames = lw_s(CAM + CAM_FLAG_FRAMES);
    if (frames) {
        a2_sw(CAM + CAM_FLAG_FRAMES, (uint32_t)(frames - 1));
        cyc += 2;
        if (frames - 1 == 0) { a2_sw(CAM + CAM_FLAGS, 0); cyc += 1; }
    }
    cpu->gpr[2] = 0;
    a2_charge_interp(cyc + 1 + 11);
    return 1;
}
A2_REWRITE_OVERLAY(func_8008B044, 0x8008B044u, camera_follow, A2_COST_SELF, A2_PURE,
                   0x27BDFFD0u, 0x3C028012u, 0xAFB00020u, 0x2450BAF8u)

/* func_8008BDF8 / func_8008BE44: follow modes. When the camera moved, the view uses the new
 * eye offset and the view matrix is rebuilt. */
static int camera_follow_mode(CPUState *cpu, uint32_t base)
{
    const uint32_t frame_sp = cpu->gpr[29] - 0x38u;
    a2_charge_interp(5);
    cpu->gpr[29] = frame_sp;
    const uint32_t still = a2_call2(cpu, base + 0x0C, FN_FOLLOW, frame_sp + 0x10, frame_sp + 0x20);
    cpu->gpr[29] = frame_sp + 0x38u;
    a2_charge_interp(2);
    if (!still) {
        for (int i = 0; i < 3; ++i) a2_sw(CAM + CAM_VIEW_OFS + 4 * i, a2_lw(CAM + CAM_EYE_OFS + 4 * i));
        a2_charge_interp(9);
        a2_call0(cpu, base + 0x34, FN_BUILD_VIEW);
    }
    a2_charge_interp(4);
    return 1;
}
static int camera_mode_8008BDF8(CPUState *cpu) { return camera_follow_mode(cpu, 0x8008BDF8u); }
static int camera_mode_8008BE44(CPUState *cpu) { return camera_follow_mode(cpu, 0x8008BE44u); }
A2_REWRITE_OVERLAY(func_8008BDF8, 0x8008BDF8u, camera_mode_8008BDF8, A2_COST_SELF, A2_VOID,
                   0x27BDFFC8u, 0xAFBF0030u, 0x27A40010u, 0x0C022C11u)
A2_REWRITE_OVERLAY(func_8008BE44, 0x8008BE44u, camera_mode_8008BE44, A2_COST_SELF, A2_VOID,
                   0x27BDFFC8u, 0xAFBF0030u, 0x27A40010u, 0x0C022C11u)

/* func_8008BE90: run the handler of the camera's mode. */
static int camera_update(CPUState *cpu)
{
    const uint32_t handler = a2_lw(HANDLERS + 4u * (uint32_t)a2_lh(CAM + CAM_MODE));
    a2_charge_interp(10);
    a2_call0(cpu, 0x8008BEB8u, handler);
    a2_charge_interp(6);
    return 1;
}
A2_REWRITE_OVERLAY(func_8008BE90, 0x8008BE90u, camera_update, A2_COST_SELF, A2_VOID,
                   0x27BDFFE8u, 0x3C03800Fu, 0x3C028012u, 0x8442BAF8u)
