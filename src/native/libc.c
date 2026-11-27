/*
 * libc.c - library functions whose translation cannot run as is on the engine:
 * critical sections (system calls), longjmp (unwinds the C stack), BIOS patching.
 */
#include <stdio.h>
#include <string.h>

#include "engine.h"
#include "libaddr.h"
#include "native_modules.h"

static int s_was_enabled = 1;

/* EnterCriticalSection: interrupts off; 1 if they were on */
static int n_enter_critical(CPUState *cpu)
{
    const int was = !a2n_in_interrupt() && s_was_enabled;
    s_was_enabled = 0;
    a2n_set_critical(1);
    return a2n_ret(cpu, (uint32_t)was);
}

static int n_exit_critical(CPUState *cpu)
{
    s_was_enabled = 1;
    a2n_set_critical(0);
    a2n_pending = 1;                               /* deliver what waited */
    return a2n_ret(cpu, 0);
}

/* longjmp(buf, value): registers from the buffer setjmp filled, then the code after that
 * setjmp continues (the translated caller has an entry there), from the engine's top */
static uint32_t s_jmp_value;

static int n_longjmp(CPUState *cpu)
{
    const uint32_t buf = cpu->gpr[4];
    s_jmp_value = cpu->gpr[5];
    cpu->gpr[31] = a2n_r32(buf + 0x00);
    cpu->gpr[28] = a2n_r32(buf + 0x2C);
    cpu->gpr[29] = a2n_r32(buf + 0x04);
    cpu->gpr[30] = a2n_r32(buf + 0x08);
    for (int i = 0; i < 8; ++i) cpu->gpr[16 + i] = a2n_r32(buf + 0x0C + 4u * (uint32_t)i);
    a2n_unwind_to_boot();
    return 1;
}

void a2n_longjmp_resume(CPUState *cpu)
{
    a2n_set_critical(0);
    s_was_enabled = 1;
    cpu->gpr[2] = s_jmp_value;
    cpu->load_value_rt = cpu->load_value_age = 0;
    const uint32_t at = cpu->gpr[31];
    a2n_dispatch(cpu, at);
    a2n_fatal("the code resumed by longjmp at %08x returned", at);
}

static int n_nothing(CPUState *cpu) { return a2n_ret(cpu, 0); }

void a2n_libc_init(void)
{
    a2n_native(0x80031238u, "EnterCriticalSection", n_enter_critical);
    a2n_native(0x80031248u, "ExitCriticalSection", n_exit_critical);
    a2n_native(LIB_longjmp, "longjmp", n_longjmp);
    a2n_native(LIB__patch_gte, "_patch_gte", n_nothing);
}
