/*
 * Traces for 1:1 checks (diagnostic only, inactive unless A2_DIAG_TRACE is set).
 *
 * At every waiting VSync call (VSync(0) or VSync(n>=2), libetc at 0x8003B1C8), that is
 * once per game frame, this writes one record:
 *   - VSync index and guest cycle (timing),
 *   - hash and count of what the game sent to the GPU since the previous record:
 *     the packets of every ordering table (DrawOTag, DrawOTagEnv), the drawing
 *     environments (PutDrawEnv) and the VRAM transfers (LoadImage, StoreImage,
 *     MoveImage, ClearImage),
 *   - hash of the whole VRAM (the picture),
 *   - a hash of each 4 KiB page of main RAM (the game state).
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

#define RAM_BYTES   (2u << 20)
#define PAGE_BYTES  4096u
#define PAGES       (RAM_BYTES / PAGE_BYTES)
#define VRAM_BYTES  (1024u * 512u * 2u)
#define FIELDS      6u                  /* index, cycle lo/hi, gpu hash, gpu items, vram hash */
#define MAX_LIST    32
#define DRAWENV_USER_BYTES 0x1Cu        /* clip, ofs, tw, tpage, dtd, dfe, isbg, rgb */

static FILE *s_trace;
static FILE *s_gpu_text;
static char s_path[1024];
static uint32_t s_index;
static long s_stop = -1;
static uint32_t s_dump_at[MAX_LIST], s_gpu_dump_at[MAX_LIST];
static int s_dump_count, s_gpu_dump_count;

static uint64_t s_gpu_hash = 0xCBF29CE484222325ull;
static uint32_t s_gpu_items;

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

static void gpu_mix(uint32_t w) { s_gpu_hash = (s_gpu_hash ^ w) * 0x100000001B3ull; }

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
        fprintf(s_gpu_text, "%s x=%d y=%d w=%d h=%d", kind, (int16_t)xy, (int16_t)(xy >> 16),
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
                fprintf(s_gpu_text, "P");
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
        fprintf(s_gpu_text, "DRAWENV");
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
        rec[3] = fold(s_gpu_hash);
        rec[4] = s_gpu_items;
        rec[5] = vram ? hash_words((const uint32_t *)vram, VRAM_BYTES / 4u) : 0;
        for (uint32_t p = 0; p < PAGES; ++p)
            rec[FIELDS + p] = hash_words((const uint32_t *)(ram + p * PAGE_BYTES), PAGE_BYTES / 4u);
        fwrite(rec, sizeof rec, 1, s_trace);
        if (in_list(s_dump_at, s_dump_count, s_index)) dump_ram(ram);
    }
    s_gpu_hash = 0xCBF29CE484222325ull;
    s_gpu_items = 0;
    if (s_gpu_text) { fclose(s_gpu_text); s_gpu_text = NULL; }
    if (s_stop >= 0 && (long)s_index >= s_stop) {
        if (s_trace) fclose(s_trace);
        _exit(0);
    }
    s_index++;
    if (in_list(s_gpu_dump_at, s_gpu_dump_count, s_index)) open_gpu_text(s_index);
    return 0;
}

__attribute__((constructor))
static void a2_register_diag(void)
{
    const char *path = getenv("A2_DIAG_TRACE");
    if (!path || !path[0]) return;
    snprintf(s_path, sizeof s_path, "%s", path);
    s_trace = fopen(s_path, "wb");
    if (!s_trace) return;
    const uint32_t header[3] = { 0x32523241u /* "A2R2" */, PAGES, FIELDS };
    fwrite(header, sizeof header, 1, s_trace);

    const char *e;
    if ((e = getenv("A2_DIAG_STOP"))) s_stop = strtol(e, NULL, 10);
    s_dump_count = parse_list(getenv("A2_DIAG_DUMP_AT"), s_dump_at);
    s_gpu_dump_count = parse_list(getenv("A2_DIAG_GPU_DUMP_AT"), s_gpu_dump_at);
    if (in_list(s_gpu_dump_at, s_gpu_dump_count, 0)) open_gpu_text(0);

    func_override_add("a2.diag.vsync", VSYNC_ADDR, vsync_hook, 0);
    func_override_add("a2.diag.draw_otag", DRAW_OTAG_ADDR, draw_otag_hook, 0);
    func_override_add("a2.diag.draw_otag_env", DRAW_OTAG_ENV_ADDR, draw_otag_env_hook, 0);
    func_override_add("a2.diag.put_drawenv", PUT_DRAWENV_ADDR, put_drawenv_hook, 0);
    func_override_add("a2.diag.load_image", LOAD_IMAGE_ADDR, load_image_hook, 0);
    func_override_add("a2.diag.store_image", STORE_IMAGE_ADDR, store_image_hook, 0);
    func_override_add("a2.diag.move_image", MOVE_IMAGE_ADDR, move_image_hook, 0);
    func_override_add("a2.diag.clear_image", CLEAR_IMAGE_ADDR, clear_image_hook, 0);
}
