/*
 * pad.c - the controllers (PsyQ libpad), from the host's gamepad and keyboard.
 *
 * Port 1 is a DualShock in analog mode, the controller the game expects for its stick
 * and vibration support; port 2 is empty. Every vertical blank the receive buffers the
 * game gave to PadInitDirect get the controller's answer, in the console's format: status,
 * type and size, buttons (0 = pressed), then the sticks (right X/Y, left X/Y).
 */
#include <string.h>

#include "engine.h"
#include "host.h"
#include "libaddr.h"
#include "native_modules.h"

static uint32_t s_buf[2];               /* receive buffers (PadInitDirect) */
static uint32_t s_act[2];               /* actuator data (PadSetAct) */
static int s_started;

void a2n_pad_vblank(void)
{
    if (!s_started) return;
    for (int port = 0; port < 2; ++port) {
        if (!s_buf[port]) continue;
        A2nPadState st;
        memset(&st, 0, sizeof st);
        if (port == 0) a2n_host_pad(0, &st);
        const uint32_t b = s_buf[port];
        if (!st.connected) {
            a2n_w8(b, 0xFF);
            a2n_w8(b + 1, 0);
            continue;
        }
        const uint16_t keys = (uint16_t)~st.buttons;
        a2n_w8(b, 0x00);
        a2n_w8(b + 1, st.analog ? 0x73 : 0x41);
        a2n_w8(b + 2, (uint8_t)(keys & 0xFF));
        a2n_w8(b + 3, (uint8_t)(keys >> 8));
        if (st.analog) {
            a2n_w8(b + 4, st.rx);
            a2n_w8(b + 5, st.ry);
            a2n_w8(b + 6, st.lx);
            a2n_w8(b + 7, st.ly);
        }
        if (s_act[port]) a2n_host_rumble(port, (uint8_t)a2n_r8(s_act[port]), (uint8_t)a2n_r8(s_act[port] + 1));
    }
}

static int connected(int port)
{
    if (port != 0) return 0;
    A2nPadState st;
    memset(&st, 0, sizeof st);
    a2n_host_pad(0, &st);
    return st.connected;
}

static int n_PadInitDirect(CPUState *cpu)
{
    s_buf[0] = cpu->gpr[4];
    s_buf[1] = cpu->gpr[5];
    for (int i = 0; i < 2; ++i)
        if (s_buf[i]) { a2n_w8(s_buf[i], 0xFF); a2n_w8(s_buf[i] + 1, 0); }
    return a2n_ret(cpu, 0);
}

static int n_PadStartCom(CPUState *cpu) { s_started = 1; return a2n_ret(cpu, 0); }
static int n_PadStopCom(CPUState *cpu) { s_started = 0; return a2n_ret(cpu, 0); }

/* 6 = stable (answering), 0 = nothing connected */
static int n_PadGetState(CPUState *cpu)
{
    const int port = (int)((cpu->gpr[4] >> 4) & 1);
    return a2n_ret(cpu, connected(port) ? 6u : 0u);
}

/* PadInfoMode(port, term, offs): 1 current type, 2 extended type, 3 mode, 4 mode table */
static int n_PadInfoMode(CPUState *cpu)
{
    const int port = (int)((cpu->gpr[4] >> 4) & 1);
    if (!connected(port)) return a2n_ret(cpu, 0);
    switch (cpu->gpr[5]) {
    case 1: return a2n_ret(cpu, 7);              /* analog (0x73) */
    case 2: return a2n_ret(cpu, 7);
    case 3: return a2n_ret(cpu, 1);
    case 4: return a2n_ret(cpu, cpu->gpr[6] == 0 ? 4u : cpu->gpr[6] == 1 ? 7u : 0u);
    default: return a2n_ret(cpu, 0);
    }
}

/* PadInfoAct(port, actuator, term): two motors, the small one on/off, the big one variable */
static int n_PadInfoAct(CPUState *cpu)
{
    const int port = (int)((cpu->gpr[4] >> 4) & 1);
    if (!connected(port)) return a2n_ret(cpu, 0);
    const int32_t act = (int32_t)cpu->gpr[5];
    if (act < 0) return a2n_ret(cpu, 2);
    switch (cpu->gpr[6]) {
    case 1: return a2n_ret(cpu, 1);              /* function: rotation */
    case 2: return a2n_ret(cpu, act == 0 ? 1u : 2u);
    case 3: return a2n_ret(cpu, act == 0 ? 0u : 1u);
    case 4: return a2n_ret(cpu, act == 0 ? 10u : 20u);
    default: return a2n_ret(cpu, 0);
    }
}

static int n_PadInfoComb(CPUState *cpu) { return a2n_ret(cpu, 0); }

static int n_PadSetAct(CPUState *cpu)
{
    const int port = (int)((cpu->gpr[4] >> 4) & 1);
    s_act[port] = cpu->gpr[5];
    return a2n_ret(cpu, 1);
}

static int n_PadSetActAlign(CPUState *cpu) { return a2n_ret(cpu, 1); }

void a2n_pad_init(void)
{
    a2n_native(LIB_PadInitDirect, "PadInitDirect", n_PadInitDirect);
    a2n_native(LIB_PadStartCom, "PadStartCom", n_PadStartCom);
    a2n_native(LIB_PadStopCom, "PadStopCom", n_PadStopCom);
    a2n_native(LIB_PadGetState, "PadGetState", n_PadGetState);
    a2n_native(LIB_PadInfoMode, "PadInfoMode", n_PadInfoMode);
    a2n_native(LIB_PadInfoAct, "PadInfoAct", n_PadInfoAct);
    a2n_native(LIB_PadInfoComb, "PadInfoComb", n_PadInfoComb);
    a2n_native(LIB_PadSetAct, "PadSetAct", n_PadSetAct);
    a2n_native(LIB_PadSetActAlign, "PadSetActAlign", n_PadSetActAlign);
}
