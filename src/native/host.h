/* host.h - what the engine needs from the platform (host.c, SDL3) */
#ifndef A2N_HOST_H
#define A2N_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t a2n_host_now_ns(void);
void a2n_host_sleep_until(uint64_t ns);
void a2n_host_vblank(void);                     /* events, picture */
void a2n_host_audio(const int16_t *stereo, int frames);   /* 44100 Hz */

typedef struct {
    int connected, analog;
    uint16_t buttons;                           /* PlayStation bits, 1 = pressed */
    uint8_t lx, ly, rx, ry;                     /* 0x80 = centre */
} A2nPadState;
void a2n_host_pad(int port, A2nPadState *out);
void a2n_host_rumble(int port, uint8_t small, uint8_t big);

/* the game's files, extracted from the player's disc (phase 5) */
const char *a2n_host_data_dir(void);
void *a2n_host_load(const char *relative, size_t *size);
const char *a2n_host_save_dir(void);

#ifdef __cplusplus
}
#endif

#endif
