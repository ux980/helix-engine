#ifndef HELIX_OLIGO_CONSTRAINTS_H
#define HELIX_OLIGO_CONSTRAINTS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t gc_count;
    uint32_t gc_percent_times_100;
    uint32_t max_homopolymer;
} helix_constraint_stats_t;

void helix_constraint_mask_apply(uint8_t *data, size_t len, uint8_t seed);
int helix_dna_constraints_ok(
    const char *dna,
    size_t dna_len,
    uint32_t gc_min_percent,
    uint32_t gc_max_percent,
    uint32_t max_homopolymer,
    helix_constraint_stats_t *out_stats
);

#endif
