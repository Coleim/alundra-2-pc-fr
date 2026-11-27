/*
 * a2_lift.h - support for the C lifted from the game's MIPS code (tools/a2lift.py).
 *
 * The lifted functions keep the registers in CPUState (named here as in MIPS assembly),
 * reach guest memory and the GTE through the same helpers as the framework's compiled code,
 * and follow its timing model for compiled code: one span charge per basic block
 * (A2_SPAN), an interrupt check at every control transfer (A2_EDGE). Calls go through the
 * guest dispatcher (a2_jal), so the callee may be original, hand-written or lifted.
 *
 * Only lifted files include this header: the register names are macros.
 */
#ifndef A2_LIFT_H
#define A2_LIFT_H

#include <stdint.h>

#include "cpu_state.h"
#include "psx_cycles.h"
#include "psx_cyc.h"
#include "psx_icache.h"
#include "psx_instr_cost.h"
#include "interrupts.h"
#include "func_override.h"
#ifndef A2_LIFT_PRECISE
#include "psx_native_span.h"
#endif

typedef struct {
    uint32_t addr;
    const char *name;
    int (*fn)(CPUState *cpu);
    int n_guard;                   /* first words that must be resident */
    uint32_t guard[4];
    int n_check;                   /* further words that tell apart the variants of a load */
    int32_t check_index[4];        /* address: the entry's, plus 4 * index */
    uint32_t check_word[4];
    int handwritten;               /* also rewritten by hand: used only while that one is disabled */
} A2LiftEntry;

/* runtime (a2_lift_runtime.c) */
void a2_jal(CPUState *cpu, uint32_t target, uint32_t ret);
void a2_tail(CPUState *cpu, uint32_t target);
int a2_return(CPUState *cpu, uint32_t target);
int a2_bad_jump(CPUState *cpu, uint32_t target);
int a2_fell_off(CPUState *cpu);
int a2_break(CPUState *cpu, uint32_t code, uint32_t pc);
int a2_fallthrough(CPUState *cpu, uint32_t next);

/* timing and interrupts, as in the framework's compiled code */
#define A2_ENTER()                                                                         \
    __attribute__((cleanup(psx_cyc_bb_defer_cleanup))) int a2_defer_ = 1;                  \
    (void)a2_defer_;                                                                        \
    psx_cyc_bb_defer_begin()
#define A2_SPAN(n)    psx_native_span_charge(n)
#define A2_SYNC()     psx_load_value_begin(cpu)
#define A2_EDGE(pc)   do { psx_cyc_bb_defer_flush(); psx_check_interrupts_at(cpu, (pc)); } while (0)
#define A2_RETURN(t)  return a2_return(cpu, (t))

#ifdef A2_LIFT_PRECISE
/* Overlay code runs in the interpreter in the original: per-instruction timing (I-cache
 * fetch or step boundary, interlock step, timed loads). */
#define F(pc)   psx_icache_fetch(cpu, (pc))
#define B(pc)   psx_cpu_step_boundary(cpu, (pc))
#define S(w)    psx_cyc_step(cpu, psx_cyc_dep_res_mask(w))
#define A2_MASK(w) psx_cyc_dep_res_mask(w)
#define LW(a, rt, w)   psx_cyc_load_word(cpu, (a), (rt), A2_MASK(w))
#define LHU(a, rt, w)  ((uint32_t)psx_cyc_load_half(cpu, (a), (rt), A2_MASK(w)))
#define LH(a, rt, w)   ((uint32_t)(int32_t)(int16_t)psx_cyc_load_half(cpu, (a), (rt), A2_MASK(w)))
#define LBU(a, rt, w)  ((uint32_t)psx_cyc_load_byte(cpu, (a), (rt), A2_MASK(w)))
#define LB(a, rt, w)   ((uint32_t)(int32_t)(int8_t)psx_cyc_load_byte(cpu, (a), (rt), A2_MASK(w)))
#define A2_LOAD_WORD(a, rt, w) psx_cyc_load_word(cpu, (a), (rt), A2_MASK(w))
#define A2_LWC2_READ(a) psx_cyc_lwc2_read(cpu, (a))
#else
/* guest memory, as the compiled code reads it (timing is in the block's span) */
#define LW(a)   psx_native_span_word(cpu, (a), 0u, 0u)
#define LHU(a)  ((uint32_t)psx_native_span_half(cpu, (a), 0u, 0u))
#define LH(a)   ((uint32_t)(int32_t)(int16_t)psx_native_span_half(cpu, (a), 0u, 0u))
#define LBU(a)  ((uint32_t)psx_native_span_byte(cpu, (a), 0u, 0u))
#define LB(a)   ((uint32_t)(int32_t)(int8_t)psx_native_span_byte(cpu, (a), 0u, 0u))
#define A2_LOAD_WORD(a, rt, w) psx_native_span_word(cpu, (a), 0u, 0u)
#define A2_LWC2_READ(a) psx_native_span_lwc2(cpu, (a))
#endif
#define SW(a, v) (psx_store_cycle_barrier(), cpu->write_word((a), (v)))
#define SH(a, v) (psx_store_cycle_barrier(), cpu->write_half((a), (uint16_t)(v)))
#define SB(a, v) (psx_store_cycle_barrier(), cpu->write_byte((a), (uint8_t)(v)))

static inline uint32_t a2_lwl_word(uint32_t cur, uint32_t addr, uint32_t w)
{
    switch (addr & 3u) {
    case 0: return (cur & 0x00FFFFFFu) | (w << 24);
    case 1: return (cur & 0x0000FFFFu) | (w << 16);
    case 2: return (cur & 0x000000FFu) | (w << 8);
    default: return w;
    }
}
static inline uint32_t a2_lwr_word(uint32_t cur, uint32_t addr, uint32_t w)
{
    switch (addr & 3u) {
    case 0: return w;
    case 1: return (cur & 0xFF000000u) | (w >> 8);
    case 2: return (cur & 0xFFFF0000u) | (w >> 16);
    default: return (cur & 0xFFFFFF00u) | (w >> 24);
    }
}
static inline void a2_swl(CPUState *cpu, uint32_t addr, uint32_t v)
{
    psx_store_cycle_barrier();
    const uint32_t al = addr & ~3u;
    uint32_t w = cpu->read_word(al);
    switch (addr & 3u) {
    case 0: w = (w & 0xFFFFFF00u) | (v >> 24); break;
    case 1: w = (w & 0xFFFF0000u) | (v >> 16); break;
    case 2: w = (w & 0xFF000000u) | (v >> 8); break;
    default: w = v; break;
    }
    cpu->write_word(al, w);
}
static inline void a2_swr(CPUState *cpu, uint32_t addr, uint32_t v)
{
    psx_store_cycle_barrier();
    const uint32_t al = addr & ~3u;
    uint32_t w = cpu->read_word(al);
    switch (addr & 3u) {
    case 0: w = v; break;
    case 1: w = (w & 0x000000FFu) | (v << 8); break;
    case 2: w = (w & 0x0000FFFFu) | (v << 16); break;
    default: w = (w & 0x00FFFFFFu) | (v << 24); break;
    }
    cpu->write_word(al, w);
}
#ifdef A2_LIFT_PRECISE
#define LWL(a, cur, rt, w) a2_lwl_word((cur), (a), A2_LOAD_WORD((a) & ~3u, (rt), (w)))
#define LWR(a, cur, rt, w) a2_lwr_word((cur), (a), A2_LOAD_WORD((a) & ~3u, (rt), (w)))
#else
#define LWL(a, cur, rt) a2_lwl_word((cur), (a), A2_LOAD_WORD((a) & ~3u, 0u, 0u))
#define LWR(a, cur, rt) a2_lwr_word((cur), (a), A2_LOAD_WORD((a) & ~3u, 0u, 0u))
#endif
#define SWL(a, v)       a2_swl(cpu, (a), (v))
#define SWR(a, v)       a2_swr(cpu, (a), (v))

/* multiply / divide (results in HI/LO, completion deadline as in compiled code) */
#define MULDIV_STALL()  psx_muldiv_stall(cpu)
#define MULT(x, y) do {                                                                     \
        const uint32_t x_ = (x), y_ = (y);                                                  \
        const int64_t p_ = (int64_t)(int32_t)x_ * (int64_t)(int32_t)y_;                     \
        cpu->lo = (uint32_t)p_; cpu->hi = (uint32_t)((uint64_t)p_ >> 32);                   \
        psx_muldiv_set(cpu, psx_mult_latency_s(x_)); } while (0)
#define MULTU(x, y) do {                                                                    \
        const uint32_t x_ = (x), y_ = (y);                                                  \
        const uint64_t p_ = (uint64_t)x_ * (uint64_t)y_;                                    \
        cpu->lo = (uint32_t)p_; cpu->hi = (uint32_t)(p_ >> 32);                             \
        psx_muldiv_set(cpu, psx_mult_latency_u(x_)); } while (0)
#define DIV(x, y) do {                                                                      \
        const uint32_t x_ = (x), y_ = (y);                                                  \
        if (y_ == 0) { cpu->lo = (int32_t)x_ >= 0 ? 0xFFFFFFFFu : 1u; cpu->hi = x_; }       \
        else if (x_ == 0x80000000u && y_ == 0xFFFFFFFFu) { cpu->lo = x_; cpu->hi = 0; }     \
        else { cpu->lo = (uint32_t)((int32_t)x_ / (int32_t)y_);                             \
               cpu->hi = (uint32_t)((int32_t)x_ % (int32_t)y_); }                           \
        psx_muldiv_set(cpu, 37u); } while (0)
#define DIVU(x, y) do {                                                                     \
        const uint32_t x_ = (x), y_ = (y);                                                  \
        if (y_ == 0) { cpu->lo = 0xFFFFFFFFu; cpu->hi = x_; }                               \
        else { cpu->lo = x_ / y_; cpu->hi = x_ % y_; }                                      \
        psx_muldiv_set(cpu, 37u); } while (0)

/* GTE (COP2) */
uint32_t gte_read_data(CPUState *cpu, uint8_t reg);
void gte_write_data(CPUState *cpu, uint8_t reg, uint32_t val);
uint32_t gte_read_ctrl(CPUState *cpu, uint8_t reg);
void gte_write_ctrl(CPUState *cpu, uint8_t reg, uint32_t val);
#define GTE_STALL()        psx_gte_stall(cpu)
#define GTE_READ(rt)       psx_gte_read(cpu, (rt))
#define GTE_MFC2(rd)       gte_read_data(cpu, (rd))
#define GTE_CFC2(rd)       gte_read_ctrl(cpu, (rd))
#define GTE_MTC2(rd, v)    (psx_gte_stall(cpu), gte_write_data(cpu, (rd), (v)))
#define GTE_CTC2(rd, v)    (psx_gte_stall(cpu), gte_write_ctrl(cpu, (rd), (v)))
#define GTE_CMD(c)         gte_execute(cpu, (c))
#define LWC2(rt, a)        do { psx_gte_stall(cpu); cpu->gte_data[rt] = A2_LWC2_READ(a); } while (0)
#define LWC2_HELPER(rt, a) do { psx_gte_stall(cpu); gte_write_data(cpu, (rt), A2_LWC2_READ(a)); } while (0)
#define SWC2(rt, a)        do { psx_gte_stall(cpu); const uint32_t a_ = (a), v_ = cpu->gte_data[rt];           \
                                psx_store_cycle_barrier(); cpu->write_word(a_, v_);                         \
                                gte_precision_store_word(a_, (rt)); } while (0)
#define SWC2_HELPER(rt, a) do { psx_gte_stall(cpu); const uint32_t a_ = (a), v_ = gte_read_data(cpu, (rt));    \
                                psx_store_cycle_barrier(); cpu->write_word(a_, v_);                         \
                                gte_precision_store_word(a_, (rt)); } while (0)

/* registers */
#define at (cpu->gpr[1])
#define v0 (cpu->gpr[2])
#define v1 (cpu->gpr[3])
#define a0 (cpu->gpr[4])
#define a1 (cpu->gpr[5])
#define a2 (cpu->gpr[6])
#define a3 (cpu->gpr[7])
#define t0 (cpu->gpr[8])
#define t1 (cpu->gpr[9])
#define t2 (cpu->gpr[10])
#define t3 (cpu->gpr[11])
#define t4 (cpu->gpr[12])
#define t5 (cpu->gpr[13])
#define t6 (cpu->gpr[14])
#define t7 (cpu->gpr[15])
#define s0 (cpu->gpr[16])
#define s1 (cpu->gpr[17])
#define s2 (cpu->gpr[18])
#define s3 (cpu->gpr[19])
#define s4 (cpu->gpr[20])
#define s5 (cpu->gpr[21])
#define s6 (cpu->gpr[22])
#define s7 (cpu->gpr[23])
#define t8 (cpu->gpr[24])
#define t9 (cpu->gpr[25])
#define k0 (cpu->gpr[26])
#define k1 (cpu->gpr[27])
#define gp (cpu->gpr[28])
#define sp (cpu->gpr[29])
#define fp (cpu->gpr[30])
#define ra (cpu->gpr[31])

#endif
