/* native_modules.h - the native replacements of the console and of the PsyQ library */
#ifndef A2N_NATIVE_MODULES_H
#define A2N_NATIVE_MODULES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* libetc.c */
void a2n_libetc_init(void);
int a2n_deterministic(void);
uint64_t a2n_vblank_count(void);
void a2n_idle(void);
uint32_t a2n_dma_callback(int channel);

/* bios.c, libc.c */
void a2n_bios_init(void);
void a2n_libc_init(void);

/* gpu.c, libgpu.c */
void a2n_gpu_init(void);
void a2n_gpu_event(void);
const uint16_t *a2n_gpu_vram(void);
void a2n_gpu_display(int *x, int *y, int *w, int *h, int *depth24, int *enabled);

/* cd.c */
void a2n_cd_init(void);
void a2n_cd_vblank(void);
void a2n_cd_event(void);

/* spu.c */
void a2n_spu_init(void);
void a2n_spu_vblank(void);
void a2n_spu_cd_audio(const int16_t *stereo, int frames, int rate);

/* mdec.c */
void a2n_mdec_init(void);

/* io.c: DMA, interrupt controller, root counters */
void a2n_io_init(void);
void a2n_dma_event(void);
void a2n_dma_complete(int channel);
void a2n_dma_irq_enable(int channel, int on);

/* pad.c, card.c */
void a2n_pad_init(void);
void a2n_pad_vblank(void);
void a2n_card_init(void);
int a2n_card_self_test(void);

#ifdef __cplusplus
}
#endif

#endif
