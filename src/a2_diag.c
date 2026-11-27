/*
 * Traces for 1:1 checks (diagnostic only, inactive unless A2_DIAG_TRACE is set).
 *
 * At every waiting VSync call (VSync(0) or VSync(n>=2), libetc at 0x8003B1C8), that is
 * once per game frame, this writes one record:
 *   - VSync index and guest cycle (timing),
 *   - hash and count of what the game sent to the GPU since the previous record, from the
 *     main thread (strict order) and, separately, from interrupt handlers (the VBlank
 *     callback's loading indicator: its place among the main thread's items depends on
 *     timing):
 *     the packets of every ordering table (DrawOTag, DrawOTagEnv), single primitives
 *     (DrawPrim), the drawing and display environments (PutDrawEnv, PutDispEnv,
 *     SetDispMask) and the VRAM transfers (LoadImage, StoreImage, MoveImage, ClearImage),
 *   - hash of the whole VRAM (the picture),
 *   - a hash of each 4 KiB page of main RAM (the game state), with the words that are not
 *     game state replaced by zero: the kernel work area (0x80000000-0x8000FFFF: register
 *     save areas, written at every interrupt), the dead stack below the current $sp (stack
 *     frames of returned functions), the libetc interrupt stack (0x8005B0D0-0x8005C07F, dead
 *     between interrupts), and the ranges of A2_DIAG_IGNORE;
 *   - a hash of those excluded words ("noise"), so they can still be compared.
 * Two runs of the same replay (original code vs rewritten code) are compared with
 * tools/replay_diff.py.
 *
 * Environment:
 *   A2_DIAG_TRACE=path        write the trace
 *   A2_DIAG_DUMP_AT=i,j,..    also write the full 2 MiB RAM at these VSync indexes
 *                             to <path>.<index>.ram
 *   A2_DIAG_GPU_DUMP_AT=i,..  write the GPU journal of these frames (what was sent
 *                             between VSync i-1 and VSync i) to <path>.<index>.gpu.txt
 *   A2_DIAG_STOP=n            end the process after VSync index n (saves time)
 *   A2_DIAG_IGNORE=a+n,..     also exclude these ranges (hex address + byte count)
 *   A2_DIAG_RAW=1             exclude nothing
 *
 * Every hook declines its call: the original code runs unchanged and no guest cycle
 * is added.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cpu_state.h"
#include "func_override.h"

uint8_t psx_read_byte(uint32_t addr);
uint32_t psx_read_word(uint32_t addr);
uint16_t psx_read_half(uint32_t addr);

extern const uint8_t *replay_host_ram(void);
extern uint64_t replay_host_cycle(void);
extern const uint16_t *gpu_get_vram(void);

/* libetc / libgpu entry points in the main executable (see symbols/lib_names.txt). */
#define VSYNC_ADDR        0x8003B1C8u
#define CLEAR_IMAGE_ADDR  0x80033AD8u
#define LOAD_IMAGE_ADDR   0x80033C00u
#define STORE_IMAGE_ADDR  0x80033C60u
#define MOVE_IMAGE_ADDR   0x80033CC0u
#define DRAW_OTAG_ADDR    0x80033F48u
#define PUT_DRAWENV_ADDR  0x80033FB8u
#define DRAW_OTAG_ENV_ADDR 0x80034078u
#define DRAW_PRIM_ADDR    0x80033EECu
#define PUT_DISPENV_ADDR  0x80034184u
#define SET_DISP_MASK_ADDR 0x800338BCu

#define RAM_BYTES   (2u << 20)
#define PAGE_BYTES  4096u
#define PAGES       (RAM_BYTES / PAGE_BYTES)
#define VRAM_BYTES  (1024u * 512u * 2u)
#define FIELDS      8u                  /* index, cycle lo/hi, gpu hash, gpu items, vram, noise, gpu irq */
#define INTR_STACK_LO 0x0005B0D0u       /* libetc interrupt stack (physical), sp starts at 0x8005C07C */
#define INTR_STACK_HI 0x0005C080u
#define KERNEL_END  0x10000u            /* kernel work area, physical */
#define DEAD_STACK  0x4000u             /* bytes below $sp treated as dead stack (from the page
                                           boundary below, so a few bytes of difference in $sp
                                           between two runs do not move the window) */
#define MAX_RANGES  24
#define MAX_LIST    32
#define DRAWENV_USER_BYTES 0x1Cu        /* clip, ofs, tw, tpage, dtd, dfe, isbg, rgb */

static FILE *s_trace;
void (*s_stop_report)(void);              /* a summary to print before an early stop */
static void (*s_stop_reports[8])(void);   /* more of them */
static int s_n_stop_reports;
static void s_stop_report_chain(void (*fn)(void))
{
    if (s_n_stop_reports < 8) s_stop_reports[s_n_stop_reports++] = fn;
}
static FILE *s_gpu_text;
static char s_path[1024];
static uint32_t s_index;
static long s_stop = -1;
static uint32_t s_dump_at[MAX_LIST], s_gpu_dump_at[MAX_LIST];
static int s_dump_count, s_gpu_dump_count;

static uint32_t s_ignore_lo[MAX_RANGES], s_ignore_hi[MAX_RANGES];
static int s_ignore_count;
static int s_raw;

static uint64_t s_gpu_main_hash = 0xCBF29CE484222325ull, s_gpu_irq_hash = 0xCBF29CE484222325ull;
static uint32_t s_gpu_items;
extern int psx_get_in_exception(void);

static int in_list(const uint32_t *list, int n, uint32_t v)
{
    for (int i = 0; i < n; ++i)
        if (list[i] == v) return 1;
    return 0;
}

static int parse_list(const char *e, uint32_t *out)
{
    int n = 0;
    while (e && *e && n < MAX_LIST) {
        char *end;
        out[n++] = (uint32_t)strtoul(e, &end, 10);
        if (end == e) break;
        e = (*end == ',') ? end + 1 : end;
    }
    return n;
}

static uint32_t fold(uint64_t h) { return (uint32_t)(h ^ (h >> 32)); }

static uint32_t hash_words(const uint32_t *w, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; ++i)
        h = (h ^ w[i]) * 0x100000001B3ull;
    return fold(h);
}

static uint32_t ram_word(const uint8_t *ram, uint32_t addr)
{
    uint32_t v;
    memcpy(&v, ram + (addr & (RAM_BYTES - 4u)), 4);
    return v;
}

/* Lines of the readable journal: items sent from an interrupt handler start with "irq ". */
static FILE *gpu_line(void)
{
    if (psx_get_in_exception()) fputs("irq ", s_gpu_text);
    return s_gpu_text;
}

static void gpu_mix(uint32_t w)
{
    if (psx_get_in_exception()) s_gpu_irq_hash = (s_gpu_irq_hash ^ w) * 0x100000001B3ull;
    else s_gpu_main_hash = (s_gpu_main_hash ^ w) * 0x100000001B3ull;
}

/* ---- GPU journal ---- */

static void journal_rect(const char *kind, const uint8_t *ram, uint32_t rect, const uint32_t *extra, int n)
{
    const uint32_t xy = ram_word(ram, rect), wh = ram_word(ram, rect + 4);
    gpu_mix(kind[0] | (kind[1] << 8));
    gpu_mix(xy);
    gpu_mix(wh);
    for (int i = 0; i < n; ++i) gpu_mix(extra[i]);
    s_gpu_items++;
    if (s_gpu_text) {
        fprintf(gpu_line(), "%s x=%d y=%d w=%d h=%d", kind, (int16_t)xy, (int16_t)(xy >> 16),
                (int16_t)wh, (int16_t)(wh >> 16));
        for (int i = 0; i < n; ++i) fprintf(s_gpu_text, " %08x", extra[i]);
        fputc('\n', s_gpu_text);
    }
}

static void journal_ot(const uint8_t *ram, uint32_t p)
{
    for (uint32_t guard = 0; guard < (1u << 20); ++guard) {
        const uint32_t tag = ram_word(ram, p);
        const uint32_t n = tag >> 24;
        if (n) {
            gpu_mix(n);
            for (uint32_t i = 1; i <= n; ++i) gpu_mix(ram_word(ram, p + 4 * i));
            s_gpu_items++;
            if (s_gpu_text) {
                fprintf(gpu_line(), "P");
                for (uint32_t i = 1; i <= n; ++i) fprintf(s_gpu_text, " %08x", ram_word(ram, p + 4 * i));
                fputc('\n', s_gpu_text);
            }
        }
        if ((tag & 0x00FFFFFFu) == 0x00FFFFFFu) return;
        p = tag & 0x00FFFFFFu;
    }
}

static void journal_drawenv(const uint8_t *ram, uint32_t env)
{
    uint32_t w[DRAWENV_USER_BYTES / 4];
    for (uint32_t i = 0; i < DRAWENV_USER_BYTES / 4; ++i) w[i] = ram_word(ram, env + 4 * i);
    gpu_mix(0x454E5644u); /* "DVNE" */
    for (uint32_t i = 0; i < DRAWENV_USER_BYTES / 4; ++i) gpu_mix(w[i]);
    s_gpu_items++;
    if (s_gpu_text) {
        fprintf(gpu_line(), "DRAWENV");
        for (uint32_t i = 0; i < DRAWENV_USER_BYTES / 4; ++i) fprintf(s_gpu_text, " %08x", w[i]);
        fputc('\n', s_gpu_text);
    }
}

static int draw_otag_hook(CPUState *cpu)
{
    const uint8_t *ram = replay_host_ram();
    if (ram) journal_ot(ram, cpu->gpr[4]);
    return 0;
}

static int draw_otag_env_hook(CPUState *cpu)
{
    const uint8_t *ram = replay_host_ram();
    if (ram) {
        journal_drawenv(ram, cpu->gpr[5]);
        journal_ot(ram, cpu->gpr[4]);
    }
    return 0;
}

static int draw_prim_hook(CPUState *cpu)
{
    const uint8_t *ram = replay_host_ram();
    if (!ram) return 0;
    const uint32_t p = cpu->gpr[4], n = ram_word(ram, p) >> 24;
    gpu_mix(0x4D495250u); /* "PRIM" */
    gpu_mix(n);
    for (uint32_t i = 1; i <= n; ++i) gpu_mix(ram_word(ram, p + 4 * i));
    s_gpu_items++;
    if (s_gpu_text) {
        fprintf(gpu_line(), "PRIM");
        for (uint32_t i = 1; i <= n; ++i) fprintf(s_gpu_text, " %08x", ram_word(ram, p + 4 * i));
        fputc('\n', s_gpu_text);
    }
    return 0;
}

static int put_dispenv_hook(CPUState *cpu)
{
    const uint8_t *ram = replay_host_ram();
    if (!ram) return 0;
    uint32_t w[5];                                 /* disp, screen, isinter, isrgb24 */
    for (int i = 0; i < 5; ++i) w[i] = ram_word(ram, cpu->gpr[4] + 4u * (uint32_t)i);
    w[4] &= 0xFFFFu;
    gpu_mix(0x50534944u); /* "DISP" */
    for (int i = 0; i < 5; ++i) gpu_mix(w[i]);
    s_gpu_items++;
    if (s_gpu_text)
        fprintf(gpu_line(), "DISPENV %08x %08x %08x %08x %04x\n", w[0], w[1], w[2], w[3], w[4]);
    return 0;
}

static int set_disp_mask_hook(CPUState *cpu)
{
    gpu_mix(0x4B53414Du); /* "MASK" */
    gpu_mix(cpu->gpr[4]);
    s_gpu_items++;
    if (s_gpu_text) fprintf(gpu_line(), "DISPMASK %u\n", cpu->gpr[4]);
    return 0;
}

static int put_drawenv_hook(CPUState *cpu)
{
    const uint8_t *ram = replay_host_ram();
    if (ram) journal_drawenv(ram, cpu->gpr[4]);
    return 0;
}

static int load_image_hook(CPUState *cpu)
{
    const uint8_t *ram = replay_host_ram();
    if (!ram) return 0;
    const uint32_t rect = cpu->gpr[4], src = cpu->gpr[5];
    const uint32_t wh = ram_word(ram, rect + 4);
    const uint32_t w = (uint16_t)wh, h = (uint16_t)(wh >> 16);
    uint32_t words = (w * h + 1u) / 2u;
    if (words > VRAM_BYTES / 4u) words = VRAM_BYTES / 4u;
    uint64_t hh = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < words; ++i) hh = (hh ^ ram_word(ram, src + 4 * i)) * 0x100000001B3ull;
    const uint32_t data = fold(hh);
    journal_rect("LOAD", ram, rect, &data, 1);
    return 0;
}

static int store_image_hook(CPUState *cpu)
{
    const uint8_t *ram = replay_host_ram();
    if (ram) journal_rect("STORE", ram, cpu->gpr[4], NULL, 0);
    return 0;
}

static int move_image_hook(CPUState *cpu)
{
    const uint8_t *ram = replay_host_ram();
    const uint32_t extra[2] = { cpu->gpr[5], cpu->gpr[6] };
    if (ram) journal_rect("MOVE", ram, cpu->gpr[4], extra, 2);
    return 0;
}

static int clear_image_hook(CPUState *cpu)
{
    const uint8_t *ram = replay_host_ram();
    const uint32_t extra[3] = { cpu->gpr[5] & 0xFFu, cpu->gpr[6] & 0xFFu, cpu->gpr[7] & 0xFFu };
    if (ram) journal_rect("CLEAR", ram, cpu->gpr[4], extra, 3);
    return 0;
}

/* ---- RAM pages with the non-game-state words masked ---- */

/* Hash of page p; words inside [lo[i], hi[i]) count as zero and go to *noise instead. */
static uint32_t page_hash_masked(const uint8_t *ram, uint32_t p, const uint32_t *lo, const uint32_t *hi,
                                 int n, uint64_t *noise)
{
    const uint32_t start = p * PAGE_BYTES, end = start + PAGE_BYTES;
    int touched = 0;
    for (int i = 0; i < n; ++i)
        if (lo[i] < end && hi[i] > start) touched = 1;
    const uint32_t *w = (const uint32_t *)(ram + start);
    if (!touched)
        return hash_words(w, PAGE_BYTES / 4u);
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t k = 0; k < PAGE_BYTES / 4u; ++k) {
        const uint32_t off = start + 4u * k;
        int masked = 0;
        for (int i = 0; i < n && !masked; ++i)
            masked = off >= lo[i] && off < hi[i];
        if (masked) {
            *noise = (*noise ^ w[k]) * 0x100000001B3ull;
            h = (h ^ 0u) * 0x100000001B3ull;
        } else {
            h = (h ^ w[k]) * 0x100000001B3ull;
        }
    }
    return fold(h);
}

/* ---- per-VSync record ---- */

static void dump_ram(const uint8_t *ram)
{
    char name[1100];
    snprintf(name, sizeof name, "%s.%u.ram", s_path, s_index);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fwrite(ram, 1, RAM_BYTES, f);
    fclose(f);
}

static void open_gpu_text(uint32_t index)
{
    char name[1100];
    snprintf(name, sizeof name, "%s.%u.gpu.txt", s_path, index);
    s_gpu_text = fopen(name, "w");
}

static int vsync_hook(CPUState *cpu)
{
    const int32_t mode = (int32_t)cpu->gpr[4];
    if (mode == 1 || mode < 0)          /* counter queries do not wait */
        return 0;
    const uint8_t *ram = replay_host_ram();
    const uint16_t *vram = gpu_get_vram();
    if (ram && s_trace) {
        static uint32_t rec[FIELDS + PAGES];
        const uint64_t cycle = replay_host_cycle();
        rec[0] = s_index;
        rec[1] = (uint32_t)cycle;
        rec[2] = (uint32_t)(cycle >> 32);
        rec[3] = fold(s_gpu_main_hash);
        rec[4] = s_gpu_items;
        rec[5] = vram ? hash_words((const uint32_t *)vram, VRAM_BYTES / 4u) : 0;
        uint32_t lo[MAX_RANGES + 3], hi[MAX_RANGES + 3];
        int n = 0;
        if (!s_raw) {
            const uint32_t sp = cpu->gpr[29] & (RAM_BYTES - 1u);
            lo[n] = 0; hi[n++] = KERNEL_END;
            lo[n] = sp > DEAD_STACK ? (sp - DEAD_STACK) & ~(PAGE_BYTES - 1u) : 0; hi[n++] = sp;
            lo[n] = INTR_STACK_LO; hi[n++] = INTR_STACK_HI;
            for (int i = 0; i < s_ignore_count; ++i) { lo[n] = s_ignore_lo[i]; hi[n++] = s_ignore_hi[i]; }
        }
        uint64_t noise = 0xCBF29CE484222325ull;
        for (uint32_t p = 0; p < PAGES; ++p)
            rec[FIELDS + p] = page_hash_masked(ram, p, lo, hi, n, &noise);
        rec[6] = fold(noise);
        rec[7] = fold(s_gpu_irq_hash);
        fwrite(rec, sizeof rec, 1, s_trace);
        if (in_list(s_dump_at, s_dump_count, s_index)) dump_ram(ram);
    }
    s_gpu_main_hash = s_gpu_irq_hash = 0xCBF29CE484222325ull;
    s_gpu_items = 0;
    if (s_gpu_text) { fclose(s_gpu_text); s_gpu_text = NULL; }
    if (s_stop >= 0 && (long)s_index >= s_stop) {
        if (s_trace) fclose(s_trace);
        fflush(NULL);
        void (*report)(void) = s_stop_report;
        if (report) report();
        for (int i = 0; i < s_n_stop_reports; ++i) s_stop_reports[i]();
        _exit(0);
    }
    s_index++;
    if (in_list(s_gpu_dump_at, s_gpu_dump_count, s_index)) open_gpu_text(s_index);
    return 0;
}

/* A2_DIAG_TTY=1: the game's console output (puts, printf format), with the frame index. */
static int tty_hook(CPUState *cpu)
{
    char text[96];
    uint32_t a = cpu->gpr[4];
    size_t n = 0;
    for (; n + 1 < sizeof text; ++n, ++a) {
        const uint8_t c = psx_read_byte(a);
        if (!c) break;
        text[n] = (c == '\n' || c == '\r') ? ' ' : (char)c;
    }
    text[n] = 0;
    fprintf(stderr, "a2.tty [%u] %s\n", s_index, text);
    return 0;
}

/* A2_DIAG_VWAIT=n: libetc's v_wait (VSync's wait loop) gives up after (timeout << 15) turns
 * of its loop. Print the waits that ended with fewer than n turns to spare: how close the
 * game comes to a "VSync: timeout" in this timing model. */
static long s_vwait_report = -1;
static int vwait_hook(CPUState *cpu)
{
    const uint32_t sp = cpu->gpr[29], timeout = cpu->gpr[5];
    func_override_call_original(cpu);
    const int32_t left = (int32_t)psx_read_word(sp - 0x10u);
    if (timeout && left < s_vwait_report)
        fprintf(stderr, "a2.vwait [%u] timeout=%u turns left=%d\n", s_index, timeout, left);
    return 1;
}

__attribute__((constructor))
static void a2_register_diag(void)
{
    const char *tty = getenv("A2_DIAG_TTY");
    if (tty && tty[0] == '1') {
        func_override_add("a2.diag.puts", 0x8003AC88u, tty_hook, 0);
        func_override_add("a2.diag.printf", 0x800317C8u, tty_hook, 0);
    }
    const char *vw = getenv("A2_DIAG_VWAIT");
    if (vw && vw[0]) {
        s_vwait_report = strtol(vw, NULL, 10);
        func_override_add("a2.diag.v_wait", 0x8003B340u, vwait_hook, FO_CREDIT_SELF);
    }
    const char *path = getenv("A2_DIAG_TRACE");
    if (!path || !path[0]) return;
    snprintf(s_path, sizeof s_path, "%s", path);
    s_trace = fopen(s_path, "wb");
    if (!s_trace) return;
    const uint32_t header[3] = { 0x34523241u /* "A2R4" */, PAGES, FIELDS };
    fwrite(header, sizeof header, 1, s_trace);

    const char *e;
    if ((e = getenv("A2_DIAG_STOP"))) s_stop = strtol(e, NULL, 10);
    s_dump_count = parse_list(getenv("A2_DIAG_DUMP_AT"), s_dump_at);
    s_gpu_dump_count = parse_list(getenv("A2_DIAG_GPU_DUMP_AT"), s_gpu_dump_at);
    if ((e = getenv("A2_DIAG_RAW")) && e[0] == '1') s_raw = 1;
    for (e = getenv("A2_DIAG_IGNORE"); e && *e && s_ignore_count < MAX_RANGES;) {
        char *end;
        const uint32_t a = (uint32_t)strtoul(e, &end, 16) & (RAM_BYTES - 1u);
        uint32_t len = 4;
        if (*end == '+') len = (uint32_t)strtoul(end + 1, &end, 16);
        s_ignore_lo[s_ignore_count] = a;
        s_ignore_hi[s_ignore_count++] = a + len;
        if (*end != ',') break;
        e = end + 1;
    }
    if (in_list(s_gpu_dump_at, s_gpu_dump_count, 0)) open_gpu_text(0);

    func_override_add("a2.diag.vsync", VSYNC_ADDR, vsync_hook, 0);
    func_override_add("a2.diag.draw_otag", DRAW_OTAG_ADDR, draw_otag_hook, 0);
    func_override_add("a2.diag.draw_otag_env", DRAW_OTAG_ENV_ADDR, draw_otag_env_hook, 0);
    func_override_add("a2.diag.put_drawenv", PUT_DRAWENV_ADDR, put_drawenv_hook, 0);
    func_override_add("a2.diag.load_image", LOAD_IMAGE_ADDR, load_image_hook, 0);
    func_override_add("a2.diag.store_image", STORE_IMAGE_ADDR, store_image_hook, 0);
    func_override_add("a2.diag.move_image", MOVE_IMAGE_ADDR, move_image_hook, 0);
    func_override_add("a2.diag.clear_image", CLEAR_IMAGE_ADDR, clear_image_hook, 0);
    func_override_add("a2.diag.draw_prim", DRAW_PRIM_ADDR, draw_prim_hook, 0);
    func_override_add("a2.diag.put_dispenv", PUT_DISPENV_ADDR, put_dispenv_hook, 0);
    func_override_add("a2.diag.set_disp_mask", SET_DISP_MASK_ADDR, set_disp_mask_hook, 0);
}

/* A2_DIAG_LIBCOUNT=file: count the calls of the functions listed in `file` ("ADDR NAME" per
 * line, the PsyQ library), printed at exit: which library the game really uses. */
typedef struct { uint32_t addr; char name[40]; uint64_t calls; } LibCount;
static LibCount *s_lib;
static int s_n_lib;

static int libcount_hook(CPUState *cpu)
{
    (void)cpu;
    const uint32_t a = func_override_active_address();
    for (int lo = 0, hi = s_n_lib - 1; lo <= hi;) {
        const int mid = (lo + hi) / 2;
        const uint32_t m = s_lib[mid].addr & 0x1FFFFFFFu;
        if (m == (a & 0x1FFFFFFFu)) { s_lib[mid].calls++; break; }
        if (m < (a & 0x1FFFFFFFu)) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int libcount_cmp(const void *x, const void *y)
{
    const LibCount *a = x, *b = y;
    return a->calls < b->calls ? 1 : a->calls > b->calls ? -1 : 0;
}

static void libcount_report(void)
{
    qsort(s_lib, (size_t)s_n_lib, sizeof *s_lib, libcount_cmp);
    for (int i = 0; i < s_n_lib && s_lib[i].calls; ++i)
        fprintf(stderr, "a2.lib %08X %-24s %llu\n", s_lib[i].addr, s_lib[i].name,
                (unsigned long long)s_lib[i].calls);
}

__attribute__((constructor))
static void a2_register_libcount(void)
{
    const char *path = getenv("A2_DIAG_LIBCOUNT");
    FILE *f = path && path[0] ? fopen(path, "r") : NULL;
    if (!f) return;
    s_lib = calloc(1024, sizeof *s_lib);
    unsigned addr;
    char name[40];
    while (s_lib && s_n_lib < 1024 && fscanf(f, "%x %39s", &addr, name) == 2) {
        s_lib[s_n_lib].addr = addr;
        snprintf(s_lib[s_n_lib].name, sizeof s_lib[s_n_lib].name, "%s", name);
        s_n_lib++;
    }
    fclose(f);
    for (int i = 0; i < s_n_lib; ++i)
        func_override_add(s_lib[i].name, s_lib[i].addr, libcount_hook, 0);
    atexit(libcount_report);
    s_stop_report_chain(libcount_report);
}

/* A2_DIAG_STREAM=1: the movie streaming ring (libcd St*) at each sector interrupt and
 * StGetNext, to compare the standalone engine's CD drive with the framework's. */
static int stream_hook(CPUState *cpu)
{
    (void)cpu;
    const uint32_t a = func_override_active_address();
    const uint32_t base = psx_read_word(0x8007F578u);
    fprintf(stderr, "a2.stream %s:", a == 0x80043F88u ? "int" : "get");
    for (int i = 0; i < 12 && base; ++i)
        fprintf(stderr, " %x/%u", psx_read_half(base + 32u * (uint32_t)i), psx_read_half(base + 32u * (uint32_t)i + 4));
    fprintf(stderr, " F554=%u F540=%u\n", psx_read_word(0x8007F554u), psx_read_word(0x8007F540u));
    return 0;
}

__attribute__((constructor))
static void a2_register_stream(void)
{
    const char *e = getenv("A2_DIAG_STREAM");
    if (!e || e[0] != '1') return;
    func_override_add("a2.diag.st_int", 0x80043F88u, stream_hook, 0);
    func_override_add("a2.diag.st_get", 0x8003AF58u, stream_hook, 0);
}
