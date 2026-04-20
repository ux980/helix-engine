#include "helix/oligo_constraints.h"

static uint8_t constraint_mask_byte(uint8_t seed, uint32_t pos) {
    uint32_t x = ((uint32_t)seed << 24) ^ (UINT32_C(0x9E3779B9) * (pos + 1U)) ^ UINT32_C(0xA5A5A5A5);

    x ^= x >> 16;
    x *= UINT32_C(0x7FEB352D);
    x ^= x >> 15;
    x *= UINT32_C(0x846CA68B);
    x ^= x >> 16;

    return (uint8_t)x;
}

void helix_constraint_mask_apply(uint8_t *data, size_t len, uint8_t seed) {
    if (!data) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        data[i] ^= constraint_mask_byte(seed, (uint32_t)i);
    }
}

int helix_dna_constraints_ok(
    const char *dna,
    size_t dna_len,
    uint32_t gc_min_percent,
    uint32_t gc_max_percent,
    uint32_t max_homopolymer,
    helix_constraint_stats_t *out_stats
) {
    uint32_t gc_count = 0U;
    uint32_t best_run = 0U;
    uint32_t run = 0U;
    char last = '\0';

    if (!dna || dna_len == 0U) {
        return 0;
    }

    for (size_t i = 0; i < dna_len; i++) {
        char base = dna[i];

        if (base == 'G' || base == 'C') {
            gc_count++;
        } else if (base != 'A' && base != 'T') {
            return 0;
        }

        if (base == last) {
            run++;
        } else {
            last = base;
            run = 1U;
        }

        if (run > best_run) {
            best_run = run;
        }
    }

    if (out_stats) {
        out_stats->gc_count = gc_count;
        out_stats->gc_percent_times_100 = (gc_count * 10000U) / (uint32_t)dna_len;
        out_stats->max_homopolymer = best_run;
    }

    if ((gc_count * 100U) < (gc_min_percent * (uint32_t)dna_len) ||
        (gc_count * 100U) > (gc_max_percent * (uint32_t)dna_len)) {
        return 0;
    }

    return best_run <= max_homopolymer;
}
