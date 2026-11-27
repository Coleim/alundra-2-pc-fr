/*
 * engine.c - runs the game's C code without a console simulation.
 *
 * Guest memory (RAM, scratchpad), the table of the game's functions (translated, rewritten
 * by hand, or native replacements of the PsyQ library), calls between them, interrupts
 * delivered at the translated code's control transfers, and the boot of the executable.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "gte.h"
#include "native_modules.h"
#include "../a2_lift.h"
#include "../a2_rewrite.h"

uint8_t a2n_ram[A2N_RAM_SIZE];
uint8_t a2n_scratch[A2N_SCRATCH_SIZE];
volatile int a2n_pending;
int32_t a2n_budget = 0x7FFFFFFF;
CPUState a2n_cpu;

extern const A2LiftEntry *const a2_lift_tables[];

/* ---------------------------------------------------------------- diagnostics */

void a2n_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "a2: fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
    a2n_host_fatal_hook();
    exit(1);
}

static int s_log_io = -1;

uint32_t a2n_io_read(uint32_t phys, int size)
{
    const A2nIo *io = a2n_io_find(phys);
    if (io && io->read) return io->read(phys, size);
    if (phys >= 0x1FC00000u && phys < 0x1FC80000u) return 0;          /* no BIOS ROM */
    if (s_log_io < 0) s_log_io = getenv("A2_LOG_IO") != NULL;
    if (s_log_io) fprintf(stderr, "a2: read%d %08x (pc near %08x)\n", size * 8, phys, a2n_cpu.gpr[31]);
    return 0;
}

void a2n_io_write(uint32_t phys, uint32_t value, int size)
{
    const A2nIo *io = a2n_io_find(phys);
    if (io && io->write) { io->write(phys, value, size); return; }
    if (phys == 0x1FFE0130u || (phys & 0xFFFF0000u) == 0x1FFE0000u) return;   /* cache control */
    if (s_log_io < 0) s_log_io = getenv("A2_LOG_IO") != NULL;
    if (s_log_io) fprintf(stderr, "a2: write%d %08x = %08x\n", size * 8, phys, value);
}

/* I/O regions served by native modules (sound chip, ...) */
static A2nIo s_io[16];
static int s_n_io;

void a2n_io_register(uint32_t lo, uint32_t hi, uint32_t (*rd)(uint32_t, int), void (*wr)(uint32_t, uint32_t, int))
{
    if (s_n_io < 16) s_io[s_n_io++] = (A2nIo){ lo, hi, rd, wr };
}

const A2nIo *a2n_io_find(uint32_t phys)
{
    for (int i = 0; i < s_n_io; ++i)
        if (phys >= s_io[i].lo && phys < s_io[i].hi) return &s_io[i];
    return NULL;
}

/* diagnostics: a device about to write guest memory where the executable's code and
 * constants are (nothing legitimate does) */
void a2n_check_write(uint32_t addr, uint32_t bytes, const char *what)
{
    const uint32_t lo = addr & 0x1FFFFFu, hi = lo + bytes;
    if (hi > 0x17000u && lo < 0x31228u) {
        fprintf(stderr, "a2: %s writes %08x..%08x over the executable\n", what, addr, addr + bytes);
        a2n_print_stack();
    }
}

#ifdef A2N_WATCH
uint32_t a2n_watch_lo = 0xFFFFFFFFu, a2n_watch_hi;
__attribute__((constructor)) static void watch_init(void)
{
    const char *e = getenv("A2_WATCH");          /* A2_WATCH=addr,bytes */
    if (e) {
        unsigned a = 0, n = 4;
        sscanf(e, "%x,%u", &a, &n);
        a2n_watch_lo = a & 0x1FFFFFu;
        a2n_watch_hi = a2n_watch_lo + n;
    }
}

void a2n_watch_hit(uint32_t addr, uint32_t value, int size)
{
    static int n;
    if (n++ > 40) return;
    fprintf(stderr, "a2: store%d %08x = %08x over the executable (pc near ra %08x)\n", size * 8, addr, value, a2n_cpu.gpr[31]);
    a2n_print_stack();
}
#endif

/* ---------------------------------------------------------------- function table */

static int listed(const char *list, const char *name);

enum { K_NATIVE, K_HAND, K_LIFTED };

typedef struct Cand {
    int kind;
    const char *name;
    int (*fn)(CPUState *);
    const A2LiftEntry *lift;              /* residency guard (lifted) */
    const uint32_t *guard;                /* residency guard (hand-written overlay code) */
    int n_guard;
    uint64_t *count;                      /* A2_TRACE_NATIVE */
    struct Cand *next;
} Cand;

#define SLOTS 16384u
static uint32_t s_addr[SLOTS];
static Cand *s_cand[SLOTS];

static uint32_t key_of(uint32_t a)
{
    const uint32_t p = a & 0x1FFFFFFFu;
    return p < 0x00800000u ? (0x80000000u | (p & 0x1FFFFFu)) : a;
}

static Cand **slot(uint32_t a, int create)
{
    const uint32_t k = key_of(a);
    for (uint32_t i = ((k >> 2) * 2654435761u) & (SLOTS - 1u);; i = (i + 1u) & (SLOTS - 1u)) {
        if (s_addr[i] == k) return &s_cand[i];
        if (!s_addr[i]) {
            if (!create) return NULL;
            s_addr[i] = k;
            return &s_cand[i];
        }
    }
}

static void add_cand(uint32_t addr, Cand *c)
{
    Cand **head = slot(addr, 1);
    /* keep the order native, hand-written, lifted */
    while (*head && (*head)->kind <= c->kind) head = &(*head)->next;
    c->next = *head;
    *head = c;
}

/* A2_TRACE_NATIVE=1: count the calls of each native function (stderr at exit) */
typedef struct { const char *name; uint64_t calls; } NCount;
static NCount s_ncount[256];
static int s_n_ncount, s_trace_native = -1;

static void native_report(void)
{
    for (int i = 0; i < s_n_ncount; ++i)
        if (s_ncount[i].calls) fprintf(stderr, "a2.native %-22s %llu\n", s_ncount[i].name, (unsigned long long)s_ncount[i].calls);
}

void a2n_native(uint32_t addr, const char *name, int (*fn)(CPUState *))
{
    Cand *c = calloc(1, sizeof *c);
    if (s_trace_native < 0) {
        s_trace_native = getenv("A2_TRACE_NATIVE") != NULL;
        if (s_trace_native) atexit(native_report);
    }
    if (s_trace_native && s_n_ncount < 256) {
        s_ncount[s_n_ncount].name = name;
        c->count = &s_ncount[s_n_ncount++].calls;
    }
    c->kind = K_NATIVE;
    c->name = name;
    c->fn = fn;
    add_cand(addr, c);
}

static const char *s_disable;

static int listed(const char *list, const char *name)
{
    if (!list || !name) return 0;
    const size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)) != NULL; p += n)
        if ((p == list || p[-1] == ',') && (p[n] == ',' || p[n] == 0)) return 1;
    return 0;
}

/* A2_REWRITE functions register here (see a2_rewrite.h) */
void a2_rewrite_register(A2Rewrite *r)
{
    if (!s_disable) s_disable = getenv("A2_DISABLE") ? getenv("A2_DISABLE") : "";
    if (listed(s_disable, r->name)) return;
    Cand *c = calloc(1, sizeof *c);
    c->kind = K_HAND;
    c->name = r->name;
    c->fn = r->impl;
    c->guard = r->guard;
    c->n_guard = r->n_guard;
    add_cand(r->addr, c);
}

int a2_rewrite_run(A2Rewrite *r, CPUState *cpu) { return r->impl(cpu); }
void a2_rewrite_note(uint32_t addr) { (void)addr; }
int a2_rewrite_known(uint32_t addr) { Cand **c = slot(addr, 0); return c && *c; }
void a2_charge(uint32_t cycles) { (void)cycles; }

static void register_lifted(void)
{
    for (const A2LiftEntry *const *t = a2_lift_tables; *t; ++t) {
        for (const A2LiftEntry *x = *t; x->addr; ++x) {
            Cand *c = calloc(1, sizeof *c);
            c->kind = K_LIFTED;
            c->name = x->name;
            c->fn = x->fn;
            c->lift = x;
            add_cand(x->addr, c);
        }
    }
}

static int resident(const Cand *c, uint32_t addr)
{
    if (c->lift) {
        const A2LiftEntry *e = c->lift;
        for (int i = 0; i < e->n_guard; ++i)
            if (a2n_r32(e->addr + 4u * (uint32_t)i) != e->guard[i]) return 0;
        for (int i = 0; i < e->n_check; ++i)
            if (a2n_r32(e->addr + (uint32_t)(4 * e->check_index[i])) != e->check_word[i]) return 0;
        return 1;
    }
    for (int i = 0; i < c->n_guard; ++i)
        if (a2n_r32(addr + 4u * (uint32_t)i) != c->guard[i]) return 0;
    return 1;
}

const char *a2n_name_of(uint32_t addr)
{
    Cand **c = slot(addr, 0);
    return c && *c ? (*c)->name : "?";
}

/* the guest functions running (diagnostics: where a stuck game is) */
#define MAX_DEPTH 1024
static uint32_t s_stack[MAX_DEPTH];
static volatile int s_depth;

void a2n_print_stack(void)
{
    const int d = s_depth;
    fprintf(stderr, "a2: guest call stack (%d):", d);
    for (int i = d > 24 ? d - 24 : 0; i < d && i < MAX_DEPTH; ++i)
        fprintf(stderr, " %08x(%s)", s_stack[i], a2n_name_of(s_stack[i]));
    fprintf(stderr, "\n");
}

/* A2_TRACE_FN=name1,name2: print the calls of these functions (arguments, return value) */
static const char *s_trace_fn;
static int s_trace_init;

static int traced(Cand *c)
{
    if (!s_trace_init) {
        s_trace_init = 1;
        s_trace_fn = getenv("A2_TRACE_FN");
    }
    return s_trace_fn && c && listed(s_trace_fn, c->name);
}

uint64_t a2n_vblank_count(void);

/* run the function at `target` (registers and $ra already set by the caller) */
void a2n_dispatch(CPUState *cpu, uint32_t target)
{
    Cand **head = slot(target, 0);
    const int d = s_depth;
    if (head && traced(*head)) {
        fprintf(stderr, "a2.call %s(%08x, %08x, %08x, %08x) ra %08x vblank %llu\n", (*head)->name, cpu->gpr[4],
                cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[31], (unsigned long long)a2n_vblank_count());
    }
    if (d < MAX_DEPTH) s_stack[d] = target;
    s_depth = d + 1;
    for (Cand *c = head ? *head : NULL; c; c = c->next) {
        if (!resident(c, target)) continue;
        if (c->count) ++*c->count;
        if (c->fn(cpu)) {
            s_depth = d;
            if (s_trace_fn && traced(*head)) fprintf(stderr, "a2.ret  %s -> %08x\n", (*head)->name, cpu->gpr[2]);
            return;
        }
    }
    s_depth = d;
    if ((target & 0x1FFFFFFFu) == 0xA0u || (target & 0x1FFFFFFFu) == 0xB0u || (target & 0x1FFFFFFFu) == 0xC0u) {
        a2n_bios_call(cpu, target & 0xFFu);
        return;
    }
    a2n_fatal("no code at %08x (called with $ra %08x, a0 %08x)", target, cpu->gpr[31], cpu->gpr[4]);
}

/* ---- the translated code's calls (a2_lift.h) ---- */

void a2_jal(CPUState *cpu, uint32_t target, uint32_t ret)
{
    cpu->gpr[31] = ret;
    a2n_dispatch(cpu, target);
    cpu->gpr[31] = ret;
}

void a2_tail(CPUState *cpu, uint32_t target)
{
    a2n_dispatch(cpu, target);
}

int a2_return(CPUState *cpu, uint32_t target)
{
    if (target != cpu->gpr[31])
        a2n_fatal("return to %08x but $ra is %08x", target, cpu->gpr[31]);
    return 1;
}

int a2_bad_jump(CPUState *cpu, uint32_t target)
{
    a2n_fatal("jump to %08x not in the jump table ($ra %08x)", target, cpu->gpr[31]);
    return 0;
}

int a2_fell_off(CPUState *cpu)
{
    a2n_fatal("ran past the end of a function ($ra %08x)", cpu->gpr[31]);
    return 0;
}

int a2_break(CPUState *cpu, uint32_t code, uint32_t pc)
{
    (void)cpu;
    if (code >= 0x100 && code <= 0x107) return 1;     /* SN debugger hooks: no debugger */
    a2n_fatal("break %#x at %08x (division by zero or game assertion)", code, pc);
    return 0;
}

int a2_fallthrough(CPUState *cpu, uint32_t next)
{
    a2n_dispatch(cpu, next);
    return 1;
}

/* ---- calls from the hand-written game code (a2_rewrite.h) ---- */

uint32_t a2_call(CPUState *cpu, uint32_t site, uint32_t target, int nargs, ...)
{
    va_list ap;
    va_start(ap, nargs);
    uint32_t args[8] = { 0 };
    for (int i = 0; i < nargs && i < 8; ++i) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    /* a frame like the original's: four home words at $sp, arguments 5 to 8 above them */
    cpu->gpr[29] -= 0x20u;
    for (int i = 0; i < 4; ++i) cpu->gpr[4 + i] = args[i];
    for (int i = 4; i < nargs && i < 8; ++i) a2n_w32(cpu->gpr[29] + 0x10u + 4u * (uint32_t)(i - 4), args[i]);
    cpu->gpr[31] = site + 8u;
    a2n_dispatch(cpu, target);
    cpu->gpr[29] += 0x20u;
    return cpu->gpr[2];
}

/* ---- the geometry coprocessor of the translated code (gte.c holds the registers' rules) ---- */

uint32_t gte_read_data(CPUState *cpu, uint8_t reg) { return a2gte_read_data(cpu->gte_data, reg); }
void gte_write_data(CPUState *cpu, uint8_t reg, uint32_t val) { a2gte_write_data(cpu->gte_data, reg, val); }
uint32_t gte_read_ctrl(CPUState *cpu, uint8_t reg) { return a2gte_read_ctrl(cpu->gte_ctrl, reg); }
void gte_write_ctrl(CPUState *cpu, uint8_t reg, uint32_t val) { a2gte_write_ctrl(cpu->gte_ctrl, reg, val); }
void gte_execute(CPUState *cpu, uint32_t cmd) { a2gte_execute(cpu->gte_data, cpu->gte_ctrl, cmd); }

/* ---- calls into the game from native code ---- */

#define RETURN_MAGIC 0x80000F00u

uint32_t a2n_call(CPUState *cpu, uint32_t fn, int nargs, ...)
{
    va_list ap;
    va_start(ap, nargs);
    uint32_t args[8] = { 0 };
    for (int i = 0; i < nargs && i < 8; ++i) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    const uint32_t saved_ra = cpu->gpr[31], saved_sp = cpu->gpr[29];
    cpu->gpr[29] = (cpu->gpr[29] - 0x20u) & ~7u;
    for (int i = 0; i < 4; ++i) cpu->gpr[4 + i] = args[i];
    for (int i = 4; i < nargs && i < 8; ++i) a2n_w32(cpu->gpr[29] + 0x10u + 4u * (uint32_t)(i - 4), args[i]);
    cpu->gpr[31] = RETURN_MAGIC;
    a2n_dispatch(cpu, fn);
    cpu->gpr[29] = saved_sp;
    cpu->gpr[31] = saved_ra;
    return cpu->gpr[2];
}

/* an interrupt handler or callback: the interrupted code keeps all its registers */
static int s_in_interrupt;
static int s_irq_mask;                    /* EnterCriticalSection */

int a2n_interrupts_enabled(void) { return !s_irq_mask && !s_in_interrupt; }
void a2n_set_critical(int on) { s_irq_mask = on; }
int a2n_in_interrupt(void) { return s_in_interrupt; }

void a2n_call_interrupt(uint32_t fn, int nargs, uint32_t arg0, uint32_t arg1)
{
    CPUState *cpu = &a2n_cpu;
    const CPUState saved = *cpu;
    s_in_interrupt++;
    cpu->gpr[29] = 0x8005C070u;           /* libetc's interrupt stack */
    cpu->load_value_rt = cpu->load_value_age = 0;
    a2n_call(cpu, fn, nargs, arg0, arg1);
    s_in_interrupt--;
    *cpu = saved;
}

void a2n_service(CPUState *cpu)
{
    (void)cpu;
    if (a2n_budget < 0) a2n_budget_spent();
    if (!a2n_interrupts_enabled()) return;
    a2n_pending = 0;
    a2n_events_run();
}

/* ---------------------------------------------------------------- boot */

void a2n_modules_init(void)
{
    a2n_libetc_init();
    a2n_bios_init();
    a2n_libc_init();
    a2n_gpu_init();
    a2n_io_init();
    a2n_cd_init();
    a2n_spu_init();
    a2n_mdec_init();
    a2n_pad_init();
    a2n_card_init();
}

static uint32_t s_entry;

int a2n_load_exe(const uint8_t *exe, size_t size)
{
    if (size < 0x800 || memcmp(exe, "PS-X EXE", 8)) return 0;
    uint32_t pc0, t_addr, t_size, s_addr;
    memcpy(&pc0, exe + 0x10, 4);
    memcpy(&t_addr, exe + 0x18, 4);
    memcpy(&t_size, exe + 0x1C, 4);
    memcpy(&s_addr, exe + 0x30, 4);
    if (t_size > size - 0x800) t_size = (uint32_t)(size - 0x800);
    memcpy(a2n_ram + (t_addr & 0x1FFFFFu), exe + 0x800, t_size);
    s_entry = pc0;
    a2n_cpu.gpr[29] = s_addr ? s_addr : 0x801FFFF0u;
    a2n_cpu.gpr[30] = a2n_cpu.gpr[29];
    return 1;
}

static jmp_buf s_reset;

void a2n_run(void)
{
    register_lifted();
    a2n_modules_init();
    if (setjmp(s_reset)) {
        /* soft reset by longjmp: resume after the guest setjmp, as the guest buffer says */
        a2n_longjmp_resume(&a2n_cpu);
        return;
    }
    a2n_cpu.gpr[31] = RETURN_MAGIC;
    a2n_dispatch(&a2n_cpu, s_entry);
    a2n_fatal("the game's entry point returned");
}

void a2n_unwind_to_boot(void)
{
    s_depth = 0;
    longjmp(s_reset, 1);
}
