#include "helix/dna_codec.h"

static const char DNA_MAP[4] = { 'A', 'C', 'G', 'T' };

void dna_state_init(dna_state_t *state) {
    if (state) {
        state->last_base = 'A';
    }
}

int dna_base_to_index(char base) {
    if (base == 'A') return 0;
    if (base == 'C') return 1;
    if (base == 'G') return 2;
    if (base == 'T') return 3;
    return -1;
}

int dna_base_is_valid(char base) {
    return dna_base_to_index(base) >= 0;
}

char dna_index_to_base(int index) {
    index &= 0x03;
    return DNA_MAP[index];
}

void dna_encode_byte_diff(uint8_t byte, dna_state_t *state, char out_quad[4]) {
    for (int j = 3; j >= 0; j--) {
        int bits = (byte >> (j * 2)) & 0x03;
        int current_idx = dna_base_to_index(state->last_base);
        int next_idx = (current_idx + bits + 1) % 4;

        state->last_base = dna_index_to_base(next_idx);
        out_quad[3 - j] = state->last_base;
    }
}

uint8_t dna_decode_byte_diff(const char in_quad[4], dna_state_t *state) {
    uint8_t byte = 0;
    if (!dna_decode_byte_diff_checked(in_quad, state, &byte)) {
        return 0;
    }
    return byte;
}

int dna_decode_byte_diff_checked(const char in_quad[4], dna_state_t *state, uint8_t *out_byte) {
    uint8_t byte = 0;

    if (!in_quad || !state || !out_byte) {
        return 0;
    }

    for (int j = 0; j < 4; j++) {
        int current_idx = dna_base_to_index(state->last_base);
        int next_idx = dna_base_to_index(in_quad[j]);

        if (current_idx < 0 || next_idx < 0) {
            return 0;
        }

        int diff = next_idx - current_idx - 1;
        if (diff < 0) {
            diff += 4;
        }

        byte |= (uint8_t)((diff & 0x03) << (6 - (j * 2)));
        state->last_base = in_quad[j];
    }

    *out_byte = byte;
    return 1;
}
