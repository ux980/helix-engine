#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "helix/codec.h"
#include "helix/config.h"
#include "helix/crc32.h"
#include "helix/dna_codec.h"
#include "helix/file_format.h"
#include "helix/oligo_constraints.h"
#include "helix/rs.h"
#include "helix/gf256.h"

static void store_u32_be(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)((value >> 24) & 0xFFU);
    out[1] = (uint8_t)((value >> 16) & 0xFFU);
    out[2] = (uint8_t)((value >> 8) & 0xFFU);
    out[3] = (uint8_t)(value & 0xFFU);
}

static void store_u16_be(uint8_t out[2], uint16_t value) {
    out[0] = (uint8_t)((value >> 8) & 0xFFU);
    out[1] = (uint8_t)(value & 0xFFU);
}

static uint32_t load_u32_be(const uint8_t in[4]) {
    return ((uint32_t)in[0] << 24) |
           ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) |
           (uint32_t)in[3];
}

static uint16_t load_u16_be(const uint8_t in[2]) {
    return (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
}

static uint32_t ceil_div_u64_to_u32(uint64_t num, uint32_t den) {
    if (den == 0U) {
        return 0U;
    }

    return (uint32_t)((num + den - 1U) / den);
}

static uint32_t expected_data_oligo_count(uint64_t original_size, uint32_t payload_bytes) {
    return ceil_div_u64_to_u32(original_size, payload_bytes);
}

static uint32_t expected_stripe_count(uint32_t data_oligo_count, uint32_t stripe_width) {
    return ceil_div_u64_to_u32(data_oligo_count, stripe_width);
}

static uint16_t expected_payload_len(uint64_t original_size, uint32_t index, uint32_t payload_bytes) {
    uint64_t offset = (uint64_t)index * (uint64_t)payload_bytes;
    uint64_t remaining;

    if (offset >= original_size) {
        return 0U;
    }

    remaining = original_size - offset;
    if (remaining > payload_bytes) {
        remaining = payload_bytes;
    }

    return (uint16_t)remaining;
}

static uint16_t expected_stripe_members(uint32_t data_oligo_count, uint32_t stripe_width, uint32_t stripe_index) {
    uint32_t start = stripe_index * stripe_width;
    uint32_t remaining;

    if (start >= data_oligo_count) {
        return 0U;
    }

    remaining = data_oligo_count - start;
    if (remaining > stripe_width) {
        remaining = stripe_width;
    }

    return (uint16_t)remaining;
}

static uint8_t parity_coefficient_for_member(uint32_t member_index) {
    return (uint8_t)(member_index + 1U);
}

static helix_status_t get_file_size(FILE *fp, uint64_t *out_size) {
    long saved_pos;
    long size;

    if (!fp || !out_size) {
        return HELIX_ERR_INVALID_ARG;
    }

    saved_pos = ftell(fp);
    if (saved_pos < 0) {
        return HELIX_ERR_FILE_READ;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        return HELIX_ERR_FILE_READ;
    }

    size = ftell(fp);
    if (size < 0) {
        (void)fseek(fp, saved_pos, SEEK_SET);
        return HELIX_ERR_FILE_READ;
    }

    if (fseek(fp, saved_pos, SEEK_SET) != 0) {
        return HELIX_ERR_FILE_READ;
    }

    *out_size = (uint64_t)size;
    return HELIX_OK;
}

static helix_status_t calculate_file_crc32(FILE *fp, uint32_t *out_crc32) {
    uint8_t buffer[4096];
    size_t nread;
    uint32_t crc;

    if (!fp || !out_crc32) {
        return HELIX_ERR_INVALID_ARG;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        return HELIX_ERR_FILE_READ;
    }

    crc = helix_crc32_init();

    while ((nread = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        crc = helix_crc32_update(crc, buffer, nread);
    }

    if (ferror(fp)) {
        return HELIX_ERR_FILE_READ;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        return HELIX_ERR_FILE_READ;
    }

    *out_crc32 = helix_crc32_finish(crc);
    return HELIX_OK;
}

static helix_status_t calculate_path_crc32(const char *path, uint32_t *out_crc32) {
    FILE *fp;
    helix_status_t st;

    if (!path || !out_crc32) {
        return HELIX_ERR_INVALID_ARG;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        return HELIX_ERR_FILE_OPEN;
    }

    st = calculate_file_crc32(fp, out_crc32);
    fclose(fp);
    return st;
}

static char *make_temp_output_path(const char *output_path) {
    size_t len;
    char *temp_path;

    if (!output_path) {
        return NULL;
    }

    len = strlen(output_path);
    temp_path = (char *)malloc(len + 6U);
    if (!temp_path) {
        return NULL;
    }

    memcpy(temp_path, output_path, len);
    memcpy(temp_path + len, ".part", 6U);
    return temp_path;
}

static helix_status_t commit_temp_output(const char *temp_path, const char *output_path) {
    if (!temp_path || !output_path) {
        return HELIX_ERR_INVALID_ARG;
    }

    remove(output_path);
    if (rename(temp_path, output_path) != 0) {
        return HELIX_ERR_FILE_WRITE;
    }

    return HELIX_OK;
}

static void build_dna_line_from_message(const uint8_t message[HELIX_RS_K], char dna_line[HELIX_OLIGO_LINE_CHARS]) {
    uint8_t block[HELIX_RS_N];
    uint8_t parity[HELIX_RS_T];
    dna_state_t state;

    memcpy(block, message, HELIX_RS_K);
    rs_encode_block(message, parity);
    memcpy(block + HELIX_RS_K, parity, HELIX_RS_T);

    dna_state_init(&state);
    for (size_t i = 0; i < HELIX_RS_N; i++) {
        dna_encode_byte_diff(block[i], &state, &dna_line[i * 4U]);
    }
    dna_line[HELIX_OLIGO_DNA_CHARS] = '\n';
}

static helix_status_t write_dna_line(const char dna_line[HELIX_OLIGO_LINE_CHARS], FILE *fo) {
    if (!dna_line || !fo) {
        return HELIX_ERR_INVALID_ARG;
    }

    if (fwrite(dna_line, 1, HELIX_OLIGO_LINE_CHARS, fo) != HELIX_OLIGO_LINE_CHARS) {
        return HELIX_ERR_FILE_WRITE;
    }

    return HELIX_OK;
}

static helix_status_t write_v5_message_line(
    const uint8_t logical_message[HELIX_RS_K],
    FILE *fo,
    const helix_header_t *header
) {
    uint8_t encoded_message[HELIX_RS_K];
    char dna_line[HELIX_OLIGO_LINE_CHARS];
    uint32_t raw_index;
    uint8_t base_seed;

    if (!logical_message || !fo || !header) {
        return HELIX_ERR_INVALID_ARG;
    }

    raw_index = load_u32_be(logical_message);
    base_seed = (uint8_t)(raw_index ^ (raw_index >> 8) ^ (raw_index >> 16) ^ (raw_index >> 24) ^
                          logical_message[HELIX_OLIGO_INDEX_BYTES] ^
                          logical_message[HELIX_OLIGO_INDEX_BYTES + 1U]);

    for (uint32_t attempt = 0U; attempt < 256U; attempt++) {
        uint8_t seed = (uint8_t)(base_seed + (attempt * 29U));

        memcpy(encoded_message, logical_message, HELIX_RS_K);
        encoded_message[HELIX_OLIGO_INDEX_BYTES + HELIX_OLIGO_LENGTH_BYTES] = seed;
        helix_constraint_mask_apply(
            encoded_message + HELIX_OLIGO_META_BYTES,
            HELIX_OLIGO_PAYLOAD_BYTES,
            seed
        );

        build_dna_line_from_message(encoded_message, dna_line);
        if (helix_dna_constraints_ok(
                dna_line,
                HELIX_OLIGO_DNA_CHARS,
                header->gc_min_percent,
                header->gc_max_percent,
                header->max_homopolymer,
                NULL
            )) {
            return write_dna_line(dna_line, fo);
        }
    }

    return HELIX_ERR_CONSTRAINT;
}

static helix_status_t read_next_oligo_line(FILE *fi, char dna_line[HELIX_OLIGO_LINE_CHARS], int *has_line) {
    int first_char;
    size_t tail_read;

    if (!fi || !dna_line || !has_line) {
        return HELIX_ERR_INVALID_ARG;
    }

    first_char = fgetc(fi);
    if (first_char == EOF) {
        if (ferror(fi)) {
            return HELIX_ERR_FILE_READ;
        }

        *has_line = 0;
        return HELIX_OK;
    }

    dna_line[0] = (char)first_char;
    tail_read = fread(dna_line + 1, 1, HELIX_OLIGO_LINE_CHARS - 1U, fi);
    if (tail_read != HELIX_OLIGO_LINE_CHARS - 1U) {
        return HELIX_ERR_FILE_READ;
    }

    if (dna_line[HELIX_OLIGO_DNA_CHARS] != '\n') {
        return HELIX_ERR_FILE_READ;
    }

    *has_line = 1;
    return HELIX_OK;
}

static helix_status_t decode_dna_line_to_block(const char dna_line[HELIX_OLIGO_LINE_CHARS], uint8_t block[HELIX_RS_N]) {
    dna_state_t state;
    uint8_t syndromes[HELIX_RS_T];

    if (!dna_line || !block) {
        return HELIX_ERR_INVALID_ARG;
    }

    if (dna_line[HELIX_OLIGO_DNA_CHARS] != '\n') {
        return HELIX_ERR_FILE_READ;
    }

    dna_state_init(&state);
    for (size_t i = 0; i < HELIX_RS_N; i++) {
        if (!dna_decode_byte_diff_checked(&dna_line[i * 4U], &state, &block[i])) {
            return HELIX_ERR_BAD_DNA;
        }
    }

    if (rs_calc_syndromes(block, syndromes)) {
        if (!rs_correct_errors(block, syndromes)) {
            return HELIX_ERR_UNCORRECTABLE;
        }
    }

    return HELIX_OK;
}

static helix_status_t decode_legacy_stream(FILE *fi, FILE *fo, uint64_t original_size) {
    char dna_block[HELIX_RS_N * 4U];
    uint8_t block[HELIX_RS_N];
    dna_state_t state;
    uint8_t syndromes[HELIX_RS_T];
    uint64_t remaining = original_size;

    dna_state_init(&state);

    while (remaining > 0U) {
        uint64_t chunk = remaining > HELIX_RS_K ? HELIX_RS_K : remaining;

        if (fread(dna_block, 1, sizeof(dna_block), fi) != sizeof(dna_block)) {
            return HELIX_ERR_FILE_READ;
        }

        for (size_t i = 0; i < HELIX_RS_N; i++) {
            if (!dna_decode_byte_diff_checked(&dna_block[i * 4U], &state, &block[i])) {
                return HELIX_ERR_BAD_DNA;
            }
        }

        if (rs_calc_syndromes(block, syndromes)) {
            if (!rs_correct_errors(block, syndromes)) {
                return HELIX_ERR_UNCORRECTABLE;
            }
        }

        if (fwrite(block, 1, (size_t)chunk, fo) != chunk) {
            return HELIX_ERR_FILE_WRITE;
        }

        remaining -= chunk;
    }

    return HELIX_OK;
}

static helix_status_t decode_indexed_v3(FILE *fi, FILE *fo, const helix_header_t *header) {
    uint32_t data_count;
    uint32_t payload_bytes;
    uint8_t *payloads = NULL;
    uint16_t *lengths = NULL;
    uint8_t *present = NULL;
    char dna_line[HELIX_OLIGO_LINE_CHARS];
    uint8_t block[HELIX_RS_N];
    int has_line = 0;
    helix_status_t st = HELIX_OK;

    if (!fi || !fo || !header) {
        return HELIX_ERR_INVALID_ARG;
    }

    payload_bytes = header->oligo_payload_bytes;
    data_count = expected_data_oligo_count(header->original_size, payload_bytes);
    if (data_count != header->oligo_count || payload_bytes != HELIX_V4_OLIGO_PAYLOAD_BYTES) {
        return HELIX_ERR_BAD_HEADER;
    }

    if (data_count > 0U) {
        payloads = (uint8_t *)calloc((size_t)data_count, payload_bytes);
        lengths = (uint16_t *)calloc(data_count, sizeof(uint16_t));
        present = (uint8_t *)calloc(data_count, 1U);
        if (!payloads || !lengths || !present) {
            st = HELIX_ERR_INTERNAL;
            goto cleanup;
        }
    }

    while ((st = read_next_oligo_line(fi, dna_line, &has_line)) == HELIX_OK && has_line) {
        uint32_t index;
        uint16_t payload_len;
        uint16_t expected_len;

        st = decode_dna_line_to_block(dna_line, block);
        if (st != HELIX_OK) {
            goto cleanup;
        }

        index = load_u32_be(block);
        payload_len = load_u16_be(block + HELIX_OLIGO_INDEX_BYTES);

        if ((index & HELIX_OLIGO_INDEX_PARITY_FLAG) != 0U || index >= data_count) {
            st = HELIX_ERR_BAD_OLIGO;
            goto cleanup;
        }

        expected_len = expected_payload_len(header->original_size, index, payload_bytes);
        if (payload_len != expected_len) {
            st = HELIX_ERR_BAD_OLIGO;
            goto cleanup;
        }

        if (present[index]) {
            if (lengths[index] != payload_len ||
                memcmp(payloads + ((size_t)index * payload_bytes),
                       block + HELIX_V4_OLIGO_META_BYTES,
                       payload_bytes) != 0) {
                st = HELIX_ERR_BAD_OLIGO;
                goto cleanup;
            }
            continue;
        }

        memcpy(payloads + ((size_t)index * payload_bytes), block + HELIX_V4_OLIGO_META_BYTES, payload_bytes);
        lengths[index] = payload_len;
        present[index] = 1U;
    }

    if (st != HELIX_OK) {
        goto cleanup;
    }

    for (uint32_t index = 0; index < data_count; index++) {
        if (!present[index]) {
            st = HELIX_ERR_BAD_OLIGO;
            goto cleanup;
        }

        if (lengths[index] > 0U &&
            fwrite(payloads + ((size_t)index * payload_bytes), 1, lengths[index], fo) != lengths[index]) {
            st = HELIX_ERR_FILE_WRITE;
            goto cleanup;
        }
    }

cleanup:
    free(payloads);
    free(lengths);
    free(present);
    return st;
}

static helix_status_t decode_indexed_v4(FILE *fi, FILE *fo, const helix_header_t *header) {
    uint32_t data_count;
    uint32_t payload_bytes;
    uint32_t stripe_width;
    uint32_t stripe_count;
    uint8_t *data_payloads = NULL;
    uint16_t *data_lengths = NULL;
    uint8_t *data_present = NULL;
    uint8_t *parity_payloads = NULL;
    uint16_t *parity_members = NULL;
    uint8_t *parity_present = NULL;
    char dna_line[HELIX_OLIGO_LINE_CHARS];
    uint8_t block[HELIX_RS_N];
    int has_line = 0;
    helix_status_t st = HELIX_OK;

    if (!fi || !fo || !header) {
        return HELIX_ERR_INVALID_ARG;
    }

    payload_bytes = header->oligo_payload_bytes;
    stripe_width = header->parity_data_oligos;
    data_count = expected_data_oligo_count(header->original_size, payload_bytes);
    stripe_count = expected_stripe_count(data_count, stripe_width);

    if (payload_bytes != HELIX_V4_OLIGO_PAYLOAD_BYTES ||
        stripe_width != HELIX_PARITY_STRIPE_DATA_OLIGOS ||
        data_count != header->oligo_count) {
        return HELIX_ERR_BAD_HEADER;
    }

    if (data_count > 0U) {
        data_payloads = (uint8_t *)calloc((size_t)data_count, payload_bytes);
        data_lengths = (uint16_t *)calloc(data_count, sizeof(uint16_t));
        data_present = (uint8_t *)calloc(data_count, 1U);
        if (!data_payloads || !data_lengths || !data_present) {
            st = HELIX_ERR_INTERNAL;
            goto cleanup;
        }
    }

    if (stripe_count > 0U) {
        parity_payloads = (uint8_t *)calloc((size_t)stripe_count, payload_bytes);
        parity_members = (uint16_t *)calloc(stripe_count, sizeof(uint16_t));
        parity_present = (uint8_t *)calloc(stripe_count, 1U);
        if (!parity_payloads || !parity_members || !parity_present) {
            st = HELIX_ERR_INTERNAL;
            goto cleanup;
        }
    }

    while ((st = read_next_oligo_line(fi, dna_line, &has_line)) == HELIX_OK && has_line) {
        uint32_t raw_index;
        uint16_t payload_len;

        st = decode_dna_line_to_block(dna_line, block);
        if (st != HELIX_OK) {
            goto cleanup;
        }

        raw_index = load_u32_be(block);
        payload_len = load_u16_be(block + HELIX_OLIGO_INDEX_BYTES);

        if ((raw_index & HELIX_OLIGO_INDEX_PARITY_FLAG) != 0U) {
            uint32_t stripe_index = raw_index & ~HELIX_OLIGO_INDEX_PARITY_FLAG;
            uint16_t expected_members;

            if (stripe_index >= stripe_count) {
                st = HELIX_ERR_BAD_OLIGO;
                goto cleanup;
            }

            expected_members = expected_stripe_members(data_count, stripe_width, stripe_index);
            if (payload_len != expected_members) {
                st = HELIX_ERR_BAD_OLIGO;
                goto cleanup;
            }

            if (parity_present[stripe_index]) {
                if (parity_members[stripe_index] != payload_len ||
                    memcmp(parity_payloads + ((size_t)stripe_index * payload_bytes),
                           block + HELIX_V4_OLIGO_META_BYTES,
                           payload_bytes) != 0) {
                    st = HELIX_ERR_BAD_OLIGO;
                    goto cleanup;
                }
                continue;
            }

            memcpy(parity_payloads + ((size_t)stripe_index * payload_bytes),
                   block + HELIX_V4_OLIGO_META_BYTES,
                   payload_bytes);
            parity_members[stripe_index] = payload_len;
            parity_present[stripe_index] = 1U;
        } else {
            uint32_t index = raw_index;
            uint16_t expected_len;

            if (index >= data_count) {
                st = HELIX_ERR_BAD_OLIGO;
                goto cleanup;
            }

            expected_len = expected_payload_len(header->original_size, index, payload_bytes);
            if (payload_len != expected_len) {
                st = HELIX_ERR_BAD_OLIGO;
                goto cleanup;
            }

            if (data_present[index]) {
                if (data_lengths[index] != payload_len ||
                    memcmp(data_payloads + ((size_t)index * payload_bytes),
                           block + HELIX_V4_OLIGO_META_BYTES,
                           payload_bytes) != 0) {
                    st = HELIX_ERR_BAD_OLIGO;
                    goto cleanup;
                }
                continue;
            }

            memcpy(data_payloads + ((size_t)index * payload_bytes), block + HELIX_V4_OLIGO_META_BYTES, payload_bytes);
            data_lengths[index] = payload_len;
            data_present[index] = 1U;
        }
    }

    if (st != HELIX_OK) {
        goto cleanup;
    }

    for (uint32_t stripe_index = 0; stripe_index < stripe_count; stripe_index++) {
        uint32_t stripe_start = stripe_index * stripe_width;
        uint16_t members = expected_stripe_members(data_count, stripe_width, stripe_index);
        uint32_t missing_index = UINT32_MAX;
        uint32_t missing_count = 0U;

        for (uint32_t member = 0; member < members; member++) {
            uint32_t index = stripe_start + member;
            if (!data_present[index]) {
                missing_index = index;
                missing_count++;
            }
        }

        if (missing_count == 0U) {
            continue;
        }

        if (missing_count != 1U || !parity_present[stripe_index]) {
            st = HELIX_ERR_BAD_OLIGO;
            goto cleanup;
        }

        memcpy(data_payloads + ((size_t)missing_index * payload_bytes),
               parity_payloads + ((size_t)stripe_index * payload_bytes),
               payload_bytes);

        for (uint32_t member = 0; member < members; member++) {
            uint32_t index = stripe_start + member;
            if (index == missing_index) {
                continue;
            }

            for (uint32_t byte_index = 0; byte_index < payload_bytes; byte_index++) {
                data_payloads[((size_t)missing_index * payload_bytes) + byte_index] ^=
                    data_payloads[((size_t)index * payload_bytes) + byte_index];
            }
        }

        data_lengths[missing_index] = expected_payload_len(header->original_size, missing_index, payload_bytes);
        data_present[missing_index] = 1U;
    }

    for (uint32_t index = 0; index < data_count; index++) {
        if (!data_present[index]) {
            st = HELIX_ERR_BAD_OLIGO;
            goto cleanup;
        }

        if (data_lengths[index] > 0U &&
            fwrite(data_payloads + ((size_t)index * payload_bytes), 1, data_lengths[index], fo) != data_lengths[index]) {
            st = HELIX_ERR_FILE_WRITE;
            goto cleanup;
        }
    }

cleanup:
    free(data_payloads);
    free(data_lengths);
    free(data_present);
    free(parity_payloads);
    free(parity_members);
    free(parity_present);
    return st;
}

static helix_status_t validate_and_unmask_v5_message(uint8_t message[HELIX_RS_K], const helix_header_t *header) {
    char dna_line[HELIX_OLIGO_LINE_CHARS];
    uint8_t seed;

    if (!message || !header) {
        return HELIX_ERR_INVALID_ARG;
    }

    build_dna_line_from_message(message, dna_line);
    if (!helix_dna_constraints_ok(
            dna_line,
            HELIX_OLIGO_DNA_CHARS,
            header->gc_min_percent,
            header->gc_max_percent,
            header->max_homopolymer,
            NULL
        )) {
        return HELIX_ERR_CONSTRAINT;
    }

    seed = message[HELIX_OLIGO_INDEX_BYTES + HELIX_OLIGO_LENGTH_BYTES];
    helix_constraint_mask_apply(message + HELIX_OLIGO_META_BYTES, HELIX_OLIGO_PAYLOAD_BYTES, seed);
    return HELIX_OK;
}

static helix_status_t decode_indexed_v5(FILE *fi, FILE *fo, const helix_header_t *header) {
    uint32_t data_count;
    uint32_t payload_bytes;
    uint32_t stripe_width;
    uint32_t stripe_count;
    uint32_t parity_count;
    uint8_t *data_payloads = NULL;
    uint16_t *data_lengths = NULL;
    uint8_t *data_present = NULL;
    uint8_t *parity_xor_payloads = NULL;
    uint16_t *parity_xor_members = NULL;
    uint8_t *parity_xor_present = NULL;
    uint8_t *parity_gf_payloads = NULL;
    uint16_t *parity_gf_members = NULL;
    uint8_t *parity_gf_present = NULL;
    char dna_line[HELIX_OLIGO_LINE_CHARS];
    uint8_t block[HELIX_RS_N];
    int has_line = 0;
    helix_status_t st = HELIX_OK;

    if (!fi || !fo || !header) {
        return HELIX_ERR_INVALID_ARG;
    }

    payload_bytes = header->oligo_payload_bytes;
    stripe_width = header->parity_data_oligos;
    parity_count = header->parity_oligos_per_stripe;
    data_count = expected_data_oligo_count(header->original_size, payload_bytes);
    stripe_count = expected_stripe_count(data_count, stripe_width);

    if (payload_bytes != HELIX_OLIGO_PAYLOAD_BYTES ||
        stripe_width != HELIX_PARITY_STRIPE_DATA_OLIGOS ||
        parity_count != HELIX_PARITY_STRIPE_PARITY_OLIGOS ||
        data_count != header->oligo_count ||
        header->gc_min_percent != HELIX_CONSTRAINT_GC_MIN_PERCENT ||
        header->gc_max_percent != HELIX_CONSTRAINT_GC_MAX_PERCENT ||
        header->max_homopolymer != HELIX_CONSTRAINT_MAX_HOMOPOLYMER) {
        return HELIX_ERR_BAD_HEADER;
    }

    if (data_count > 0U) {
        data_payloads = (uint8_t *)calloc((size_t)data_count, payload_bytes);
        data_lengths = (uint16_t *)calloc(data_count, sizeof(uint16_t));
        data_present = (uint8_t *)calloc(data_count, 1U);
        if (!data_payloads || !data_lengths || !data_present) {
            st = HELIX_ERR_INTERNAL;
            goto cleanup;
        }
    }

    if (stripe_count > 0U) {
        parity_xor_payloads = (uint8_t *)calloc((size_t)stripe_count, payload_bytes);
        parity_xor_members = (uint16_t *)calloc(stripe_count, sizeof(uint16_t));
        parity_xor_present = (uint8_t *)calloc(stripe_count, 1U);
        parity_gf_payloads = (uint8_t *)calloc((size_t)stripe_count, payload_bytes);
        parity_gf_members = (uint16_t *)calloc(stripe_count, sizeof(uint16_t));
        parity_gf_present = (uint8_t *)calloc(stripe_count, 1U);
        if (!parity_xor_payloads || !parity_xor_members || !parity_xor_present ||
            !parity_gf_payloads || !parity_gf_members || !parity_gf_present) {
            st = HELIX_ERR_INTERNAL;
            goto cleanup;
        }
    }

    while ((st = read_next_oligo_line(fi, dna_line, &has_line)) == HELIX_OK && has_line) {
        uint32_t raw_index;
        uint32_t kind;
        uint32_t value;
        uint16_t payload_len;

        st = decode_dna_line_to_block(dna_line, block);
        if (st != HELIX_OK) {
            goto cleanup;
        }

        st = validate_and_unmask_v5_message(block, header);
        if (st != HELIX_OK) {
            goto cleanup;
        }

        raw_index = load_u32_be(block);
        kind = raw_index & HELIX_OLIGO_INDEX_KIND_MASK;
        value = raw_index & HELIX_OLIGO_INDEX_VALUE_MASK;
        payload_len = load_u16_be(block + HELIX_OLIGO_INDEX_BYTES);

        if (kind == HELIX_OLIGO_INDEX_KIND_DATA) {
            uint16_t expected_len;

            if (value >= data_count) {
                st = HELIX_ERR_BAD_OLIGO;
                goto cleanup;
            }

            expected_len = expected_payload_len(header->original_size, value, payload_bytes);
            if (payload_len != expected_len) {
                st = HELIX_ERR_BAD_OLIGO;
                goto cleanup;
            }

            if (data_present[value]) {
                if (data_lengths[value] != payload_len ||
                    memcmp(data_payloads + ((size_t)value * payload_bytes),
                           block + HELIX_OLIGO_META_BYTES,
                           payload_bytes) != 0) {
                    st = HELIX_ERR_BAD_OLIGO;
                    goto cleanup;
                }
                continue;
            }

            memcpy(data_payloads + ((size_t)value * payload_bytes), block + HELIX_OLIGO_META_BYTES, payload_bytes);
            data_lengths[value] = payload_len;
            data_present[value] = 1U;
        } else if (kind == HELIX_OLIGO_INDEX_KIND_PARITY_XOR || kind == HELIX_OLIGO_INDEX_KIND_PARITY_GF) {
            uint16_t expected_members;
            uint8_t *present_array;
            uint16_t *member_array;
            uint8_t *payload_array;

            if (value >= stripe_count) {
                st = HELIX_ERR_BAD_OLIGO;
                goto cleanup;
            }

            expected_members = expected_stripe_members(data_count, stripe_width, value);
            if (payload_len != expected_members) {
                st = HELIX_ERR_BAD_OLIGO;
                goto cleanup;
            }

            if (kind == HELIX_OLIGO_INDEX_KIND_PARITY_XOR) {
                present_array = parity_xor_present;
                member_array = parity_xor_members;
                payload_array = parity_xor_payloads;
            } else {
                present_array = parity_gf_present;
                member_array = parity_gf_members;
                payload_array = parity_gf_payloads;
            }

            if (present_array[value]) {
                if (member_array[value] != payload_len ||
                    memcmp(payload_array + ((size_t)value * payload_bytes),
                           block + HELIX_OLIGO_META_BYTES,
                           payload_bytes) != 0) {
                    st = HELIX_ERR_BAD_OLIGO;
                    goto cleanup;
                }
                continue;
            }

            memcpy(payload_array + ((size_t)value * payload_bytes), block + HELIX_OLIGO_META_BYTES, payload_bytes);
            member_array[value] = payload_len;
            present_array[value] = 1U;
        } else {
            st = HELIX_ERR_BAD_OLIGO;
            goto cleanup;
        }
    }

    if (st != HELIX_OK) {
        goto cleanup;
    }

    for (uint32_t stripe_index = 0; stripe_index < stripe_count; stripe_index++) {
        uint32_t stripe_start = stripe_index * stripe_width;
        uint16_t members = expected_stripe_members(data_count, stripe_width, stripe_index);
        uint32_t missing_local[2];
        uint32_t missing_count = 0U;

        for (uint32_t member = 0; member < members; member++) {
            uint32_t index = stripe_start + member;
            if (!data_present[index]) {
                if (missing_count < 2U) {
                    missing_local[missing_count] = member;
                }
                missing_count++;
            }
        }

        if (missing_count == 0U) {
            continue;
        }

        if (missing_count > 2U) {
            st = HELIX_ERR_BAD_OLIGO;
            goto cleanup;
        }

        if (missing_count == 1U) {
            uint32_t missing_member = missing_local[0];
            uint32_t missing_index = stripe_start + missing_member;
            uint8_t *target = data_payloads + ((size_t)missing_index * payload_bytes);

            if (parity_xor_present[stripe_index]) {
                memcpy(target, parity_xor_payloads + ((size_t)stripe_index * payload_bytes), payload_bytes);

                for (uint32_t member = 0; member < members; member++) {
                    uint32_t index = stripe_start + member;
                    if (member == missing_member) {
                        continue;
                    }

                    for (uint32_t byte_index = 0; byte_index < payload_bytes; byte_index++) {
                        target[byte_index] ^= data_payloads[((size_t)index * payload_bytes) + byte_index];
                    }
                }
            } else if (parity_gf_present[stripe_index]) {
                uint8_t coeff = parity_coefficient_for_member(missing_member);

                memcpy(target, parity_gf_payloads + ((size_t)stripe_index * payload_bytes), payload_bytes);

                for (uint32_t member = 0; member < members; member++) {
                    uint32_t index = stripe_start + member;
                    uint8_t current_coeff;

                    if (member == missing_member) {
                        continue;
                    }

                    current_coeff = parity_coefficient_for_member(member);
                    for (uint32_t byte_index = 0; byte_index < payload_bytes; byte_index++) {
                        target[byte_index] ^=
                            gf256_mul(current_coeff, data_payloads[((size_t)index * payload_bytes) + byte_index]);
                    }
                }

                for (uint32_t byte_index = 0; byte_index < payload_bytes; byte_index++) {
                    target[byte_index] = gf256_div(target[byte_index], coeff);
                }
            } else {
                st = HELIX_ERR_BAD_OLIGO;
                goto cleanup;
            }

            data_lengths[missing_index] = expected_payload_len(header->original_size, missing_index, payload_bytes);
            data_present[missing_index] = 1U;
            continue;
        }

        if (!parity_xor_present[stripe_index] || !parity_gf_present[stripe_index]) {
            st = HELIX_ERR_BAD_OLIGO;
            goto cleanup;
        }

        {
            uint32_t first_member = missing_local[0];
            uint32_t second_member = missing_local[1];
            uint32_t first_index = stripe_start + first_member;
            uint32_t second_index = stripe_start + second_member;
            uint8_t *first_target = data_payloads + ((size_t)first_index * payload_bytes);
            uint8_t *second_target = data_payloads + ((size_t)second_index * payload_bytes);
            uint8_t coeff_a = parity_coefficient_for_member(first_member);
            uint8_t coeff_b = parity_coefficient_for_member(second_member);
            uint8_t denominator = (uint8_t)(coeff_a ^ coeff_b);

            if (denominator == 0U) {
                st = HELIX_ERR_INTERNAL;
                goto cleanup;
            }

            memcpy(first_target, parity_xor_payloads + ((size_t)stripe_index * payload_bytes), payload_bytes);
            memcpy(second_target, parity_gf_payloads + ((size_t)stripe_index * payload_bytes), payload_bytes);

            for (uint32_t member = 0; member < members; member++) {
                uint32_t index = stripe_start + member;
                uint8_t coeff;

                if (member == first_member || member == second_member) {
                    continue;
                }

                coeff = parity_coefficient_for_member(member);
                for (uint32_t byte_index = 0; byte_index < payload_bytes; byte_index++) {
                    uint8_t value = data_payloads[((size_t)index * payload_bytes) + byte_index];
                    first_target[byte_index] ^= value;
                    second_target[byte_index] ^= gf256_mul(coeff, value);
                }
            }

            for (uint32_t byte_index = 0; byte_index < payload_bytes; byte_index++) {
                uint8_t s0 = first_target[byte_index];
                uint8_t s1 = second_target[byte_index];
                uint8_t second_value = gf256_div(
                    (uint8_t)(s1 ^ gf256_mul(coeff_a, s0)),
                    denominator
                );
                uint8_t first_value = (uint8_t)(s0 ^ second_value);

                first_target[byte_index] = first_value;
                second_target[byte_index] = second_value;
            }

            data_lengths[first_index] = expected_payload_len(header->original_size, first_index, payload_bytes);
            data_lengths[second_index] = expected_payload_len(header->original_size, second_index, payload_bytes);
            data_present[first_index] = 1U;
            data_present[second_index] = 1U;
        }
    }

    for (uint32_t index = 0; index < data_count; index++) {
        if (!data_present[index]) {
            st = HELIX_ERR_BAD_OLIGO;
            goto cleanup;
        }

        if (data_lengths[index] > 0U &&
            fwrite(data_payloads + ((size_t)index * payload_bytes), 1, data_lengths[index], fo) != data_lengths[index]) {
            st = HELIX_ERR_FILE_WRITE;
            goto cleanup;
        }
    }

cleanup:
    free(data_payloads);
    free(data_lengths);
    free(data_present);
    free(parity_xor_payloads);
    free(parity_xor_members);
    free(parity_xor_present);
    free(parity_gf_payloads);
    free(parity_gf_members);
    free(parity_gf_present);
    return st;
}

helix_status_t helix_encode_file(const char *input_path, const char *output_path) {
    FILE *fi = NULL;
    FILE *fo = NULL;
    helix_status_t st;
    uint64_t original_size = 0U;
    uint32_t crc32 = 0U;
    uint32_t data_oligo_count;
    uint32_t stripe_count;
    uint8_t logical_message[HELIX_RS_K];
    helix_header_t header;

    if (!input_path || !output_path) {
        return HELIX_ERR_INVALID_ARG;
    }

    fi = fopen(input_path, "rb");
    if (!fi) {
        return HELIX_ERR_FILE_OPEN;
    }

    fo = fopen(output_path, "wb");
    if (!fo) {
        fclose(fi);
        return HELIX_ERR_FILE_OPEN;
    }

    rs_init();
    gf256_init();

    st = get_file_size(fi, &original_size);
    if (st != HELIX_OK) {
        goto cleanup;
    }

    st = calculate_file_crc32(fi, &crc32);
    if (st != HELIX_OK) {
        goto cleanup;
    }

    data_oligo_count = expected_data_oligo_count(original_size, HELIX_OLIGO_PAYLOAD_BYTES);
    stripe_count = expected_stripe_count(data_oligo_count, HELIX_PARITY_STRIPE_DATA_OLIGOS);

    memset(&header, 0, sizeof(header));
    header.version = 5U;
    header.original_size = original_size;
    header.crc32 = crc32;
    header.header_size = HELIX_HEADER_V5_SIZE;
    header.has_crc32 = 1;
    header.is_indexed = 1;
    header.oligo_count = data_oligo_count;
    header.oligo_payload_bytes = HELIX_OLIGO_PAYLOAD_BYTES;
    header.parity_data_oligos = HELIX_PARITY_STRIPE_DATA_OLIGOS;
    header.parity_oligos_per_stripe = HELIX_PARITY_STRIPE_PARITY_OLIGOS;
    header.gc_min_percent = HELIX_CONSTRAINT_GC_MIN_PERCENT;
    header.gc_max_percent = HELIX_CONSTRAINT_GC_MAX_PERCENT;
    header.max_homopolymer = HELIX_CONSTRAINT_MAX_HOMOPOLYMER;

    st = helix_write_header_v5(
        fo,
        original_size,
        crc32,
        data_oligo_count,
        HELIX_OLIGO_PAYLOAD_BYTES,
        HELIX_PARITY_STRIPE_DATA_OLIGOS,
        HELIX_PARITY_STRIPE_PARITY_OLIGOS,
        HELIX_CONSTRAINT_GC_MIN_PERCENT,
        HELIX_CONSTRAINT_GC_MAX_PERCENT,
        HELIX_CONSTRAINT_MAX_HOMOPOLYMER
    );
    if (st != HELIX_OK) {
        goto cleanup;
    }

    if (fseek(fi, 0, SEEK_SET) != 0) {
        st = HELIX_ERR_FILE_READ;
        goto cleanup;
    }

    for (uint32_t stripe_index = 0; stripe_index < stripe_count; stripe_index++) {
        uint32_t stripe_start = stripe_index * HELIX_PARITY_STRIPE_DATA_OLIGOS;
        uint16_t members = expected_stripe_members(data_oligo_count, HELIX_PARITY_STRIPE_DATA_OLIGOS, stripe_index);
        uint8_t parity_xor[HELIX_OLIGO_PAYLOAD_BYTES];
        uint8_t parity_gf[HELIX_OLIGO_PAYLOAD_BYTES];

        memset(parity_xor, 0, sizeof(parity_xor));
        memset(parity_gf, 0, sizeof(parity_gf));

        for (uint32_t member = 0; member < members; member++) {
            uint32_t data_index = stripe_start + member;
            size_t nread;
            uint8_t coeff = parity_coefficient_for_member(member);

            memset(logical_message, 0, sizeof(logical_message));
            store_u32_be(logical_message, HELIX_OLIGO_INDEX_KIND_DATA | data_index);
            nread = fread(logical_message + HELIX_OLIGO_META_BYTES, 1, HELIX_OLIGO_PAYLOAD_BYTES, fi);

            if (nread != expected_payload_len(original_size, data_index, HELIX_OLIGO_PAYLOAD_BYTES)) {
                st = HELIX_ERR_FILE_READ;
                goto cleanup;
            }

            store_u16_be(logical_message + HELIX_OLIGO_INDEX_BYTES, (uint16_t)nread);
            logical_message[HELIX_OLIGO_INDEX_BYTES + HELIX_OLIGO_LENGTH_BYTES] = 0U;

            for (uint32_t byte_index = 0; byte_index < HELIX_OLIGO_PAYLOAD_BYTES; byte_index++) {
                uint8_t value = logical_message[HELIX_OLIGO_META_BYTES + byte_index];
                parity_xor[byte_index] ^= value;
                parity_gf[byte_index] ^= gf256_mul(coeff, value);
            }

            st = write_v5_message_line(logical_message, fo, &header);
            if (st != HELIX_OK) {
                goto cleanup;
            }
        }

        memset(logical_message, 0, sizeof(logical_message));
        store_u32_be(logical_message, HELIX_OLIGO_INDEX_KIND_PARITY_XOR | stripe_index);
        store_u16_be(logical_message + HELIX_OLIGO_INDEX_BYTES, members);
        memcpy(logical_message + HELIX_OLIGO_META_BYTES, parity_xor, HELIX_OLIGO_PAYLOAD_BYTES);
        st = write_v5_message_line(logical_message, fo, &header);
        if (st != HELIX_OK) {
            goto cleanup;
        }

        memset(logical_message, 0, sizeof(logical_message));
        store_u32_be(logical_message, HELIX_OLIGO_INDEX_KIND_PARITY_GF | stripe_index);
        store_u16_be(logical_message + HELIX_OLIGO_INDEX_BYTES, members);
        memcpy(logical_message + HELIX_OLIGO_META_BYTES, parity_gf, HELIX_OLIGO_PAYLOAD_BYTES);
        st = write_v5_message_line(logical_message, fo, &header);
        if (st != HELIX_OK) {
            goto cleanup;
        }
    }

cleanup:
    fclose(fi);
    if (fclose(fo) != 0 && st == HELIX_OK) {
        st = HELIX_ERR_FILE_WRITE;
    }

    if (st != HELIX_OK) {
        remove(output_path);
    }

    return st;
}

helix_status_t helix_decode_file(const char *input_path, const char *output_path) {
    FILE *fi = NULL;
    FILE *fo = NULL;
    char *temp_path = NULL;
    helix_header_t header;
    helix_status_t st;

    if (!input_path || !output_path) {
        return HELIX_ERR_INVALID_ARG;
    }

    fi = fopen(input_path, "rb");
    if (!fi) {
        return HELIX_ERR_FILE_OPEN;
    }

    st = helix_read_header_info(fi, &header);
    if (st != HELIX_OK) {
        fclose(fi);
        return st;
    }

    temp_path = make_temp_output_path(output_path);
    if (!temp_path) {
        fclose(fi);
        return HELIX_ERR_INTERNAL;
    }

    fo = fopen(temp_path, "wb");
    if (!fo) {
        free(temp_path);
        fclose(fi);
        return HELIX_ERR_FILE_OPEN;
    }

    rs_init();
    gf256_init();

    if (header.version <= 2U) {
        st = decode_legacy_stream(fi, fo, header.original_size);
    } else if (header.version == 3U) {
        st = decode_indexed_v3(fi, fo, &header);
    } else if (header.version == 4U) {
        st = decode_indexed_v4(fi, fo, &header);
    } else if (header.version == 5U) {
        st = decode_indexed_v5(fi, fo, &header);
    } else {
        st = HELIX_ERR_BAD_HEADER;
    }

    fclose(fi);

    if (fclose(fo) != 0 && st == HELIX_OK) {
        st = HELIX_ERR_FILE_WRITE;
    }

    if (st == HELIX_OK && header.has_crc32) {
        uint32_t actual_crc32 = 0U;

        st = calculate_path_crc32(temp_path, &actual_crc32);
        if (st == HELIX_OK && actual_crc32 != header.crc32) {
            st = HELIX_ERR_CHECKSUM;
        }
    }

    if (st == HELIX_OK) {
        helix_status_t commit_st = commit_temp_output(temp_path, output_path);
        if (commit_st != HELIX_OK) {
            remove(temp_path);
            free(temp_path);
            return commit_st;
        }
    } else {
        remove(temp_path);
    }

    free(temp_path);
    return st;
}
