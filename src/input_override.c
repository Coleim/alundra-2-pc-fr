/* REWRITTEN: func_800A09C0 */
/*
 * Player input heading (func_800A09C0 in GAME00.BIN, loaded at 0x80084ED0).
 *
 * The game keeps its input state at 0x80060C40 (one 0x18-byte record per pad):
 *   +0x02  u16  buttons held (libpad bit layout, active high)
 *   +0x08  s32  8-way direction index, -1 when no direction is pressed
 *   +0x10  s32  direction angle: index * 64 on a 512-unit circle, -1 when none
 * Angles are counter-clockwise from "right": 0 right, 128 up, 256 left, 384 down.
 *
 * This function turns that angle into the world heading the entities use, adding
 * the camera yaw. The faithful version below mirrors the original instruction for
 * instruction; the analog extension replaces the 8-way angle by the exact stick angle.
 *
 * Environment:
 *   A2_ANALOG=0   disable the analog extension (default: enabled)
 *   A2_VERIFY=1   also run the original code and count disagreements
 *                 (counters a2.heading.calls / a2.heading.mismatch)
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cpu_state.h"
#include "func_override.h"

extern uint32_t psx_read_word(uint32_t addr);
extern uint16_t psx_read_half(uint32_t addr);
extern void psx_mod_counter_add(const char *name, uint32_t delta);
extern int psx_mod_register_command(const char *name,
                                    int (*fn)(const char *args, char *reply, int cap));

#define INPUT_STATE      0x80060C40u   /* game-level input record, pad 1 */
#define PAD_RAW          0x8006AF20u   /* libpad-derived record, pad 1, stride 0x10C */
#define PAD_RAW_LX       (PAD_RAW + 0x104u)
#define PAD_RAW_LY       (PAD_RAW + 0x108u)
#define HEADING_MODE     0x8011BAF8u   /* s16: 2 = side-scroll style left/right only */
#define CAMERA_YAW       0x8011BBD8u   /* s16 */
#define FORCED_HEADING   0x8012D2A8u   /* s16[5]: +4 input angle, +6 heading, +8 enable */

#define BTN_RIGHT        0x2000u
#define BTN_LEFT         0x8000u
#define STICK_DEAD_ZONE  64            /* same box as the game's own stick-to-d-pad rule */

static int s_analog = 1;
static int s_verify = 0;

/* Last values, for the "a2_heading" debug command. */
static volatile int32_t s_last_input_angle = -2;
static volatile int32_t s_last_fine = -2;
static volatile int32_t s_last_result = -2;
static volatile int32_t s_last_stick_x, s_last_stick_y;

static int32_t s16(uint16_t v) { return (int16_t)v; }

/* Faithful port of func_800A09C0. */
static int32_t input_heading_faithful(int32_t *input_angle_out)
{
    int32_t angle = (int32_t)psx_read_word(INPUT_STATE + 0x10);
    *input_angle_out = angle;
    if (angle == -1)
        return -1;

    if (s16(psx_read_half(HEADING_MODE)) == 2) {
        uint16_t held = psx_read_half(INPUT_STATE + 0x02);
        if (held & BTN_RIGHT) angle = 0;
        if (held & BTN_LEFT)  angle = 0x100;
    }

    if (s16(psx_read_half(FORCED_HEADING + 8)) != 0 &&
        angle == s16(psx_read_half(FORCED_HEADING + 4)))
        return s16(psx_read_half(FORCED_HEADING + 6));

    return (angle + s16(psx_read_half(CAMERA_YAW)) + 0x80) & 0x1FF;
}

/* Exact stick angle on the 512-unit circle, if the stick is past the dead zone. */
static int stick_angle(int32_t *angle_out)
{
    int32_t lx = (int32_t)psx_read_word(PAD_RAW_LX);
    int32_t ly = (int32_t)psx_read_word(PAD_RAW_LY);
    if (lx * lx + ly * ly < STICK_DEAD_ZONE * STICK_DEAD_ZONE)
        return 0;
    double turns = atan2((double)(-ly), (double)lx) / (2.0 * M_PI);
    *angle_out = (int32_t)lround(turns * 512.0) & 0x1FF;
    return 1;
}

static int input_heading_override(CPUState *cpu)
{
    int32_t input_angle;
    int32_t result = input_heading_faithful(&input_angle);
    const int32_t faithful = result;

    int32_t fine_used = -1;
    if (s_analog && input_angle != -1 &&
        s16(psx_read_half(HEADING_MODE)) != 2 &&
        s16(psx_read_half(FORCED_HEADING + 8)) == 0) {
        int32_t fine;
        if (stick_angle(&fine)) {
            result = (fine + s16(psx_read_half(CAMERA_YAW)) + 0x80) & 0x1FF;
            fine_used = fine;
        }
    }

    if (s_verify) {
        func_override_call_original(cpu);
        psx_mod_counter_add("a2.heading.calls", 1);
        if ((int32_t)cpu->gpr[2] != faithful)
            psx_mod_counter_add("a2.heading.mismatch", 1);
    }

    s_last_input_angle = input_angle;
    s_last_result = result;
    s_last_fine = fine_used;

    cpu->gpr[2] = (uint32_t)result;
    cpu->gpr[4] = (uint32_t)result;
    return 1;
}

static int heading_command(const char *args, char *reply, int cap)
{
    (void)args;
    return snprintf(reply, (size_t)cap, "input_angle=%d fine=%d result=%d analog=%d",
                    (int)s_last_input_angle, (int)s_last_fine, (int)s_last_result, s_analog);
}

__attribute__((constructor))
static void a2_register_input_overrides(void)
{
    const char *e;
    if ((e = getenv("A2_ANALOG")) && e[0] == '0') s_analog = 0;
    if ((e = getenv("A2_VERIFY")) && e[0] == '1') s_verify = 1;

    /* First instructions of func_800A09C0: guards against another overlay being resident. */
    static const uint32_t guard[4] = { 0x3c028006u, 0x24450c40u, 0x8ca40010u, 0x2402ffffu };
    psx_mod_register_command("a2_heading", heading_command);
    func_override_add_guarded("a2.input.heading", 0x800A09C0u, input_heading_override,
                              guard, 4, 40);
}
