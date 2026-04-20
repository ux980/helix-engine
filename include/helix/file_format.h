#ifndef HELIX_FILE_FORMAT_H
#define HELIX_FILE_FORMAT_H

#include <stdint.h>
#include <stdio.h>
#include "helix/status.h"

typedef struct {
    uint32_t version;
    uint64_t original_size;
    uint32_t crc32;
    uint32_t header_size;
    int has_crc32;
    int is_indexed;
    uint32_t oligo_count;
    uint32_t oligo_payload_bytes;
    uint32_t parity_data_oligos;
    uint32_t parity_oligos_per_stripe;
    uint32_t gc_min_percent;
    uint32_t gc_max_percent;
    uint32_t max_homopolymer;
} helix_header_t;

helix_status_t helix_write_header(FILE *fp, uint64_t original_size);
helix_status_t helix_write_header_with_crc(FILE *fp, uint64_t original_size, uint32_t crc32);
helix_status_t helix_write_header_v3(
    FILE *fp,
    uint64_t original_size,
    uint32_t crc32,
    uint32_t oligo_count,
    uint32_t oligo_payload_bytes
);
helix_status_t helix_write_header_v4(
    FILE *fp,
    uint64_t original_size,
    uint32_t crc32,
    uint32_t oligo_count,
    uint32_t oligo_payload_bytes,
    uint32_t parity_data_oligos
);
helix_status_t helix_write_header_v5(
    FILE *fp,
    uint64_t original_size,
    uint32_t crc32,
    uint32_t oligo_count,
    uint32_t oligo_payload_bytes,
    uint32_t parity_data_oligos,
    uint32_t parity_oligos_per_stripe,
    uint32_t gc_min_percent,
    uint32_t gc_max_percent,
    uint32_t max_homopolymer
);
helix_status_t helix_read_header(FILE *fp, uint64_t *original_size);
helix_status_t helix_read_header_info(FILE *fp, helix_header_t *header);

#endif
