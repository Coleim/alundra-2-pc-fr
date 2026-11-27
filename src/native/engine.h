/* engine.h - the standalone engine's internal interfaces (src/native/) */
#ifndef A2N_ENGINE_H
#define A2N_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "a2_native.h"

#ifdef __cplusplus
extern "C" {
#endif

extern CPUState a2n_cpu;

void a2n_fatal(const char *fmt, ...);
void a2n_host_fatal_hook(void);

/* I/O regions handled by a native module */
typedef struct {
    uint32_t lo, hi;
    uint32_t (*read)(uint32_t phys, int size);
    void (*write)(uint32_t phys, uint32_t value, int size);
} A2nIo;
void a2n_io_register(uint32_t lo, uint32_t hi, uint32_t (*rd)(uint32_t, int), void (*wr)(uint32_t, uint32_t, int));
const A2nIo *a2n_io_find(uint32_t phys);

/* functions: native replacements win over hand-written and translated code */
void a2n_native(uint32_t addr, const char *name, int (*fn)(CPUState *));
void a2n_dispatch(CPUState *cpu, uint32_t target);
const char *a2n_name_of(uint32_t addr);
void a2n_print_stack(void);
void a2n_check_write(uint32_t addr, uint32_t bytes, const char *what);
uint32_t a2n_call(CPUState *cpu, uint32_t fn, int nargs, ...);

/* interrupts and callbacks */
void a2n_call_interrupt(uint32_t fn, int nargs, uint32_t a0, uint32_t a1);
int a2n_interrupts_enabled(void);
int a2n_in_interrupt(void);
void a2n_set_critical(int on);

enum {
    A2N_EV_VBLANK = 1u << 0,
    A2N_EV_CD     = 1u << 1,
    A2N_EV_GPU    = 1u << 2,
    A2N_EV_DMA    = 1u << 3,
    A2N_EV_CDTIMER = 1u << 4,
};
void a2n_event_post(uint32_t ev);
void a2n_events_run(void);
void a2n_budget_spent(void);
uint64_t a2n_edges(void);
void a2n_cd_timer_set(uint32_t edges);
void a2n_cd_timer(void);

/* BIOS calls (0xA0 / 0xB0 / 0xC0 tables) */
void a2n_bios_call(CPUState *cpu, uint32_t table);

/* boot */
int a2n_load_exe(const uint8_t *exe, size_t size);
void a2n_modules_init(void);
void a2n_run(void);
void a2n_longjmp_resume(CPUState *cpu);
void a2n_unwind_to_boot(void);

/* helpers for native functions */
static inline uint32_t a2n_arg(CPUState *cpu, int i)
{
    return i < 4 ? cpu->gpr[4 + i] : a2n_r32(cpu->gpr[29] + 0x10u + 4u * (uint32_t)(i - 4));
}
static inline int a2n_ret(CPUState *cpu, uint32_t v)
{
    cpu->gpr[2] = v;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif
