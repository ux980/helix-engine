#ifndef HELIX_RS_H
#define HELIX_RS_H

#include <stddef.h>
#include <stdint.h>
#include "helix/config.h"

void rs_init(void);
void rs_encode_block(const uint8_t *data, uint8_t *parity);
int rs_calc_syndromes(const uint8_t *block, uint8_t *syn);
int rs_correct_errors(uint8_t *block, const uint8_t *syn);

#endif