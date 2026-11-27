/*
 * host.c - the platform layer of the standalone engine (SDL3): window and picture, sound,
 * controllers, timing, files, and the first-launch import of the player's disc.
 *
 *   alundra2 [--data DIR] [--saves DIR] [--import IMAGE] [--fullscreen] [--scale N]
 *            [--smooth] [--vsync] [--headless] [--frames N] [--dump-vram FILE]
 *            [--dump-ram FILE]
 *
 * Data folder: --data, else $A2_DATA, else "data" beside the program when it holds an
 * imported disc, else the user's application folder (SDL_GetPrefPath). Memory cards:
 * --saves, else $A2_SAVES, else "memcards" in the user's application folder.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cd.h"
#include "disc.h"
#include "engine.h"
#include "host.h"
#include "native_modules.h"

#ifdef _WIN32
/* laptops with two GPUs: ask for the fast one */
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

#define TITLE "Alundra 2"

static struct {
    int headless, fullscreen, scale, smooth, vsync, self_test;
    long frames, shot_every;
    const char *data_opt, *saves_opt, *import, *dump_vram, *dump_ram, *shot_prefix, *verify;
} opt = { 0, 0, 3, 0, 0, 0, -1, 0, NULL, NULL, NULL, NULL, NULL, "shot", NULL };

static char s_data[1024], s_saves[1024];
static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex15, *s_tex24;
static SDL_AudioStream *s_audio;
static SDL_Gamepad *s_pad;
static uint64_t s_vblanks;
static uint8_t s_rumble_small, s_rumble_big;

/* ---------------------------------------------------------------- time */

uint64_t a2n_host_now_ns(void) { return SDL_GetTicksNS(); }

void a2n_host_sleep_until(uint64_t ns)
{
    const uint64_t now = SDL_GetTicksNS();
    if (ns > now) SDL_DelayPrecise(ns - now);
}

static Uint64 SDLCALL tick(void *ud, SDL_TimerID id, Uint64 interval)
{
    (void)ud;
    (void)id;
    a2n_clock_tick();
    return interval;
}

/* A2_WATCHDOG=seconds: report where the game is when no vertical blank came for that long */
static uint64_t s_watch_last, s_watch_count;
static Uint32 SDLCALL watchdog(void *ud, SDL_TimerID id, Uint32 interval)
{
    (void)ud;
    (void)id;
    const uint64_t n = a2n_vblank_count();
    if (n != s_watch_count) { s_watch_count = n; s_watch_last = SDL_GetTicks(); return interval; }
    fprintf(stderr, "a2: watchdog: no vertical blank for %u s (count %llu)\n",
            (unsigned)((SDL_GetTicks() - s_watch_last) / 1000), (unsigned long long)n);
    a2n_print_stack();
    if (getenv("A2_WATCHDOG_EXIT")) _Exit(3);
    return interval;
}

/* ---------------------------------------------------------------- files */

const char *a2n_host_data_dir(void) { return s_data; }
const char *a2n_host_save_dir(void) { return s_saves; }

void *a2n_host_load(const char *relative, size_t *size)
{
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", s_data, relative);
    return SDL_LoadFile(p, size);
}

static int has_disc(const char *dir)
{
    char p[1100];
    snprintf(p, sizeof p, "%s/disc.txt", dir);
    return SDL_GetPathInfo(p, NULL);
}

static void trim_slash(char *p)
{
    size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) p[--n] = 0;
}

static void choose_dirs(void)
{
    char *pref = SDL_GetPrefPath(NULL, "alundra2-fr");
    const char *base = SDL_GetBasePath();
    char beside[1024] = "";
    if (base) snprintf(beside, sizeof beside, "%sdata", base);
    if (opt.data_opt) snprintf(s_data, sizeof s_data, "%s", opt.data_opt);
    else if (getenv("A2_DATA")) snprintf(s_data, sizeof s_data, "%s", getenv("A2_DATA"));
    else if (beside[0] && has_disc(beside)) snprintf(s_data, sizeof s_data, "%s", beside);
    else snprintf(s_data, sizeof s_data, "%sdata", pref ? pref : "./");
    if (opt.saves_opt) snprintf(s_saves, sizeof s_saves, "%s", opt.saves_opt);
    else if (getenv("A2_SAVES")) snprintf(s_saves, sizeof s_saves, "%s", getenv("A2_SAVES"));
    else snprintf(s_saves, sizeof s_saves, "%smemcards", pref ? pref : "./");
    trim_slash(s_data);
    trim_slash(s_saves);
    SDL_CreateDirectory(s_saves);
    /* memory cards of the earlier (0.2) builds: copied once, the originals stay */
    char *old = SDL_GetPrefPath(NULL, "Alundra 2");
    for (int i = 1; old && !opt.saves_opt && !getenv("A2_SAVES") && i <= 2; ++i) {
        char from[1100], to[1100];
        snprintf(from, sizeof from, "%ssaves/card%d.mcd", old, i);
        snprintf(to, sizeof to, "%s/card%d.mcd", s_saves, i);
        if (SDL_GetPathInfo(from, NULL) && !SDL_GetPathInfo(to, NULL) && SDL_CopyFile(from, to))
            fprintf(stderr, "a2: memory card copied from %s\n", from);
    }
    SDL_free(old);
    SDL_free(pref);
}

/* ---------------------------------------------------------------- messages */

static void message(int error, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "a2: %s\n", buf);
    if (!opt.headless)
        SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, TITLE, buf, s_win);
}

void a2n_host_fatal_hook(void)
{
    if (!opt.headless)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, TITLE,
                                 "Le jeu s'est arrêté sur une erreur interne.\n"
                                 "Les détails sont dans la sortie d'erreur (stderr).", s_win);
}

/* ---------------------------------------------------------------- window */

static void open_window(void)
{
    if (opt.headless || s_win) return;
    const int w = 320 * opt.scale, h = 240 * opt.scale;
    s_win = SDL_CreateWindow(TITLE, w, h, SDL_WINDOW_RESIZABLE | (opt.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!s_win) { fprintf(stderr, "a2: no window: %s\n", SDL_GetError()); exit(1); }
    s_ren = SDL_CreateRenderer(s_win, NULL);
    if (!s_ren) { fprintf(stderr, "a2: no renderer: %s\n", SDL_GetError()); exit(1); }
    SDL_SetRenderVSync(s_ren, opt.vsync ? 1 : 0);
    SDL_SetRenderLogicalPresentation(s_ren, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    s_tex15 = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_XBGR1555, SDL_TEXTUREACCESS_STREAMING, 1024, 512);
    s_tex24 = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 682, 512);
    const SDL_ScaleMode mode = opt.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
    SDL_SetTextureScaleMode(s_tex15, mode);
    SDL_SetTextureScaleMode(s_tex24, mode);
}

static void toggle_fullscreen(void)
{
    opt.fullscreen = !opt.fullscreen;
    SDL_SetWindowFullscreen(s_win, opt.fullscreen);
}

static void present(void)
{
    int x, y, w, h, depth24, on;
    a2n_gpu_display(&x, &y, &w, &h, &depth24, &on);
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderClear(s_ren);
    if (on && w > 0 && h > 0) {
        const uint16_t *vram = a2n_gpu_vram();
        if (w > 1024) w = 1024;
        if (h > 512) h = 512;
        x &= 1023;
        y &= 511;
        SDL_Texture *t = depth24 ? s_tex24 : s_tex15;
        const int vw = depth24 ? (w * 3 + 1) / 2 : w;          /* width in VRAM halfwords */
        const SDL_Rect r = { 0, 0, w, h };
        if (x + vw <= 1024 && y + h <= 512) {
            SDL_UpdateTexture(t, &r, vram + y * 1024 + x, 2048);
        } else {                                                /* wraps around VRAM */
            static uint16_t tmp[1024 * 512];
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < vw; ++i) tmp[j * vw + i] = vram[((y + j) & 511) * 1024 + ((x + i) & 1023)];
            SDL_UpdateTexture(t, &r, tmp, vw * 2);
        }
        const SDL_FRect src = { 0, 0, (float)w, (float)h }, dst = { 0, 0, 640, 480 };
        SDL_RenderTexture(s_ren, t, &src, &dst);
    }
    SDL_RenderPresent(s_ren);
}

/* ---------------------------------------------------------------- controllers */

enum {
    B_SELECT = 1 << 0, B_L3 = 1 << 1, B_R3 = 1 << 2, B_START = 1 << 3,
    B_UP = 1 << 4, B_RIGHT = 1 << 5, B_DOWN = 1 << 6, B_LEFT = 1 << 7,
    B_L2 = 1 << 8, B_R2 = 1 << 9, B_L1 = 1 << 10, B_R1 = 1 << 11,
    B_TRIANGLE = 1 << 12, B_CIRCLE = 1 << 13, B_CROSS = 1 << 14, B_SQUARE = 1 << 15,
};

static const struct { SDL_Scancode key; uint16_t bit; } s_keys[] = {
    { SDL_SCANCODE_UP, B_UP }, { SDL_SCANCODE_DOWN, B_DOWN },
    { SDL_SCANCODE_LEFT, B_LEFT }, { SDL_SCANCODE_RIGHT, B_RIGHT },
    { SDL_SCANCODE_X, B_CROSS }, { SDL_SCANCODE_SPACE, B_CROSS },
    { SDL_SCANCODE_C, B_CIRCLE }, { SDL_SCANCODE_Z, B_SQUARE }, { SDL_SCANCODE_S, B_TRIANGLE },
    { SDL_SCANCODE_Q, B_L1 }, { SDL_SCANCODE_E, B_R1 }, { SDL_SCANCODE_1, B_L2 }, { SDL_SCANCODE_3, B_R2 },
    { SDL_SCANCODE_RETURN, B_START }, { SDL_SCANCODE_BACKSPACE, B_SELECT },
    { SDL_SCANCODE_RSHIFT, B_SELECT },
};

static const struct { SDL_GamepadButton b; uint16_t bit; } s_buttons[] = {
    { SDL_GAMEPAD_BUTTON_SOUTH, B_CROSS }, { SDL_GAMEPAD_BUTTON_EAST, B_CIRCLE },
    { SDL_GAMEPAD_BUTTON_WEST, B_SQUARE }, { SDL_GAMEPAD_BUTTON_NORTH, B_TRIANGLE },
    { SDL_GAMEPAD_BUTTON_BACK, B_SELECT }, { SDL_GAMEPAD_BUTTON_START, B_START },
    { SDL_GAMEPAD_BUTTON_LEFT_STICK, B_L3 }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, B_R3 },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, B_L1 }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, B_R1 },
    { SDL_GAMEPAD_BUTTON_DPAD_UP, B_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, B_DOWN },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT, B_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, B_RIGHT },
};

static uint8_t axis8(Sint16 v) { return (uint8_t)((v + 32768) >> 8); }

/* --input FILE: scripted buttons for tests, one rule a line: START END PERIOD HOLD BUTTONS
 * (vertical blanks; the buttons are held while (vblank - START) % PERIOD < HOLD), BUTTONS
 * as names joined by '+': UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2
 * START SELECT L3 R3 */
typedef struct { long start, end, period, hold; uint16_t buttons; } InputRule;
static InputRule *s_rules;
static int s_n_rules;

static uint16_t button_bits(const char *names)
{
    static const struct { const char *name; uint16_t bit; } map[] = {
        { "UP", B_UP }, { "DOWN", B_DOWN }, { "LEFT", B_LEFT }, { "RIGHT", B_RIGHT },
        { "CROSS", B_CROSS }, { "CIRCLE", B_CIRCLE }, { "SQUARE", B_SQUARE }, { "TRIANGLE", B_TRIANGLE },
        { "L1", B_L1 }, { "R1", B_R1 }, { "L2", B_L2 }, { "R2", B_R2 },
        { "START", B_START }, { "SELECT", B_SELECT }, { "L3", B_L3 }, { "R3", B_R3 },
    };
    uint16_t bits = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", names);
    for (char *t = strtok(buf, "+"); t; t = strtok(NULL, "+"))
        for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i)
            if (!strcmp(t, map[i].name)) bits |= map[i].bit;
    return bits;
}

static void load_input_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "a2: cannot read %s\n", path); exit(2); }
    char line[512], names[256];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        InputRule r;
        if (line[0] == '#' || sscanf(line, "%ld %ld %ld %ld %255s", &r.start, &r.end, &r.period, &r.hold, names) != 5) continue;
        r.buttons = button_bits(names);
        if (r.period < 1) r.period = 1;
        if (s_n_rules == cap) s_rules = realloc(s_rules, sizeof *s_rules * (size_t)(cap = cap ? cap * 2 : 16));
        s_rules[s_n_rules++] = r;
    }
    fclose(f);
}

static uint16_t scripted_buttons(void)
{
    const long v = (long)s_vblanks;
    uint16_t bits = 0;
    for (int i = 0; i < s_n_rules; ++i) {
        const InputRule *r = &s_rules[i];
        if (v >= r->start && v < r->end && (v - r->start) % r->period < r->hold) bits |= r->buttons;
    }
    return bits;
}

void a2n_host_pad(int port, A2nPadState *out)
{
    memset(out, 0, sizeof *out);
    if (port != 0) return;
    out->connected = 1;
    out->analog = 1;
    out->lx = out->ly = out->rx = out->ry = 0x80;
    out->buttons = scripted_buttons();
    if (opt.headless) return;
    const bool *kb = SDL_GetKeyboardState(NULL);
    for (size_t i = 0; i < sizeof s_keys / sizeof s_keys[0]; ++i)
        if (kb[s_keys[i].key]) out->buttons |= s_keys[i].bit;
    if (s_pad) {
        for (size_t i = 0; i < sizeof s_buttons / sizeof s_buttons[0]; ++i)
            if (SDL_GetGamepadButton(s_pad, s_buttons[i].b)) out->buttons |= s_buttons[i].bit;
        if (SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) out->buttons |= B_L2;
        if (SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) out->buttons |= B_R2;
        out->lx = axis8(SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTX));
        out->ly = axis8(SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTY));
        out->rx = axis8(SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTX));
        out->ry = axis8(SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTY));
    }
}

void a2n_host_rumble(int port, uint8_t small, uint8_t big)
{
    if (port != 0) return;
    s_rumble_small = small;
    s_rumble_big = big;
}

static void open_first_gamepad(void)
{
    if (s_pad) return;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) s_pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
}

/* ---------------------------------------------------------------- events */

static void handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_QUIT:
        SDL_Quit();
        exit(0);
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_F11 ||
            (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & SDL_KMOD_ALT)))
            toggle_fullscreen();
        else if (e->key.scancode == SDL_SCANCODE_ESCAPE && opt.fullscreen)
            toggle_fullscreen();
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        open_first_gamepad();
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (s_pad && e->gdevice.which == SDL_GetGamepadID(s_pad)) {
            SDL_CloseGamepad(s_pad);
            s_pad = NULL;
            open_first_gamepad();
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- sound */

/* --dump-audio FILE: the mixed sound as a 44.1 kHz stereo WAV (diagnostics) */
static FILE *s_wav;
static uint32_t s_wav_frames;

static void wav_header(FILE *f, uint32_t frames)
{
    const uint32_t data = frames * 4, riff = 36 + data, fmt_size = 16, rate = 44100, bps = 44100 * 4;
    const uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&bps, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

static void wav_close(void)
{
    if (!s_wav) return;
    wav_header(s_wav, s_wav_frames);
    fclose(s_wav);
    s_wav = NULL;
}

void a2n_host_audio(const int16_t *stereo, int frames)
{
    if (s_wav) {
        fwrite(stereo, 4, (size_t)frames, s_wav);
        s_wav_frames += (uint32_t)frames;
    }
    if (!s_audio) return;
    /* keep at most ~150 ms queued: the host clock and the sound card drift apart */
    if (SDL_GetAudioStreamQueued(s_audio) > 44100 * 4 * 15 / 100) return;
    SDL_PutAudioStreamData(s_audio, stereo, frames * 4);
}

static void open_audio(void)
{
    if (opt.headless) return;
    const SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 44100 };
    s_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!s_audio) { fprintf(stderr, "a2: no sound: %s\n", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(s_audio);
}

/* ---------------------------------------------------------------- vertical blank */

static void dump(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(p, 1, n, f);
    fclose(f);
}

/* the picture on screen as a BMP file (--shot-every N: diagnostics, headless runs) */
static void screenshot(const char *path)
{
    int x, y, w, h, depth24, on;
    a2n_gpu_display(&x, &y, &w, &h, &depth24, &on);
    if (w <= 0 || h <= 0) return;
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGB24);
    if (!s) return;
    const uint16_t *vram = a2n_gpu_vram();
    const uint8_t *bytes = (const uint8_t *)vram;
    for (int j = 0; j < h; ++j) {
        uint8_t *row = (uint8_t *)s->pixels + j * s->pitch;
        for (int i = 0; i < w; ++i) {
            uint8_t r = 0, g = 0, b = 0;
            if (!on) {
            } else if (depth24) {
                const int bx = x * 2 + i * 3;
                const uint8_t *p = bytes + (((y + j) & 511) * 2048);
                r = p[(bx) & 2047];
                g = p[(bx + 1) & 2047];
                b = p[(bx + 2) & 2047];
            } else {
                const uint16_t c = vram[((y + j) & 511) * 1024 + ((x + i) & 1023)];
                r = (uint8_t)((c & 31) << 3);
                g = (uint8_t)(((c >> 5) & 31) << 3);
                b = (uint8_t)(((c >> 10) & 31) << 3);
            }
            row[i * 3] = r;
            row[i * 3 + 1] = g;
            row[i * 3 + 2] = b;
        }
    }
    SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
}

static void finish_run(void)
{
    wav_close();
    if (opt.dump_vram) dump(opt.dump_vram, a2n_gpu_vram(), 1024 * 512 * 2);
    if (opt.dump_ram) dump(opt.dump_ram, a2n_ram, A2N_RAM_SIZE);
}

/* A2_POKE=vblank:addr=value[:size][,...]: write guest memory at given vertical blanks (tests;
 * size 1, 2 or 4 bytes, 4 by default) */
static void pokes(void)
{
    static const char *list;
    static int init;
    if (!init) { init = 1; list = getenv("A2_POKE"); }
    if (!list) return;
    for (const char *p = list; p && *p;) {
        unsigned long v = 0;
        unsigned a = 0, val = 0, size = 4;
        if (sscanf(p, "%lu:%x=%x:%u", &v, &a, &val, &size) >= 3 && v == s_vblanks) {
            if (size == 1) a2n_w8(a, (uint8_t)val);
            else if (size == 2) a2n_w16(a, (uint16_t)val);
            else a2n_w32(a, val);
            fprintf(stderr, "a2: poke %08x = %x (%u bytes) at vblank %lu\n", a, val, size, v);
        }
        p = strchr(p, ',');
        if (p) p++;
    }
}

void a2n_host_vblank(void)
{
    s_vblanks++;
    pokes();
    if (opt.shot_every > 0 && s_vblanks % (uint64_t)opt.shot_every == 0) {
        char p[1100];
        snprintf(p, sizeof p, "%s_%06llu.bmp", opt.shot_prefix, (unsigned long long)s_vblanks);
        screenshot(p);
    }
    if (opt.frames >= 0 && (long)s_vblanks >= opt.frames) {
        finish_run();
        fprintf(stderr, "a2: stopped after %ld vertical blanks\n", opt.frames);
        a2n_print_stack();
        exit(0);
    }
    if (opt.headless) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_event(&e);
    if (s_pad) SDL_RumbleGamepad(s_pad, s_rumble_small ? 0xFFFF : 0, (Uint16)(s_rumble_big * 257u), 100);
    present();
}

/* ---------------------------------------------------------------- first launch: import */

static SDL_AtomicInt s_progress;      /* per mille */
static SDL_AtomicInt s_import_done;
static const char *s_import_error;
static char s_import_path[1024];

static void import_progress(double f, void *ctx)
{
    (void)ctx;
    const int pm = (int)(f * 1000);
    if (opt.headless && pm / 10 != SDL_GetAtomicInt(&s_progress) / 10) fprintf(stderr, "\ra2: import %3d %%", pm / 10);
    SDL_SetAtomicInt(&s_progress, pm);
}

static int SDLCALL import_thread(void *ud)
{
    (void)ud;
    s_import_error = a2n_disc_import(s_import_path, s_data, import_progress, NULL);
    SDL_SetAtomicInt(&s_import_done, 1);
    return 0;
}

static SDL_AtomicInt s_dialog_state;   /* 0 waiting, 1 chosen, 2 cancelled */
static char s_dialog_path[1024];

static void SDLCALL dialog_done(void *ud, const char *const *files, int filter)
{
    (void)ud;
    (void)filter;
    if (files && files[0]) {
        snprintf(s_dialog_path, sizeof s_dialog_path, "%s", files[0]);
        SDL_SetAtomicInt(&s_dialog_state, 1);
    } else {
        SDL_SetAtomicInt(&s_dialog_state, 2);
    }
}

static void draw_text_screen(const char *const *lines, int n, int progress)
{
    SDL_SetRenderDrawColor(s_ren, 8, 12, 32, 255);
    SDL_RenderClear(s_ren);
    SDL_SetRenderDrawColor(s_ren, 230, 230, 230, 255);
    for (int i = 0; i < n; ++i) SDL_RenderDebugText(s_ren, 32, (float)(48 + i * 16), lines[i]);
    if (progress >= 0) {
        const SDL_FRect frame = { 32, 400, 576, 20 }, bar = { 34, 402, 572.0f * (float)progress / 1000.0f, 16 };
        SDL_RenderRect(s_ren, &frame);
        SDL_SetRenderDrawColor(s_ren, 80, 160, 255, 255);
        SDL_RenderFillRect(s_ren, &bar);
    }
    SDL_RenderPresent(s_ren);
}

/* import `image` into the data folder, with a progress bar; 1 on success */
static int run_import(const char *image)
{
    snprintf(s_import_path, sizeof s_import_path, "%s", image);
    SDL_SetAtomicInt(&s_progress, 0);
    SDL_SetAtomicInt(&s_import_done, 0);
    s_import_error = NULL;
    if (opt.headless) {
        import_thread(NULL);
        fprintf(stderr, "\n");
    } else {
        SDL_Thread *t = SDL_CreateThread(import_thread, "import", NULL);
        while (!SDL_GetAtomicInt(&s_import_done)) {
            SDL_Event e;
            while (SDL_PollEvent(&e))
                if (e.type == SDL_EVENT_QUIT) { SDL_Quit(); exit(0); }
            char pct[64];
            snprintf(pct, sizeof pct, "Import en cours : %d %%", SDL_GetAtomicInt(&s_progress) / 10);
            const char *lines[] = { "Alundra 2 - installation", "", "Copie des fichiers de votre disque...", "",
                                    pct, "", "L'image du disque ne sera plus necessaire ensuite." };
            draw_text_screen(lines, 7, SDL_GetAtomicInt(&s_progress));
            SDL_Delay(30);
        }
        SDL_WaitThread(t, NULL);
    }
    if (s_import_error) {
        message(1, "Import impossible : %s", s_import_error);
        return 0;
    }
    return 1;
}

static void first_launch(void)
{
    open_window();
    static const SDL_DialogFileFilter filters[] = {
        { "Image de disque PlayStation (.cue, .bin)", "cue;bin" },
        { "Tous les fichiers", "*" },
    };
    for (;;) {
        SDL_SetAtomicInt(&s_dialog_state, 0);
        int asked = 0;
        for (;;) {
            SDL_Event e;
            int go = 0;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) { SDL_Quit(); exit(0); }
                if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                    e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                    go = 1;
            }
            if (go && !asked) {
                asked = 1;
                SDL_ShowOpenFileDialog(dialog_done, NULL, s_win, filters, 2, NULL, false);
            }
            const int st = SDL_GetAtomicInt(&s_dialog_state);
            if (st == 1) break;
            if (st == 2) { asked = 0; SDL_SetAtomicInt(&s_dialog_state, 0); }
            const char *lines[] = {
                "Alundra 2 - premier lancement", "",
                "Le jeu utilise les fichiers de VOTRE disque :", "Alundra 2 version francaise (SLES-02601).", "",
                "Faites une image du disque (.cue + .bin, mode 2/2352)", "avec votre lecteur, puis choisissez-la.", "",
                "Appuyez sur une touche pour choisir le fichier .cue", "", "Les fichiers seront copies dans :", s_data,
            };
            draw_text_screen(lines, 12, -1);
            SDL_Delay(30);
        }
        if (run_import(s_dialog_path) && a2n_disc_open(s_data)) return;
    }
}

/* --verify-disc IMAGE.bin: compare every sector the drive would read with the image */
static int verify_disc(const char *bin)
{
    FILE *f = fopen(bin, "rb");
    if (!f) { fprintf(stderr, "a2: cannot open %s\n", bin); return 1; }
    uint8_t a[2352], b[2352];
    uint32_t data_bad = 0, header_bad = 0, missing = 0;
    const uint32_t n = a2n_disc_sectors();
    for (uint32_t lba = 0; lba < n; ++lba) {
        if (fread(a, 1, sizeof a, f) != sizeof a) { missing++; break; }
        if (!a2n_disc_read(lba, b)) { missing++; continue; }
        if (memcmp(a + 24, b + 24, 2048)) {
            if (data_bad++ < 5) fprintf(stderr, "a2: sector %u: data differs\n", lba);
        } else if (memcmp(a, b, 24)) {
            if (header_bad++ < 8) {
                fprintf(stderr, "a2: sector %u header:", lba);
                for (int i = 12; i < 24; ++i) fprintf(stderr, " %02x/%02x", a[i], b[i]);
                fprintf(stderr, "\n");
            }
        }
    }
    fclose(f);
    fprintf(stderr, "a2: %u sectors: %u with different data, %u with a different header only, %u unreadable\n", n,
            data_bad, header_bad, missing);
    return data_bad || missing ? 1 : 0;
}

/* ---------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
            "usage: alundra2 [--data DIR] [--saves DIR] [--import IMAGE.cue] [--fullscreen]\n"
            "                [--scale N] [--smooth] [--vsync] [--headless] [--frames N]\n"
            "                [--dump-vram FILE] [--dump-ram FILE]\n");
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--headless")) opt.headless = 1;
        else if (!strcmp(a, "--fullscreen")) opt.fullscreen = 1;
        else if (!strcmp(a, "--smooth")) opt.smooth = 1;
        else if (!strcmp(a, "--vsync")) opt.vsync = 1;
        else if (!strcmp(a, "--data") && v) opt.data_opt = argv[++i];
        else if (!strcmp(a, "--saves") && v) opt.saves_opt = argv[++i];
        else if (!strcmp(a, "--import") && v) opt.import = argv[++i];
        else if (!strcmp(a, "--scale") && v) opt.scale = atoi(argv[++i]);
        else if (!strcmp(a, "--frames") && v) opt.frames = atol(argv[++i]);
        else if (!strcmp(a, "--dump-vram") && v) opt.dump_vram = argv[++i];
        else if (!strcmp(a, "--dump-ram") && v) opt.dump_ram = argv[++i];
        else if (!strcmp(a, "--shot-every") && v) opt.shot_every = atol(argv[++i]);
        else if (!strcmp(a, "--input") && v) load_input_script(argv[++i]);
        else if (!strcmp(a, "--verify-disc") && v) opt.verify = argv[++i];
        else if (!strcmp(a, "--self-test")) opt.self_test = 1;
        else if (!strcmp(a, "--dump-audio") && v) {
            s_wav = fopen(argv[++i], "wb");
            if (s_wav) wav_header(s_wav, 0);
        }
        else if (!strcmp(a, "--shot-prefix") && v) opt.shot_prefix = argv[++i];
        else if (!strncmp(a, "-psn_", 5)) { }                      /* macOS Finder */
        else { usage(); return 2; }
    }
    if (opt.scale < 1) opt.scale = 1;
    if (!SDL_Init(opt.headless ? 0 : SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "a2: SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetAppMetadata(TITLE, "1.0", "alundra2-fr");
    choose_dirs();

    if (opt.self_test) return a2n_card_self_test();
    if (opt.import && !run_import(opt.import)) return 1;
    if (!a2n_disc_open(s_data)) {
        if (opt.headless) {
            fprintf(stderr, "a2: no imported disc in %s (use --import alundra.cue)\n", s_data);
            return 1;
        }
        first_launch();
    }
    if (opt.verify) return verify_disc(opt.verify);
    size_t size = 0;
    void *exe = a2n_host_load("files/SLES_026.01", &size);
    if (!exe || !a2n_load_exe(exe, size)) {
        message(1, "Le dossier de données %s est incomplet (SLES_026.01). Supprimez-le et relancez le jeu.", s_data);
        return 1;
    }
    SDL_free(exe);

    open_window();
    open_audio();
    open_first_gamepad();
    if (!a2n_deterministic()) SDL_AddTimerNS(4000000, tick, NULL);
    if (getenv("A2_WATCHDOG")) {
        s_watch_last = SDL_GetTicks();
        SDL_AddTimer((Uint32)(atoi(getenv("A2_WATCHDOG")) * 1000), watchdog, NULL);
    }
    a2n_run();
    return 0;
}
