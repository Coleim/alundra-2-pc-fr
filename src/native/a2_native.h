/*
 * a2_native.h - the standalone engine's view of the guest machine, for the game's C code.
 *
 * The game keeps its original memory layout: 2 MB of RAM at 0x80000000 (mirrored in KUSEG
 * and KSEG1), the 1 KB scratchpad at 0x1F800000, and 32-bit pointers everywhere. The CPU
 * registers live in CPUState, as the translated code expects. There is no processor to
 * simulate: the code is C, memory is two arrays, and the few hardware registers the game
 * still writes (the sound chip) go to src/native/ modules.
 */
#ifndef A2_NATIVE_H
#define A2_NATIVE_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPUState {
    uint32_t gpr[32];
    uint32_t hi, lo;
    uint32_t pc;
    uint32_t cop0[32];
    uint32_t gte_data[32];
    uint32_t gte_ctrl[32];
    /* load delay: a load's value reaches its register one instruction later */
    uint32_t load_value_rt, load_value, load_value_age;
} CPUState;

#define A2N_RAM_SIZE     0x200000u
#define A2N_SCRATCH_SIZE 0x400u
extern uint8_t a2n_ram[A2N_RAM_SIZE];
extern uint8_t a2n_scratch[A2N_SCRATCH_SIZE];

/* hardware registers and anything outside RAM and the scratchpad */
uint32_t a2n_io_read(uint32_t phys, int size);
void a2n_io_write(uint32_t phys, uint32_t value, int size);

static inline uint32_t a2n_r32(uint32_t a)
{
    const uint32_t p = a & 0x1FFFFFFFu;
    uint32_t v;
    if (p < 0x00800000u) { memcpy(&v, a2n_ram + (p & 0x1FFFFCu), 4); return v; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { memcpy(&v, a2n_scratch + (p & 0x3FCu), 4); return v; }
    return a2n_io_read(p, 4);
}
static inline uint32_t a2n_r16(uint32_t a)
{
    const uint32_t p = a & 0x1FFFFFFFu;
    uint16_t v;
    if (p < 0x00800000u) { memcpy(&v, a2n_ram + (p & 0x1FFFFEu), 2); return v; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { memcpy(&v, a2n_scratch + (p & 0x3FEu), 2); return v; }
    return a2n_io_read(p, 2) & 0xFFFFu;
}
static inline uint32_t a2n_r8(uint32_t a)
{
    const uint32_t p = a & 0x1FFFFFFFu;
    if (p < 0x00800000u) return a2n_ram[p & 0x1FFFFFu];
    if ((p & 0xFFFFFC00u) == 0x1F800000u) return a2n_scratch[p & 0x3FFu];
    return a2n_io_read(p, 1) & 0xFFu;
}
#ifdef A2N_WATCH
/* debug builds: report stores into the executable's code and constants */
void a2n_watch_hit(uint32_t addr, uint32_t value, int size);
extern uint32_t a2n_watch_lo, a2n_watch_hi;
#define A2N_WATCHED(p, v, n) do { if ((((p) & 0x1FFFFFu) - 0x17000u < 0x1A228u && (p) < 0x00800000u) || \
                                      (((p) & 0x1FFFFFu) >= a2n_watch_lo && ((p) & 0x1FFFFFu) < a2n_watch_hi)) a2n_watch_hit((p), (v), (n)); } while (0)
#else
#define A2N_WATCHED(p, v, n) ((void)0)
#endif

static inline void a2n_w32(uint32_t a, uint32_t v)
{
    const uint32_t p = a & 0x1FFFFFFFu;
    A2N_WATCHED(p, v, 4);
    if (p < 0x00800000u) { memcpy(a2n_ram + (p & 0x1FFFFCu), &v, 4); return; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { memcpy(a2n_scratch + (p & 0x3FCu), &v, 4); return; }
    a2n_io_write(p, v, 4);
}
static inline void a2n_w16(uint32_t a, uint16_t v)
{
    const uint32_t p = a & 0x1FFFFFFFu;
    A2N_WATCHED(p, v, 2);
    if (p < 0x00800000u) { memcpy(a2n_ram + (p & 0x1FFFFEu), &v, 2); return; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { memcpy(a2n_scratch + (p & 0x3FEu), &v, 2); return; }
    a2n_io_write(p, v, 2);
}
static inline void a2n_w8(uint32_t a, uint8_t v)
{
    const uint32_t p = a & 0x1FFFFFFFu;
    A2N_WATCHED(p, v, 1);
    if (p < 0x00800000u) { a2n_ram[p & 0x1FFFFFu] = v; return; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { a2n_scratch[p & 0x3FFu] = v; return; }
    a2n_io_write(p, v, 1);
}
/* host pointer to guest RAM (n bytes from a, within RAM), or NULL */
static inline uint8_t *a2n_ptr(uint32_t a, uint32_t n)
{
    const uint32_t p = a & 0x1FFFFFFFu;
    if (p < 0x00800000u && (p & 0x1FFFFFu) + n <= A2N_RAM_SIZE) return a2n_ram + (p & 0x1FFFFFu);
    if ((p & 0xFFFFFC00u) == 0x1F800000u && (p & 0x3FFu) + n <= A2N_SCRATCH_SIZE) return a2n_scratch + (p & 0x3FFu);
    return 0;
}

/* swl / swr: the bytes of a register stored into an unaligned word */
static inline uint32_t a2n_swl_word(uint32_t w, uint32_t addr, uint32_t v)
{
    switch (addr & 3u) {
    case 0: return (w & 0xFFFFFF00u) | (v >> 24);
    case 1: return (w & 0xFFFF0000u) | (v >> 16);
    case 2: return (w & 0xFF000000u) | (v >> 8);
    default: return v;
    }
}
static inline uint32_t a2n_swr_word(uint32_t w, uint32_t addr, uint32_t v)
{
    switch (addr & 3u) {
    case 0: return v;
    case 1: return (w & 0x000000FFu) | (v << 8);
    case 2: return (w & 0x0000FFFFu) | (v << 16);
    default: return (w & 0x00FFFFFFu) | (v << 24);
    }
}

/* the load-delay pipeline (names as in the framework, for the translated code) */
static inline void psx_load_value_commit(CPUState *cpu)
{
    if (cpu->load_value_rt) cpu->gpr[cpu->load_value_rt] = cpu->load_value;
    cpu->load_value_rt = cpu->load_value_age = 0;
    cpu->gpr[0] = 0;
}
static inline void psx_load_value_begin(CPUState *cpu)
{
    if (cpu->load_value_rt) {
        if (cpu->load_value_age) psx_load_value_commit(cpu);
        else cpu->load_value_age = 1;
    }
}
static inline void psx_load_value_cancel(CPUState *cpu, uint32_t rt)
{
    if (rt && cpu->load_value_rt == rt) cpu->load_value_rt = cpu->load_value_age = 0;
}
static inline void psx_load_value_arm(CPUState *cpu, uint32_t rt, uint32_t value)
{
    if (cpu->load_value_rt && cpu->load_value_rt != rt) psx_load_value_commit(cpu);
    cpu->load_value_rt = rt;
    cpu->load_value = value;
    cpu->load_value_age = 0;
}

/* interrupts: delivered at the translated code's control transfers. a2n_budget counts
 * those transfers down: in deterministic mode it is the clock (a vertical blank also comes
 * after that much work, so busy-waiting code sees time pass); in real time it is unused. */
extern volatile int a2n_pending;
extern int32_t a2n_budget;
void a2n_service(CPUState *cpu);
static inline void a2n_edge(CPUState *cpu)
{
    if (a2n_pending | (--a2n_budget < 0)) a2n_service(cpu);
}

/* GTE (src/native/gte.c) */
uint32_t gte_read_data(CPUState *cpu, uint8_t reg);
void gte_write_data(CPUState *cpu, uint8_t reg, uint32_t val);
uint32_t gte_read_ctrl(CPUState *cpu, uint8_t reg);
void gte_write_ctrl(CPUState *cpu, uint8_t reg, uint32_t val);
void gte_execute(CPUState *cpu, uint32_t cmd);

#ifdef __cplusplus
}
#endif

#endif
