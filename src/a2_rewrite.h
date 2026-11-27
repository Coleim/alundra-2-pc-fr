/*
 * a2_rewrite.h - common layer for the game functions rewritten in C (phase 3).
 *
 * A rewritten function replaces the original through the framework's func_override
 * (dispatch keyed by guest address), so the game stays playable while functions are
 * replaced one by one, and every replacement can be switched off.
 *
 *     static int heading(CPUState *cpu) { ... cpu->gpr[2] = result; return 1; }
 *     A2_REWRITE(func_800A09C0, 0x800A09C0u, heading, 102, A2_PURE);
 *
 * Conventions for the C body:
 *   - arguments in cpu->gpr[4..7] (and the caller's stack from sp+0x10), result in
 *     cpu->gpr[2] (and gpr[3] for 64-bit), return 1;
 *   - guest memory only through a2_lw/a2_lh/.../a2_sw (MIPS load/store semantics);
 *   - other guest functions only through a2_call*(): the callee may itself be rewritten
 *     or still original, the dispatcher decides;
 *   - no host state that matters for the game (replays and save states only see guest RAM).
 *
 * Timing: the framework counts guest cycles; a replacement charges the cycles the original
 * takes (`cost`, measured with A2_VERIFY), or A2_COST_SELF if the body calls a2_charge().
 *
 * Environment:
 *   A2_OVERRIDES=0          register no replacement (original code everywhere)
 *   A2_DISABLE=f1,f2        leave these functions original (names as in A2_REWRITE)
 *   A2_VERIFY=all|f1,f2     shadow check (A2_PURE functions): the original runs for real,
 *                           then the rewrite runs on the same input, results and RAM writes
 *                           are compared, and the original's state is kept; the original's
 *                           cycle cost is measured. Summary on stderr at exit.
 *   A2_VERIFY_EVERY=n       check one call in n (hot functions)
 */
#ifndef A2_REWRITE_H
#define A2_REWRITE_H

#include <stdint.h>

#ifdef A2_STANDALONE
#include "native/a2_native.h"
#define psx_read_word(a)     a2n_r32(a)
#define psx_read_half(a)     ((uint16_t)a2n_r16(a))
#define psx_read_byte(a)     ((uint8_t)a2n_r8(a))
#define psx_write_word(a, v) a2n_w32((a), (v))
#define psx_write_half(a, v) a2n_w16((a), (v))
#define psx_write_byte(a, v) a2n_w8((a), (v))
#else
#include "cpu_state.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef A2_STANDALONE
uint32_t psx_read_word(uint32_t addr);
uint16_t psx_read_half(uint32_t addr);
uint8_t  psx_read_byte(uint32_t addr);
void     psx_write_word(uint32_t addr, uint32_t val);
void     psx_write_half(uint32_t addr, uint16_t val);
void     psx_write_byte(uint32_t addr, uint8_t val);
#endif

/* Guest memory, with the MIPS extension rules. */
static inline uint32_t a2_lw(uint32_t a)  { return psx_read_word(a); }
static inline int32_t  a2_lh(uint32_t a)  { return (int16_t)psx_read_half(a); }
static inline uint32_t a2_lhu(uint32_t a) { return psx_read_half(a); }
static inline int32_t  a2_lb(uint32_t a)  { return (int8_t)psx_read_byte(a); }
static inline uint32_t a2_lbu(uint32_t a) { return psx_read_byte(a); }
static inline void a2_sw(uint32_t a, uint32_t v) { psx_write_word(a, v); }
static inline void a2_sh(uint32_t a, uint32_t v) { psx_write_half(a, (uint16_t)v); }
static inline void a2_sb(uint32_t a, uint32_t v) { psx_write_byte(a, (uint8_t)v); }

/* Call a guest function. `site` is the address of the original `jal`, so the callee sees
 * the same $ra as in the original (site + 8). Returns $v0. */
uint32_t a2_call(CPUState *cpu, uint32_t site, uint32_t target, int nargs, ...);
#define a2_call0(cpu, site, fn)                 a2_call((cpu), (site), (fn), 0)
#define a2_call1(cpu, site, fn, x)              a2_call((cpu), (site), (fn), 1, (uint32_t)(x))
#define a2_call2(cpu, site, fn, x, y)           a2_call((cpu), (site), (fn), 2, (uint32_t)(x), (uint32_t)(y))
#define a2_call3(cpu, site, fn, x, y, z)        a2_call((cpu), (site), (fn), 3, (uint32_t)(x), (uint32_t)(y), (uint32_t)(z))
#define a2_call4(cpu, site, fn, x, y, z, w)     a2_call((cpu), (site), (fn), 4, (uint32_t)(x), (uint32_t)(y), (uint32_t)(z), (uint32_t)(w))

/* Charge guest cycles (A2_COST_SELF bodies). Ignored while the shadow check runs a body. */
void a2_charge(uint32_t cycles);

/* Overlay code (GAME, EVENT, PACK1 modules) ran in the interpreter, where the instruction
 * cache makes a block cost about three times its span (func_8008B044: 764 cycles for 249 of
 * spans, measured with A2_VERIFY). Rewrites of overlay code charge their path's spans
 * through this estimate. */
static inline void a2_charge_interp(uint32_t spans) { a2_charge(3u * spans); }

#define A2_COST_SELF (-1)

enum {
    A2_PURE  = 1u << 0,  /* reads/writes guest RAM only (callees too): can be shadow-checked */
    A2_VOID  = 1u << 1,  /* returns nothing: $v0 is not compared */
    A2_RET64 = 1u << 2,  /* returns $v0:$v1 */
};

typedef int (*A2Impl)(CPUState *cpu);

typedef struct A2Rewrite {
    const char *name;
    uint32_t addr;
    A2Impl impl;
    int32_t cost;
    uint32_t flags;
    const uint32_t *guard;          /* first words of the original, for overlay code */
    int n_guard;
    int (*tramp)(CPUState *cpu);
    /* statistics */
    uint64_t calls, checked, skipped, mismatches, cyc_sum, cyc_min, cyc_max;
    uint64_t own_sum, own_min, own_max;   /* cycles without the guest calls (exclusive) */
    struct A2Rewrite *next;
    uint64_t cost_mismatches;             /* calls where the rewrite charges other cycles */
} A2Rewrite;

void a2_rewrite_register(A2Rewrite *r);
/* A function replaced by C registered directly with func_override (not A2_REWRITE). */
void a2_rewrite_note(uint32_t addr);
int  a2_rewrite_known(uint32_t addr);
int  a2_rewrite_run(A2Rewrite *r, CPUState *cpu);

#define A2_REWRITE_IMPL(sym, address, impl_fn, cost_, flags_, guard_, n_guard_)                 \
    static A2Rewrite sym##_desc = { #sym, (address), (impl_fn), (cost_), (flags_),              \
                                    (guard_), (n_guard_), 0, 0, 0, 0, 0, 0, 0, 0,             \
                                    0, 0, 0, 0 };                                               \
    static int sym##_tramp(CPUState *cpu) { return a2_rewrite_run(&sym##_desc, cpu); }         \
    __attribute__((constructor)) static void sym##_register(void)                               \
    {                                                                                           \
        sym##_desc.tramp = sym##_tramp;                                                         \
        a2_rewrite_register(&sym##_desc);                                                       \
    }

/* Function of the main executable (always resident). */
#define A2_REWRITE(sym, address, impl_fn, cost_, flags_)                                        \
    A2_REWRITE_IMPL(sym, address, impl_fn, cost_, flags_, 0, 0)

/* Function of an overlay: only replaced while these first words are resident. */
#define A2_REWRITE_OVERLAY(sym, address, impl_fn, cost_, flags_, ...)                           \
    static const uint32_t sym##_guard[] = { __VA_ARGS__ };                                      \
    A2_REWRITE_IMPL(sym, address, impl_fn, cost_, flags_, sym##_guard,                          \
                    (int)(sizeof sym##_guard / sizeof sym##_guard[0]))

#ifdef __cplusplus
}
#endif

#endif
