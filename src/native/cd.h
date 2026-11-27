/* cd.h - CD-ROM drive (cd.c), disc sectors (disc.c), XA audio (xa.c) */
#ifndef A2N_CD_H
#define A2N_CD_H

#include <stdint.h>

void a2n_cd_dma(uint32_t addr, uint32_t words);
int a2n_disc_open(const char *data_dir);
int a2n_disc_read(uint32_t lba, uint8_t *raw);
uint32_t a2n_disc_sectors(void);

#endif
