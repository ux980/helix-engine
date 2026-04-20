#ifndef HELIX_GF256_H
#define HELIX_GF256_H

#include <stdint.h>

void gf256_init(void);
uint8_t gf256_mul(uint8_t a, uint8_t b);
uint8_t gf256_div(uint8_t a, uint8_t b);
uint8_t gf256_pow_exp(int index);

#endif