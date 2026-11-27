/* disc.h - the player's disc, imported as files (disc.c) */
#ifndef A2N_DISC_H
#define A2N_DISC_H

#include <stdint.h>

/* FNV-1a 64 of SLES_026.01 of the French release (SLES-02601) */
#define A2N_EXE_FNV 0x7CE2B92537AA1490ull

/* returns NULL on success, or a message */
const char *a2n_disc_import(const char *image, const char *data_dir, void (*progress)(double, void *), void *ctx);

#endif
