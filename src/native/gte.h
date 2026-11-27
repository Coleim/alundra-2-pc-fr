/* gte.h - the geometry coprocessor in software (gte.c) */
#ifndef A2_GTE_H
#define A2_GTE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* data[32] and ctrl[32]: the coprocessor's registers */
void a2gte_execute(uint32_t *data, uint32_t *ctrl, uint32_t cmd);
uint32_t a2gte_read_data(const uint32_t *data, int reg);
void a2gte_write_data(uint32_t *data, int reg, uint32_t value);
uint32_t a2gte_read_ctrl(const uint32_t *ctrl, int reg);
void a2gte_write_ctrl(uint32_t *ctrl, int reg, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif
