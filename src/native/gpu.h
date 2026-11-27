/* gpu.h - software GPU (gpu.c) */
#ifndef A2N_GPU_H
#define A2N_GPU_H

#include <stdint.h>

#define VRAM_W 1024
#define VRAM_H 512

typedef struct {
    int tex_x, tex_y, semi_mode, tex_depth, dither, draw_display, tex_disable, flip_x, flip_y;
    uint32_t texpage_raw;
    int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    int area_x0, area_y0, area_x1, area_y1;
    int off_x, off_y;
    int mask_set, mask_check;
} GpuState;

extern uint16_t g_vram[VRAM_W * VRAM_H];
extern GpuState g_gpu;

void gpu_reset(void);
void gpu_set_texpage(uint32_t w);
int gpu_command(const uint32_t *words, int n);
void gpu_fill(int x, int y, int w, int h, uint32_t color);
void gpu_copy(int sx, int sy, int dx, int dy, int w, int h);
void gpu_load(int x, int y, int w, int h, const uint16_t *src);
void gpu_store(int x, int y, int w, int h, uint16_t *dst);

#endif
