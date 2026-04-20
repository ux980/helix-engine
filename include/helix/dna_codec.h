#ifndef HELIX_DNA_CODEC_H
#define HELIX_DNA_CODEC_H

#include <stdint.h>

typedef struct {
    char last_base;
} dna_state_t;

void dna_state_init(dna_state_t *state);
int dna_base_to_index(char base);
int dna_base_is_valid(char base);
char dna_index_to_base(int index);

void dna_encode_byte_diff(uint8_t byte, dna_state_t *state, char out_quad[4]);
uint8_t dna_decode_byte_diff(const char in_quad[4], dna_state_t *state);
int dna_decode_byte_diff_checked(const char in_quad[4], dna_state_t *state, uint8_t *out_byte);

#endif
