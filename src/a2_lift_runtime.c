/*
 * a2_lift_runtime.c - registration and calls of the lifted functions (see a2_lift.h).
 *
 * Several overlay modules share load addresses (GAME00-03 and EVENT00 at 0x80084ED0, the
 * PACK1 code modules at 0x80149AE8): one override is registered per address, and the call
 * runs the lifted function whose first words are resident, or the original code if none is.
 *
 * Environment: A2_OVERRIDES=0 or A2_LIFTED=0 registers nothing; A2_LIFTED=main only the main
 * executable's functions; A2_DISABLE=name,... leaves those functions original.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a2_lift.h"
#include "native/gte.h"

extern const A2LiftEntry *const a2_lift_tables[];
extern uint32_t psx_peek_word_untraced(uint32_t addr);

#define SLOTS 8192u                     /* power of two, > 2x the lifted functions */

typedef struct Candidate {
    const A2LiftEntry *e;
    struct Candidate *next;
} Candidate;

static uint32_t s_addr[SLOTS];
static Candidate *s_cand[SLOTS];
static uint64_t s_calls, s_declined;

static uint32_t slot_of(uint32_t a) { return ((a >> 2) * 2654435761u) & (SLOTS - 1u); }

static Candidate **lookup(uint32_t a, int create)
{
    for (uint32_t k = slot_of(a);; k = (k + 1u) & (SLOTS - 1u)) {
        if (s_addr[k] == a) return &s_cand[k];
        if (!s_addr[k]) {
            if (!create) return NULL;
            s_addr[k] = a;
            return &s_cand[k];
        }
    }
}

static int resident(const A2LiftEntry *e)
{
    for (int i = 0; i < e->n_guard; ++i)
        if (psx_peek_word_untraced(e->addr + 4u * (uint32_t)i) != e->guard[i]) return 0;
    for (int i = 0; i < e->n_check; ++i)
        if (psx_peek_word_untraced(e->addr + (uint32_t)(4 * e->check_index[i])) != e->check_word[i]) return 0;
    return 1;
}

/* ---- A2_LIFT_VERIFY=f1,f2: run the original for real, then the lifted function on the same
 * state; compare registers, RAM and guest cycles; keep the original's results. */
extern uint8_t *g_psx_ram;
extern uint32_t g_psx_ram_size;
static const char *s_verify_list;
static int s_in_verify;
static uint8_t *s_pre, *s_orig;
static uint64_t s_checked, s_bad, s_cyc_bad;

static uint64_t now(void) { return psx_cycle_count + g_psx_cyc_batch; }

static int listed(const char *list, const char *name);

static int lift_verify(const A2LiftEntry *e, CPUState *cpu)
{
    const uint32_t size = g_psx_ram_size;
    if (!s_pre && (!(s_pre = malloc(size)) || !(s_orig = malloc(size)))) return e->fn(cpu);
    const CPUState entry = *cpu;
    memcpy(s_pre, g_psx_ram, size);
    s_in_verify++;
    uint64_t tb = now();
    func_override_call_original(cpu);
    const uint64_t c_orig = now() - tb;
    const CPUState after = *cpu;
    memcpy(s_orig, g_psx_ram, size);
    if (memcmp(s_pre, s_orig, 0x10000) != 0) { s_in_verify--; return 1; }   /* interrupted */
    memcpy(g_psx_ram, s_pre, size);
    *cpu = entry;
    tb = now();
    e->fn(cpu);
    const uint64_t c_lift = now() - tb;
    s_in_verify--;
    s_checked++;
    int lines = 0;
    const uint32_t spr = entry.gpr[29] & (size - 1u);
    for (int g = 1; g < 32; ++g) {
        if (g == 26 || g == 27) continue;
        if (cpu->gpr[g] != after.gpr[g] && lines++ < 4)
            fprintf(stderr, "a2.lift.verify %s: reg %d original=%#010x lifted=%#010x\n", e->name, g,
                    after.gpr[g], cpu->gpr[g]);
    }
    for (uint32_t off = 0x10000; off < size; off += 4) {
        if (off >= (spr > 0x10000u ? spr - 0x10000u : 0) && off < spr) { off = spr - 4; continue; }
        uint32_t x, y;
        memcpy(&x, s_orig + off, 4);
        memcpy(&y, g_psx_ram + off, 4);
        if (x != y && lines++ < 8)
            fprintf(stderr, "a2.lift.verify %s: RAM %#010x original=%#010x lifted=%#010x\n", e->name,
                    0x80000000u | off, x, y);
    }
    if (lines) s_bad++;
    if (c_orig != c_lift) {
        if (s_cyc_bad++ < 20)
            fprintf(stderr, "a2.lift.verify %s: cycles original=%llu lifted=%llu\n", e->name,
                    (unsigned long long)c_orig, (unsigned long long)c_lift);
    }
    memcpy(g_psx_ram, s_orig, size);
    *cpu = after;
    return 1;
}

static void verify_summary(void)
{
    fprintf(stderr, "a2.lift.verify: %llu calls checked, %llu with differences, %llu with other cycles"
            " (lifted calls %llu, declined %llu)\n",
            (unsigned long long)s_checked, (unsigned long long)s_bad, (unsigned long long)s_cyc_bad,
            (unsigned long long)s_calls, (unsigned long long)s_declined);
}

static int lift_dispatch(CPUState *cpu)
{
    if (s_in_verify) return 0;                 /* callees of a checked call run original */
    const uint32_t a = func_override_active_address();
    Candidate **c = lookup(a, 0);
    for (Candidate *p = c ? *c : NULL; p; p = p->next) {
        if (resident(p->e)) {
            s_calls++;
            if (s_verify_list && listed(s_verify_list, p->e->name)) return lift_verify(p->e, cpu);
            return p->e->fn(cpu);
        }
    }
    s_declined++;
    return 0;
}

static int listed(const char *list, const char *name)
{
    if (!list) return 0;
    const size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)) != NULL; p += n)
        if ((p == list || p[-1] == ',') && (p[n] == ',' || p[n] == 0)) return 1;
    return 0;
}

/* ---- A2_LIFT_COVERAGE=1: report the call targets of lifted code that are neither lifted,
 * hand-written, library nor kernel code: game code still running as the original. */
int a2_rewrite_known(uint32_t addr);
static int s_coverage;
static struct { uint32_t target, from; uint64_t n; } s_uncovered[256];
static int s_n_uncovered;

static void coverage_report(void)
{
    fprintf(stderr, "a2.lift.coverage: %d call targets not in C\n", s_n_uncovered);
    for (int i = 0; i < s_n_uncovered; ++i)
        fprintf(stderr, "a2.lift.coverage: %#010x x%llu (first from %#010x)\n", s_uncovered[i].target,
                (unsigned long long)s_uncovered[i].n, s_uncovered[i].from);
}

static void coverage_check(uint32_t target, uint32_t from)
{
    const uint32_t a = target & 0x1FFFFFFFu;
    if (a < 0x00010000u || a >= 0x1F000000u) return;                 /* kernel, BIOS */
    if (a >= 0x00031228u && a < 0x0005D800u) return;                 /* PsyQ library */
    if (a2_rewrite_known(0x80000000u | a)) return;
    Candidate **c = lookup(0x80000000u | a, 0);
    for (Candidate *p = c ? *c : NULL; p; p = p->next)
        if (resident(p->e)) return;
    for (int i = 0; i < s_n_uncovered; ++i)
        if (s_uncovered[i].target == target) { s_uncovered[i].n++; return; }
    if (s_n_uncovered < 256) {
        s_uncovered[s_n_uncovered].target = target;
        s_uncovered[s_n_uncovered].from = from;
        s_uncovered[s_n_uncovered++].n = 1;
    }
}

__attribute__((constructor))
static void a2_lift_register(void)
{
    const char *e;
    if ((e = getenv("A2_OVERRIDES")) && e[0] == '0') return;
    if ((e = getenv("A2_LIFTED")) && e[0] == '0') return;
    const int main_only = e && !strcmp(e, "main");
    const char *disable = getenv("A2_DISABLE");
    const char *ll = getenv("A2_LIFT_LIB");
    const int lift_lib = ll && ll[0] == '1';     /* the library's translation (standalone engine) */
    if ((e = getenv("A2_LIFT_COVERAGE")) && e[0] == '1') {
        extern void (*s_stop_report)(void);
        s_coverage = 1;
        atexit(coverage_report);
        s_stop_report = coverage_report;
    }
    if ((s_verify_list = getenv("A2_LIFT_VERIFY"))) {
        extern void (*s_stop_report)(void);
        atexit(verify_summary);
        s_stop_report = verify_summary;
    }
    for (const A2LiftEntry *const *t = a2_lift_tables; *t; ++t) {
        for (const A2LiftEntry *x = *t; x->addr; ++x) {
            if (listed(disable, x->name) != x->handwritten) continue;   /* the hand-written one runs */
            if (main_only && x->addr >= 0x80060000u) continue;   /* overlays load from 0x80084ED0 */
            if (x->addr >= 0x80031228u && x->addr < 0x8005D800u && !lift_lib) continue;   /* PsyQ library */
            Candidate **c = lookup(x->addr, 1);
            if (!*c && func_override_add(x->name, x->addr, lift_dispatch, FO_CREDIT_SELF) != FO_OK)
                continue;                   /* address already owned (hand-written rewrite) */
            Candidate *n = (Candidate *)malloc(sizeof *n);
            if (!n) return;
            n->e = x;
            n->next = *c;
            *c = n;
        }
    }
}

void a2_jal(CPUState *cpu, uint32_t target, uint32_t ret)
{
    if (s_coverage) coverage_check(target, ret - 8u);
    cpu->gpr[31] = ret;
    func_override_guest_call(cpu, target, ret);
    cpu->gpr[31] = ret;
}

void a2_tail(CPUState *cpu, uint32_t target)
{
    if (s_coverage) coverage_check(target, 0);
    func_override_guest_call(cpu, target, cpu->gpr[31]);
}

int a2_return(CPUState *cpu, uint32_t target)
{
    if (target != cpu->gpr[31]) {
        fprintf(stderr, "a2.lift: return to %#010x but $ra is %#010x\n", target, cpu->gpr[31]);
        abort();
    }
    return 1;
}

int a2_bad_jump(CPUState *cpu, uint32_t target)
{
    fprintf(stderr, "a2.lift: jump to %#010x not in the jump table (pc %#010x)\n", target, cpu->pc);
    abort();
}

int a2_fell_off(CPUState *cpu)
{
    fprintf(stderr, "a2.lift: ran past the end of a function (ra %#010x)\n", cpu->gpr[31]);
    abort();
}

int a2_break(CPUState *cpu, uint32_t code, uint32_t pc)
{
    (void)cpu;
    fprintf(stderr, "a2.lift: break %#x at %#010x (division by zero or game assertion)\n", code, pc);
    abort();
}

/* The original runs past its last instruction into the next function, with the same $ra. */
int a2_fallthrough(CPUState *cpu, uint32_t next)
{
    func_override_guest_call(cpu, next, cpu->gpr[31]);
    return 1;
}

/* ---- A2_GTE_CHECK=1: run every GTE command of the translated code also on src/native/gte.c,
 * from the same registers, and compare all 64 registers as the CPU reads them. */
static int s_gte_check = -1;
static uint64_t s_gte_cmds, s_gte_bad;

static void gte_check_report(void)
{
    fprintf(stderr, "a2.gte: %llu commands compared, %llu different\n", (unsigned long long)s_gte_cmds,
            (unsigned long long)s_gte_bad);
}

void a2_gte_cmd(CPUState *cpu, uint32_t cmd)
{
    if (s_gte_check < 0) {
        const char *e = getenv("A2_GTE_CHECK");
        s_gte_check = e && e[0] == '1';
        if (s_gte_check) {
            extern void (*s_stop_report)(void);
            atexit(gte_check_report);
            if (!s_stop_report) s_stop_report = gte_check_report;
        }
    }
    if (!s_gte_check) { gte_execute(cpu, cmd); return; }
    uint32_t d[32] = { 0 }, c[32] = { 0 };
    for (int r = 0; r < 32; ++r) {
        if (r == 15 || r == 28 || r == 29 || r == 31) continue;
        a2gte_write_data(d, r, gte_read_data(cpu, (uint8_t)r));
    }
    for (int r = 0; r < 32; ++r) a2gte_write_ctrl(c, r, gte_read_ctrl(cpu, (uint8_t)r));
    uint32_t in_d[32], in_c[32];
    memcpy(in_d, d, sizeof d);
    memcpy(in_c, c, sizeof c);
    gte_execute(cpu, cmd);
    a2gte_execute(d, c, cmd);
    s_gte_cmds++;
    int bad = 0;
    for (int r = 0; r < 32; ++r) {
        const uint32_t x = gte_read_data(cpu, (uint8_t)r), y = a2gte_read_data(d, r);
        if (x != y && bad++ < 6 && s_gte_bad < 20)
            fprintf(stderr, "a2.gte cmd %08x: data %d framework=%08x native=%08x\n", cmd, r, x, y);
    }
    for (int r = 0; r < 32; ++r) {
        const uint32_t x = gte_read_ctrl(cpu, (uint8_t)r), y = a2gte_read_ctrl(c, r);
        if (x != y && bad++ < 6 && s_gte_bad < 20)
            fprintf(stderr, "a2.gte cmd %08x: ctrl %d framework=%08x native=%08x\n", cmd, r, x, y);
    }
    if (bad) {
        if (s_gte_bad < 20) {
            fprintf(stderr, "a2.gte   input data:");
            for (int r = 0; r < 32; ++r) fprintf(stderr, " %08x", in_d[r]);
            fprintf(stderr, "\na2.gte   input ctrl:");
            for (int r = 0; r < 32; ++r) fprintf(stderr, " %08x", in_c[r]);
            fprintf(stderr, "\n");
        }
        s_gte_bad++;
    }
}
