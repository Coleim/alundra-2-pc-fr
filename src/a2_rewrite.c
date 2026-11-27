/*
 * a2_rewrite.c - registration, timing and shadow check of the rewritten game functions.
 * See a2_rewrite.h.
 */
#include "a2_rewrite.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "func_override.h"
#include "psx_cycles.h"

extern uint8_t *g_psx_ram;
extern uint32_t g_psx_ram_size;

#define KERNEL_END      0x10000u      /* kernel work area (physical), rewritten by interrupts */
#define DEAD_STACK      0x10000u      /* below the entry $sp: frames of the function and its callees */
#define SCRATCH_BASE    0x1F800000u
#define SCRATCH_WORDS   256u
#define MAX_REPORTS     8             /* calls with details printed, per function */
#define MAX_LINES       6             /* details printed per call */

static A2Rewrite *s_list;
static int s_env_read, s_disabled_all, s_verify_all;
static const char *s_disable = "", *s_verify = "";   /* comma lists, straight from the environment */
static uint64_t s_verify_every = 1;
static int s_shadow;                  /* > 0 while a shadow check runs */
static uint64_t s_callee_cycles;      /* guest cycles spent in a2_call during a shadow run */
static int s_atexit;

/* ---- environment ---- */

static void read_env(void)
{
    if (s_env_read) return;
    s_env_read = 1;
    const char *e;
    if ((e = getenv("A2_OVERRIDES")) && e[0] == '0') s_disabled_all = 1;
    if ((e = getenv("A2_DISABLE"))) s_disable = e;
    if ((e = getenv("A2_VERIFY"))) {
        if (!strcmp(e, "all") || !strcmp(e, "1")) s_verify_all = 1;
        else s_verify = e;
    }
    if ((e = getenv("A2_VERIFY_EVERY")) && atoi(e) > 1) s_verify_every = (uint64_t)atoi(e);
}

static int listed(const char *list, const char *name)
{
    const size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)) != NULL; p += n)
        if ((p == list || p[-1] == ',') && (p[n] == ',' || p[n] == 0)) return 1;
    return 0;
}

static int verifying(const A2Rewrite *r)
{
    return (r->flags & A2_PURE) && (s_verify_all || listed(s_verify, r->name));
}

/* ---- summary ---- */

static void print_summary(void)
{
    for (A2Rewrite *r = s_list; r; r = r->next) {
        if (!r->checked && !r->skipped && !r->mismatches) continue;
        fprintf(stderr, "a2.verify %-16s calls=%llu checked=%llu skipped=%llu mismatches=%llu",
                r->name, (unsigned long long)r->calls, (unsigned long long)r->checked,
                (unsigned long long)r->skipped, (unsigned long long)r->mismatches);
        if (r->checked)
            fprintf(stderr, " cycles mean=%.1f min=%llu max=%llu own mean=%.1f min=%llu max=%llu charged=%d"
                    " cost_mismatches=%llu",
                    (double)r->cyc_sum / (double)r->checked, (unsigned long long)r->cyc_min,
                    (unsigned long long)r->cyc_max, (double)r->own_sum / (double)r->checked,
                    (unsigned long long)r->own_min, (unsigned long long)r->own_max, (int)r->cost,
                    (unsigned long long)r->cost_mismatches);
        fputc('\n', stderr);
    }
}

/* ---- registration ---- */

void a2_rewrite_register(A2Rewrite *r)
{
    read_env();
    r->next = s_list;
    s_list = r;
    if (s_disabled_all || listed(s_disable, r->name)) return;
    if (r->n_guard)
        func_override_add_guarded(r->name, r->addr, r->tramp, r->guard, r->n_guard, FO_CREDIT_SELF);
    else
        func_override_add(r->name, r->addr, r->tramp, FO_CREDIT_SELF);
    if (!s_atexit && (s_verify_all || s_verify[0])) {
        s_atexit = 1;
        atexit(print_summary);
    }
}

static uint32_t s_noted[16];
static int s_n_noted;

void a2_rewrite_note(uint32_t addr)
{
    if (s_n_noted < 16) s_noted[s_n_noted++] = addr;
}

int a2_rewrite_known(uint32_t addr)
{
    for (A2Rewrite *r = s_list; r; r = r->next)
        if (r->addr == addr) return 1;
    for (int i = 0; i < s_n_noted; ++i)
        if (s_noted[i] == addr) return 1;
    return 0;
}

/* ---- guest calls and timing ---- */

uint32_t a2_call(CPUState *cpu, uint32_t site, uint32_t target, int nargs, ...)
{
    va_list ap;
    va_start(ap, nargs);
    uint32_t args[8] = { 0 };
    for (int i = 0; i < nargs && i < 8; ++i) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    /* A frame of our own, like the original's: the callee may use the four home words at
     * $sp, and arguments 5 to 8 go at $sp+0x10. */
    const uint32_t frame = 0x20u;
    cpu->gpr[29] -= frame;
    for (int i = 0; i < 4; ++i) cpu->gpr[4 + i] = args[i];
    for (int i = 4; i < nargs && i < 8; ++i) a2_sw(cpu->gpr[29] + 0x10u + 4u * (uint32_t)(i - 4), args[i]);
    const uint64_t t0 = psx_cycle_count + g_psx_cyc_batch;
    func_override_guest_call(cpu, target, site + 8u);
    if (s_shadow) s_callee_cycles += psx_cycle_count + g_psx_cyc_batch - t0;
    cpu->gpr[29] += frame;
    return cpu->gpr[2];
}

static uint64_t s_charged;              /* what the rewrite charged, during a shadow check */

void a2_charge(uint32_t cycles)
{
    if (s_shadow) s_charged += cycles;
    else if (cycles) psx_advance_cycles(cycles);
}

static uint64_t guest_now(void) { return psx_cycle_count + g_psx_cyc_batch; }

/* ---- shadow check ---- */

static uint8_t *s_pre, *s_orig;
static uint32_t s_scr_pre[SCRATCH_WORDS], s_scr_orig[SCRATCH_WORDS];

static void scratch_save(uint32_t *out)
{
    for (uint32_t i = 0; i < SCRATCH_WORDS; ++i) out[i] = psx_read_word(SCRATCH_BASE + 4u * i);
}

static void scratch_load(const uint32_t *in)
{
    for (uint32_t i = 0; i < SCRATCH_WORDS; ++i) psx_write_word(SCRATCH_BASE + 4u * i, in[i]);
}

static void report(A2Rewrite *r, uint64_t before, int *lines, const char *what, uint32_t addr,
                   uint32_t orig, uint32_t ours)
{
    if (r->mismatches == before) r->mismatches++;
    if (r->mismatches <= MAX_REPORTS && (*lines)++ < MAX_LINES)
        fprintf(stderr, "a2.verify %s call %llu: %s %#010x original=%#010x rewrite=%#010x\n", r->name,
                (unsigned long long)r->calls, what, addr, orig, ours);
}

/* Original first (for real), then the rewrite on the same input; compare; keep the original. */
static int shadow_check(A2Rewrite *r, CPUState *cpu)
{
    const uint32_t size = g_psx_ram_size;
    if (!s_pre) {
        s_pre = (uint8_t *)malloc(size);
        s_orig = (uint8_t *)malloc(size);
        if (!s_pre || !s_orig) return 0;
    }
    const CPUState entry = *cpu;
    memcpy(s_pre, g_psx_ram, size);
    scratch_save(s_scr_pre);

    s_shadow++;
    const uint64_t t0 = guest_now();
    func_override_call_original(cpu);
    const uint64_t cycles = guest_now() - t0;
    const CPUState after_orig = *cpu;
    memcpy(s_orig, g_psx_ram, size);
    scratch_save(s_scr_orig);

    /* An interrupt during the original rewrites the kernel area and may touch game RAM:
     * such a call cannot be compared. */
    if (memcmp(s_pre, s_orig, KERNEL_END) != 0) {
        r->skipped++;
        s_shadow--;
        return 1;
    }

    memcpy(g_psx_ram, s_pre, size);
    scratch_load(s_scr_pre);
    *cpu = entry;
    s_callee_cycles = 0;
    s_charged = 0;
    r->impl(cpu);
    s_shadow--;

    /* own cost: the original's cycles minus what the same calls cost in the rewrite's run */
    const uint64_t own = cycles > s_callee_cycles ? cycles - s_callee_cycles : 0;
    const uint64_t charged = r->cost > 0 ? (uint64_t)r->cost : s_charged;
    if (charged != own && r->cost_mismatches++ < MAX_REPORTS)
        fprintf(stderr, "a2.verify %s call %llu: cycles original=%llu rewrite=%llu\n", r->name,
                (unsigned long long)r->calls, (unsigned long long)own, (unsigned long long)charged);
    r->checked++;
    r->cyc_sum += cycles;
    if (!r->cyc_min || cycles < r->cyc_min) r->cyc_min = cycles;
    if (cycles > r->cyc_max) r->cyc_max = cycles;
    r->own_sum += own;
    if (!r->own_min || own < r->own_min) r->own_min = own;
    if (own > r->own_max) r->own_max = own;

    const uint64_t before = r->mismatches;
    int lines = 0;
    /* results, and the registers a function must preserve (s0-s7, gp, sp, fp) */
    static const int regs[] = { 2, 3, 16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30 };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; ++i) {
        const int g = regs[i];
        if ((g == 2 && (r->flags & A2_VOID)) || (g == 3 && !(r->flags & A2_RET64)))
            continue;
        if (cpu->gpr[g] != after_orig.gpr[g])
            report(r, before, &lines, "register", (uint32_t)g, after_orig.gpr[g], cpu->gpr[g]);
    }
    const uint32_t sp = entry.gpr[29] & (size - 1u);
    const uint32_t dead_lo = sp > DEAD_STACK ? sp - DEAD_STACK : 0;
    for (uint32_t off = KERNEL_END; off < size; off += 4) {
        if (off >= dead_lo && off < sp) { off = sp - 4; continue; }
        uint32_t a, b;
        memcpy(&a, s_orig + off, 4);
        memcpy(&b, g_psx_ram + off, 4);
        if (a != b)
            report(r, before, &lines, "RAM", 0x80000000u | off, a, b);
    }
    for (uint32_t i = 0; i < SCRATCH_WORDS; ++i) {
        const uint32_t b = psx_read_word(SCRATCH_BASE + 4u * i);
        if (b != s_scr_orig[i])
            report(r, before, &lines, "scratchpad", SCRATCH_BASE + 4u * i, s_scr_orig[i], b);
    }

    memcpy(g_psx_ram, s_orig, size);
    scratch_load(s_scr_orig);
    *cpu = after_orig;
    return 1;
}

int a2_rewrite_run(A2Rewrite *r, CPUState *cpu)
{
    if (s_shadow)                       /* inside a shadow check: originals only */
        return 0;
    r->calls++;
    if (verifying(r) && (r->calls - 1) % s_verify_every == 0)
        return shadow_check(r, cpu);
    const int handled = r->impl(cpu);
    if (handled && r->cost > 0) psx_advance_cycles((uint32_t)r->cost);
    return handled;
}
