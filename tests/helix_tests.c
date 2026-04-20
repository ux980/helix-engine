#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "helix/codec.h"
#include "helix/config.h"
#include "helix/crc32.h"
#include "helix/dna_codec.h"
#include "helix/file_format.h"
#include "helix/oligo_constraints.h"
#include "helix/rs.h"
#include "helix/status.h"

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 0; \
    } \
} while (0)

#define ASSERT_EQ_INT(expected, actual) do { \
    int exp_value = (int)(expected); \
    int act_value = (int)(actual); \
    if (exp_value != act_value) { \
        printf("FAIL: %s:%d: expected %d, got %d\n", \
               __FILE__, __LINE__, exp_value, act_value); \
        return 0; \
    } \
} while (0)

static int write_bytes(const char *path, const uint8_t *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }

    if (len > 0 && fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        return 0;
    }

    return fclose(fp) == 0;
}

static int files_equal(const char *a_path, const char *b_path) {
    FILE *a = fopen(a_path, "rb");
    FILE *b = fopen(b_path, "rb");
    int result = 1;

    if (!a || !b) {
        if (a) fclose(a);
        if (b) fclose(b);
        return 0;
    }

    while (1) {
        int ca = fgetc(a);
        int cb = fgetc(b);

        if (ca != cb) {
            result = 0;
            break;
        }

        if (ca == EOF) {
            break;
        }
    }

    fclose(a);
    fclose(b);
    return result;
}

static int read_file_bytes(const char *path, uint8_t **out_data, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    long size;
    uint8_t *data = NULL;

    if (!fp || !out_data || !out_len) {
        if (fp) fclose(fp);
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }

    size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    data = (uint8_t *)malloc((size_t)size);
    if (!data && size > 0) {
        fclose(fp);
        return 0;
    }

    if (size > 0 && fread(data, 1, (size_t)size, fp) != (size_t)size) {
        free(data);
        fclose(fp);
        return 0;
    }

    fclose(fp);
    *out_data = data;
    *out_len = (size_t)size;
    return 1;
}

static int read_header_info_from_path(const char *path, helix_header_t *header) {
    FILE *fp = fopen(path, "rb");
    helix_status_t st;

    if (!fp || !header) {
        if (fp) fclose(fp);
        return 0;
    }

    st = helix_read_header_info(fp, header);
    fclose(fp);
    return st == HELIX_OK;
}

static int replace_byte_at(const char *path, long offset, char value) {
    FILE *fp = fopen(path, "r+b");
    if (!fp) {
        return 0;
    }

    if (fseek(fp, offset, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    if (fputc((unsigned char)value, fp) == EOF) {
        fclose(fp);
        return 0;
    }

    return fclose(fp) == 0;
}

static int replace_base_with_different_valid_base(const char *path, long offset) {
    static const char bases[] = { 'A', 'C', 'G', 'T' };
    FILE *fp = fopen(path, "r+b");
    int current;

    if (!fp) {
        return 0;
    }

    if (fseek(fp, offset, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    current = fgetc(fp);
    if (current == EOF) {
        fclose(fp);
        return 0;
    }

    if (fseek(fp, offset, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    for (size_t i = 0; i < sizeof(bases); i++) {
        if (bases[i] != (char)current) {
            if (fputc((unsigned char)bases[i], fp) == EOF) {
                fclose(fp);
                return 0;
            }
            return fclose(fp) == 0;
        }
    }

    fclose(fp);
    return 0;
}

static int shuffle_oligo_lines(const char *path) {
    helix_header_t header;
    uint8_t *data = NULL;
    size_t size = 0;
    uint32_t line_count;
    size_t body_size;

    if (!read_header_info_from_path(path, &header) || header.header_size == 0U) {
        return 0;
    }

    if (!read_file_bytes(path, &data, &size)) {
        return 0;
    }

    if (size < header.header_size) {
        free(data);
        return 0;
    }

    body_size = size - header.header_size;
    if ((body_size % HELIX_OLIGO_LINE_CHARS) != 0U) {
        free(data);
        return 0;
    }

    line_count = (uint32_t)(body_size / HELIX_OLIGO_LINE_CHARS);
    if (line_count < 2U) {
        free(data);
        return 0;
    }

    for (uint32_t i = 0; i < line_count / 2U; i++) {
        uint32_t j = line_count - 1U - i;
        for (size_t k = 0; k < HELIX_OLIGO_LINE_CHARS; k++) {
            uint8_t *body = data + header.header_size;
            uint8_t tmp = body[(size_t)i * HELIX_OLIGO_LINE_CHARS + k];
            body[(size_t)i * HELIX_OLIGO_LINE_CHARS + k] =
                body[(size_t)j * HELIX_OLIGO_LINE_CHARS + k];
            body[(size_t)j * HELIX_OLIGO_LINE_CHARS + k] = tmp;
        }
    }

    if (!write_bytes(path, data, size)) {
        free(data);
        return 0;
    }

    free(data);
    return 1;
}

static int replace_oligo_line(const char *path, uint32_t dst_index, uint32_t src_index) {
    helix_header_t header;
    uint8_t *data = NULL;
    size_t size = 0;
    uint32_t line_count;
    size_t body_size;
    uint8_t line_copy[HELIX_OLIGO_LINE_CHARS];

    if (!read_header_info_from_path(path, &header)) {
        return 0;
    }

    if (!read_file_bytes(path, &data, &size)) {
        return 0;
    }

    if (size < header.header_size) {
        free(data);
        return 0;
    }

    body_size = size - header.header_size;
    if ((body_size % HELIX_OLIGO_LINE_CHARS) != 0U) {
        free(data);
        return 0;
    }

    line_count = (uint32_t)(body_size / HELIX_OLIGO_LINE_CHARS);
    if (dst_index >= line_count || src_index >= line_count) {
        free(data);
        return 0;
    }

    memcpy(
        line_copy,
        data + header.header_size + ((size_t)src_index * HELIX_OLIGO_LINE_CHARS),
        HELIX_OLIGO_LINE_CHARS
    );
    memcpy(
        data + header.header_size + ((size_t)dst_index * HELIX_OLIGO_LINE_CHARS),
        line_copy,
        HELIX_OLIGO_LINE_CHARS
    );

    if (!write_bytes(path, data, size)) {
        free(data);
        return 0;
    }

    free(data);
    return 1;
}

static int remove_oligo_line(const char *path, uint32_t remove_index) {
    helix_header_t header;
    uint8_t *data = NULL;
    size_t size = 0;
    uint32_t line_count;
    size_t body_size;
    size_t line_offset;

    if (!read_header_info_from_path(path, &header)) {
        return 0;
    }

    if (!read_file_bytes(path, &data, &size)) {
        return 0;
    }

    if (size < header.header_size) {
        free(data);
        return 0;
    }

    body_size = size - header.header_size;
    if ((body_size % HELIX_OLIGO_LINE_CHARS) != 0U) {
        free(data);
        return 0;
    }

    line_count = (uint32_t)(body_size / HELIX_OLIGO_LINE_CHARS);
    if (remove_index >= line_count || line_count == 0U) {
        free(data);
        return 0;
    }

    line_offset = (size_t)header.header_size + ((size_t)remove_index * HELIX_OLIGO_LINE_CHARS);
    memmove(
        data + line_offset,
        data + line_offset + HELIX_OLIGO_LINE_CHARS,
        size - line_offset - HELIX_OLIGO_LINE_CHARS
    );

    if (!write_bytes(path, data, size - HELIX_OLIGO_LINE_CHARS)) {
        free(data);
        return 0;
    }

    free(data);
    return 1;
}

static int encoded_file_meets_constraints(const char *path) {
    helix_header_t header;
    FILE *fp = fopen(path, "rb");
    char dna_line[HELIX_OLIGO_LINE_CHARS];

    if (!fp) {
        return 0;
    }

    if (helix_read_header_info(fp, &header) != HELIX_OK || header.version != 5) {
        fclose(fp);
        return 0;
    }

    while (fread(dna_line, 1, HELIX_OLIGO_LINE_CHARS, fp) == HELIX_OLIGO_LINE_CHARS) {
        if (dna_line[HELIX_OLIGO_DNA_CHARS] != '\n' ||
            !helix_dna_constraints_ok(
                dna_line,
                HELIX_OLIGO_DNA_CHARS,
                header.gc_min_percent,
                header.gc_max_percent,
                header.max_homopolymer,
                NULL
            )) {
            fclose(fp);
            return 0;
        }
    }

    if (!feof(fp)) {
        fclose(fp);
        return 0;
    }

    fclose(fp);
    return 1;
}

static void cleanup_test_files(void) {
    remove("helix_test_header.dna");
    remove("helix_test_bad_header.dna");
    remove("helix_test_crc_header.dna");
    remove("helix_test_empty.bin");
    remove("helix_test_empty.dna");
    remove("helix_test_empty_out.bin");
    remove("helix_test_input.bin");
    remove("helix_test_encoded.dna");
    remove("helix_test_decoded.bin");
    remove("helix_test_decoded.bin.part");
    remove("helix_test_bad_dna.dna");
    remove("helix_test_bad_dna_out.bin");
    remove("helix_test_bad_dna_out.bin.part");
    remove("helix_test_bad_crc.dna");
    remove("helix_test_bad_crc_out.bin");
    remove("helix_test_bad_crc_out.bin.part");
    remove("helix_test_one_sub.dna");
    remove("helix_test_one_sub_out.bin");
    remove("helix_test_one_sub_out.bin.part");
    remove("helix_test_existing_output.bin");
    remove("helix_test_shuffled.dna");
    remove("helix_test_shuffled_out.bin");
    remove("helix_test_missing_oligo.dna");
    remove("helix_test_missing_oligo_out.bin");
    remove("helix_test_missing_oligo_out.bin.part");
    remove("helix_test_duplicate_oligo.dna");
    remove("helix_test_duplicate_oligo_out.bin");
    remove("helix_test_duplicate_oligo_out.bin.part");
    remove("helix_test_missing_two_oligos.dna");
    remove("helix_test_missing_two_oligos_out.bin");
    remove("helix_test_missing_two_oligos_out.bin.part");
    remove("helix_test_missing_parity.dna");
    remove("helix_test_missing_parity_out.bin");
    remove("helix_test_missing_parity_out.bin.part");
}

static int test_dna_roundtrip_all_bytes(void) {
    char encoded[256 * 4];
    dna_state_t enc_state;
    dna_state_t dec_state;
    uint8_t decoded = 0;

    dna_state_init(&enc_state);
    for (int value = 0; value < 256; value++) {
        dna_encode_byte_diff((uint8_t)value, &enc_state, &encoded[value * 4]);
    }

    dna_state_init(&dec_state);
    for (int value = 0; value < 256; value++) {
        ASSERT_TRUE(dna_decode_byte_diff_checked(&encoded[value * 4], &dec_state, &decoded));
        ASSERT_EQ_INT(value, decoded);
    }

    ASSERT_EQ_INT(-1, dna_base_to_index('X'));
    return 1;
}

static int test_dna_rejects_invalid_base(void) {
    char quad[4] = { 'A', 'C', 'X', 'T' };
    dna_state_t state;
    uint8_t decoded = 0;

    dna_state_init(&state);
    ASSERT_EQ_INT(0, dna_decode_byte_diff_checked(quad, &state, &decoded));
    ASSERT_EQ_INT(-1, dna_base_to_index('X'));
    ASSERT_EQ_INT(0, dna_base_is_valid('X'));
    ASSERT_EQ_INT(1, dna_base_is_valid('G'));
    return 1;
}

static int test_crc32_known_vector(void) {
    const uint8_t data[] = "123456789";
    ASSERT_TRUE(helix_crc32(data, 9) == UINT32_C(0xCBF43926));
    ASSERT_TRUE(helix_crc32(NULL, 0) == UINT32_C(0x00000000));
    return 1;
}

static int test_header_roundtrip_and_validation(void) {
    FILE *fp = fopen("helix_test_header.dna", "wb");
    uint64_t size = 0;
    helix_header_t header;

    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ_INT(HELIX_OK, helix_write_header(fp, UINT64_C(123456789)));
    ASSERT_TRUE(fclose(fp) == 0);

    fp = fopen("helix_test_header.dna", "rb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ_INT(HELIX_OK, helix_read_header(fp, &size));
    ASSERT_TRUE(fclose(fp) == 0);
    ASSERT_TRUE(size == UINT64_C(123456789));

    fp = fopen("helix_test_crc_header.dna", "wb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ_INT(HELIX_OK, helix_write_header_with_crc(fp, UINT64_C(987654321), UINT32_C(0xCBF43926)));
    ASSERT_TRUE(fclose(fp) == 0);

    fp = fopen("helix_test_crc_header.dna", "rb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ_INT(HELIX_OK, helix_read_header_info(fp, &header));
    ASSERT_TRUE(fclose(fp) == 0);
    ASSERT_TRUE(header.original_size == UINT64_C(987654321));
    ASSERT_TRUE(header.crc32 == UINT32_C(0xCBF43926));
    ASSERT_EQ_INT(1, header.has_crc32);
    ASSERT_EQ_INT(HELIX_HEADER_V2_SIZE, header.header_size);
    ASSERT_EQ_INT(2, header.version);
    ASSERT_EQ_INT(0, header.is_indexed);

    fp = fopen("helix_test_crc_header.dna", "wb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ_INT(
        HELIX_OK,
        helix_write_header_v3(
            fp,
            UINT64_C(987654321),
            UINT32_C(0xCBF43926),
            123,
            HELIX_V4_OLIGO_PAYLOAD_BYTES
        )
    );
    ASSERT_TRUE(fclose(fp) == 0);

    fp = fopen("helix_test_crc_header.dna", "rb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ_INT(HELIX_OK, helix_read_header_info(fp, &header));
    ASSERT_TRUE(fclose(fp) == 0);
    ASSERT_TRUE(header.original_size == UINT64_C(987654321));
    ASSERT_TRUE(header.crc32 == UINT32_C(0xCBF43926));
    ASSERT_EQ_INT(1, header.has_crc32);
    ASSERT_EQ_INT(HELIX_HEADER_V3_SIZE, header.header_size);
    ASSERT_EQ_INT(3, header.version);
    ASSERT_EQ_INT(1, header.is_indexed);
    ASSERT_EQ_INT(123, header.oligo_count);
    ASSERT_EQ_INT(HELIX_V4_OLIGO_PAYLOAD_BYTES, header.oligo_payload_bytes);

    fp = fopen("helix_test_crc_header.dna", "wb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ_INT(
        HELIX_OK,
        helix_write_header_v4(
            fp,
            UINT64_C(987654321),
            UINT32_C(0xCBF43926),
            123,
            HELIX_V4_OLIGO_PAYLOAD_BYTES,
            HELIX_PARITY_STRIPE_DATA_OLIGOS
        )
    );
    ASSERT_TRUE(fclose(fp) == 0);

    fp = fopen("helix_test_crc_header.dna", "rb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ_INT(HELIX_OK, helix_read_header_info(fp, &header));
    ASSERT_TRUE(fclose(fp) == 0);
    ASSERT_TRUE(header.original_size == UINT64_C(987654321));
    ASSERT_TRUE(header.crc32 == UINT32_C(0xCBF43926));
    ASSERT_EQ_INT(1, header.has_crc32);
    ASSERT_EQ_INT(HELIX_HEADER_V4_SIZE, header.header_size);
    ASSERT_EQ_INT(4, header.version);
    ASSERT_EQ_INT(1, header.is_indexed);
    ASSERT_EQ_INT(123, header.oligo_count);
    ASSERT_EQ_INT(HELIX_V4_OLIGO_PAYLOAD_BYTES, header.oligo_payload_bytes);
    ASSERT_EQ_INT(HELIX_PARITY_STRIPE_DATA_OLIGOS, header.parity_data_oligos);

    fp = fopen("helix_test_crc_header.dna", "wb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ_INT(
        HELIX_OK,
        helix_write_header_v5(
            fp,
            UINT64_C(987654321),
            UINT32_C(0xCBF43926),
            123,
            HELIX_OLIGO_PAYLOAD_BYTES,
            HELIX_PARITY_STRIPE_DATA_OLIGOS,
            HELIX_PARITY_STRIPE_PARITY_OLIGOS,
            HELIX_CONSTRAINT_GC_MIN_PERCENT,
            HELIX_CONSTRAINT_GC_MAX_PERCENT,
            HELIX_CONSTRAINT_MAX_HOMOPOLYMER
        )
    );
    ASSERT_TRUE(fclose(fp) == 0);

    fp = fopen("helix_test_crc_header.dna", "rb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ_INT(HELIX_OK, helix_read_header_info(fp, &header));
    ASSERT_TRUE(fclose(fp) == 0);
    ASSERT_EQ_INT(HELIX_HEADER_V5_SIZE, header.header_size);
    ASSERT_EQ_INT(5, header.version);
    ASSERT_EQ_INT(123, header.oligo_count);
    ASSERT_EQ_INT(HELIX_OLIGO_PAYLOAD_BYTES, header.oligo_payload_bytes);
    ASSERT_EQ_INT(HELIX_PARITY_STRIPE_DATA_OLIGOS, header.parity_data_oligos);
    ASSERT_EQ_INT(HELIX_PARITY_STRIPE_PARITY_OLIGOS, header.parity_oligos_per_stripe);
    ASSERT_EQ_INT(HELIX_CONSTRAINT_GC_MIN_PERCENT, header.gc_min_percent);
    ASSERT_EQ_INT(HELIX_CONSTRAINT_GC_MAX_PERCENT, header.gc_max_percent);
    ASSERT_EQ_INT(HELIX_CONSTRAINT_MAX_HOMOPOLYMER, header.max_homopolymer);

    fp = fopen("helix_test_bad_header.dna", "wb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ_INT(HELIX_OK, helix_write_header(fp, 42));
    ASSERT_TRUE(fclose(fp) == 0);
    ASSERT_TRUE(replace_byte_at("helix_test_bad_header.dna", 8, 'X'));

    fp = fopen("helix_test_bad_header.dna", "rb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ_INT(HELIX_ERR_BAD_HEADER, helix_read_header(fp, &size));
    ASSERT_TRUE(fclose(fp) == 0);

    return 1;
}

static int build_rs_block(uint8_t block[HELIX_RS_N]) {
    uint8_t data[HELIX_RS_K];
    uint8_t parity[HELIX_RS_T];

    for (int i = 0; i < HELIX_RS_K; i++) {
        data[i] = (uint8_t)((i * 17 + 29) & 0xFF);
    }

    rs_encode_block(data, parity);
    memcpy(block, data, HELIX_RS_K);
    memcpy(block + HELIX_RS_K, parity, HELIX_RS_T);
    return 1;
}

static void corrupt_rs_symbols(uint8_t block[HELIX_RS_N], int count) {
    for (int i = 0; i < count; i++) {
        int pos = (i * 13) % HELIX_RS_N;
        block[pos] ^= (uint8_t)(i + 1);
    }
}

static int test_rs_corrects_up_to_limit(void) {
    uint8_t original[HELIX_RS_N];
    uint8_t block[HELIX_RS_N];
    uint8_t syndromes[HELIX_RS_T];

    rs_init();
    ASSERT_TRUE(build_rs_block(original));
    memcpy(block, original, sizeof(block));

    ASSERT_EQ_INT(0, rs_calc_syndromes(block, syndromes));
    corrupt_rs_symbols(block, HELIX_RS_MAX_ERR);
    ASSERT_EQ_INT(1, rs_calc_syndromes(block, syndromes));
    ASSERT_EQ_INT(1, rs_correct_errors(block, syndromes));
    ASSERT_EQ_INT(0, rs_calc_syndromes(block, syndromes));
    ASSERT_TRUE(memcmp(block, original, sizeof(block)) == 0);

    return 1;
}

static int test_rs_rejects_over_limit(void) {
    uint8_t block[HELIX_RS_N];
    uint8_t syndromes[HELIX_RS_T];

    rs_init();
    ASSERT_TRUE(build_rs_block(block));
    corrupt_rs_symbols(block, HELIX_RS_MAX_ERR + 1);

    ASSERT_EQ_INT(1, rs_calc_syndromes(block, syndromes));
    ASSERT_EQ_INT(0, rs_correct_errors(block, syndromes));

    return 1;
}

static int test_codec_roundtrip_empty_and_multiblock(void) {
    uint8_t data[600];

    ASSERT_TRUE(write_bytes("helix_test_empty.bin", NULL, 0));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_empty.bin", "helix_test_empty.dna"));
    ASSERT_EQ_INT(HELIX_OK, helix_decode_file("helix_test_empty.dna", "helix_test_empty_out.bin"));
    ASSERT_TRUE(files_equal("helix_test_empty.bin", "helix_test_empty_out.bin"));

    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)((i * 31 + 7) & 0xFF);
    }

    ASSERT_TRUE(write_bytes("helix_test_input.bin", data, sizeof(data)));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_encoded.dna"));
    ASSERT_EQ_INT(HELIX_OK, helix_decode_file("helix_test_encoded.dna", "helix_test_decoded.bin"));
    ASSERT_TRUE(files_equal("helix_test_input.bin", "helix_test_decoded.bin"));

    return 1;
}

static int test_codec_reports_invalid_dna(void) {
    helix_header_t header;

    ASSERT_TRUE(write_bytes("helix_test_input.bin", (const uint8_t *)"abc", 3));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_bad_dna.dna"));
    ASSERT_TRUE(read_header_info_from_path("helix_test_bad_dna.dna", &header));
    ASSERT_TRUE(replace_byte_at("helix_test_bad_dna.dna", (long)header.header_size + 4L, 'X'));
    ASSERT_EQ_INT(HELIX_ERR_BAD_DNA,
                  helix_decode_file("helix_test_bad_dna.dna", "helix_test_bad_dna_out.bin"));
    return 1;
}

static int test_codec_reports_checksum_mismatch(void) {
    ASSERT_TRUE(write_bytes("helix_test_input.bin", (const uint8_t *)"abc", 3));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_bad_crc.dna"));
    ASSERT_TRUE(replace_byte_at("helix_test_bad_crc.dna", (long)(strlen(HELIX_MAGIC_V5) + 20), '0'));
    ASSERT_EQ_INT(HELIX_ERR_CHECKSUM,
                  helix_decode_file("helix_test_bad_crc.dna", "helix_test_bad_crc_out.bin"));
    return 1;
}

static int test_codec_failure_keeps_existing_output(void) {
    uint8_t data[300];
    const uint8_t sentinel[] = "KEEP";
    helix_header_t header;

    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)((i * 7 + 11) & 0xFF);
    }

    ASSERT_TRUE(write_bytes("helix_test_input.bin", data, sizeof(data)));
    ASSERT_TRUE(write_bytes("helix_test_existing_output.bin", sentinel, sizeof(sentinel) - 1));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_bad_dna.dna"));
    ASSERT_TRUE(read_header_info_from_path("helix_test_bad_dna.dna", &header));
    ASSERT_TRUE(replace_byte_at("helix_test_bad_dna.dna", (long)header.header_size + 4L, 'X'));
    ASSERT_EQ_INT(
        HELIX_ERR_BAD_DNA,
        helix_decode_file("helix_test_bad_dna.dna", "helix_test_existing_output.bin")
    );
    ASSERT_TRUE(write_bytes("helix_test_bad_dna_out.bin", sentinel, sizeof(sentinel) - 1));
    ASSERT_TRUE(files_equal("helix_test_existing_output.bin", "helix_test_bad_dna_out.bin"));
    return 1;
}

static int test_codec_corrects_single_base_substitution(void) {
    uint8_t data[HELIX_RS_K];
    helix_header_t header;

    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)((i * 19 + 3) & 0xFF);
    }

    ASSERT_TRUE(write_bytes("helix_test_input.bin", data, sizeof(data)));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_one_sub.dna"));
    ASSERT_TRUE(read_header_info_from_path("helix_test_one_sub.dna", &header));
    ASSERT_TRUE(replace_base_with_different_valid_base("helix_test_one_sub.dna", (long)header.header_size + 9L));
    ASSERT_EQ_INT(HELIX_OK,
                  helix_decode_file("helix_test_one_sub.dna", "helix_test_one_sub_out.bin"));
    ASSERT_TRUE(files_equal("helix_test_input.bin", "helix_test_one_sub_out.bin"));
    return 1;
}

static int test_codec_emits_biologically_constrained_oligos(void) {
    uint8_t data[1024];

    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)((i * 47 + 15) & 0xFF);
    }

    ASSERT_TRUE(write_bytes("helix_test_input.bin", data, sizeof(data)));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_encoded.dna"));
    ASSERT_TRUE(encoded_file_meets_constraints("helix_test_encoded.dna"));
    return 1;
}

static int test_codec_decodes_shuffled_oligos(void) {
    uint8_t data[600];

    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)((i * 23 + 5) & 0xFF);
    }

    ASSERT_TRUE(write_bytes("helix_test_input.bin", data, sizeof(data)));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_shuffled.dna"));
    ASSERT_TRUE(shuffle_oligo_lines("helix_test_shuffled.dna"));
    ASSERT_EQ_INT(HELIX_OK, helix_decode_file("helix_test_shuffled.dna", "helix_test_shuffled_out.bin"));
    ASSERT_TRUE(files_equal("helix_test_input.bin", "helix_test_shuffled_out.bin"));
    return 1;
}

static int test_codec_recovers_missing_oligo_with_parity(void) {
    uint8_t data[600];

    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)((i * 13 + 17) & 0xFF);
    }

    ASSERT_TRUE(write_bytes("helix_test_input.bin", data, sizeof(data)));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_missing_oligo.dna"));
    ASSERT_TRUE(remove_oligo_line("helix_test_missing_oligo.dna", 1));
    ASSERT_EQ_INT(
        HELIX_OK,
        helix_decode_file("helix_test_missing_oligo.dna", "helix_test_missing_oligo_out.bin")
    );
    ASSERT_TRUE(files_equal("helix_test_input.bin", "helix_test_missing_oligo_out.bin"));
    return 1;
}

static int test_codec_recovers_duplicate_replacing_missing_oligo(void) {
    uint8_t data[600];

    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)((i * 29 + 9) & 0xFF);
    }

    ASSERT_TRUE(write_bytes("helix_test_input.bin", data, sizeof(data)));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_duplicate_oligo.dna"));
    ASSERT_TRUE(replace_oligo_line("helix_test_duplicate_oligo.dna", 1, 0));
    ASSERT_EQ_INT(
        HELIX_OK,
        helix_decode_file("helix_test_duplicate_oligo.dna", "helix_test_duplicate_oligo_out.bin")
    );
    ASSERT_TRUE(files_equal("helix_test_input.bin", "helix_test_duplicate_oligo_out.bin"));
    return 1;
}

static int test_codec_recovers_two_missing_oligos_same_stripe(void) {
    uint8_t data[600];

    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)((i * 37 + 21) & 0xFF);
    }

    ASSERT_TRUE(write_bytes("helix_test_input.bin", data, sizeof(data)));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_missing_two_oligos.dna"));
    ASSERT_TRUE(remove_oligo_line("helix_test_missing_two_oligos.dna", 1));
    ASSERT_TRUE(remove_oligo_line("helix_test_missing_two_oligos.dna", 0));
    ASSERT_EQ_INT(
        HELIX_OK,
        helix_decode_file("helix_test_missing_two_oligos.dna", "helix_test_missing_two_oligos_out.bin")
    );
    ASSERT_TRUE(files_equal("helix_test_input.bin", "helix_test_missing_two_oligos_out.bin"));
    return 1;
}

static int test_codec_decodes_without_parity_if_all_data_present(void) {
    uint8_t data[600];

    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)((i * 41 + 1) & 0xFF);
    }

    ASSERT_TRUE(write_bytes("helix_test_input.bin", data, sizeof(data)));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_missing_parity.dna"));
    ASSERT_TRUE(remove_oligo_line("helix_test_missing_parity.dna", 3));
    ASSERT_EQ_INT(
        HELIX_OK,
        helix_decode_file("helix_test_missing_parity.dna", "helix_test_missing_parity_out.bin")
    );
    ASSERT_TRUE(files_equal("helix_test_input.bin", "helix_test_missing_parity_out.bin"));
    return 1;
}

static int test_codec_recovers_one_missing_with_only_second_parity(void) {
    uint8_t data[600];

    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)((i * 43 + 31) & 0xFF);
    }

    ASSERT_TRUE(write_bytes("helix_test_input.bin", data, sizeof(data)));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_missing_oligo.dna"));
    ASSERT_TRUE(remove_oligo_line("helix_test_missing_oligo.dna", 3));
    ASSERT_TRUE(remove_oligo_line("helix_test_missing_oligo.dna", 1));
    ASSERT_EQ_INT(
        HELIX_OK,
        helix_decode_file("helix_test_missing_oligo.dna", "helix_test_missing_oligo_out.bin")
    );
    ASSERT_TRUE(files_equal("helix_test_input.bin", "helix_test_missing_oligo_out.bin"));
    return 1;
}

static int test_codec_rejects_two_missing_if_one_parity_is_missing(void) {
    uint8_t data[600];

    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)((i * 53 + 7) & 0xFF);
    }

    ASSERT_TRUE(write_bytes("helix_test_input.bin", data, sizeof(data)));
    ASSERT_EQ_INT(HELIX_OK, helix_encode_file("helix_test_input.bin", "helix_test_missing_two_oligos.dna"));
    ASSERT_TRUE(remove_oligo_line("helix_test_missing_two_oligos.dna", 4));
    ASSERT_TRUE(remove_oligo_line("helix_test_missing_two_oligos.dna", 1));
    ASSERT_TRUE(remove_oligo_line("helix_test_missing_two_oligos.dna", 0));
    ASSERT_EQ_INT(
        HELIX_ERR_BAD_OLIGO,
        helix_decode_file("helix_test_missing_two_oligos.dna", "helix_test_missing_two_oligos_out.bin")
    );
    return 1;
}

typedef int (*test_fn)(void);

typedef struct {
    const char *name;
    test_fn fn;
} test_case_t;

static int run_test(const test_case_t *test) {
    cleanup_test_files();
    printf("RUN  %s\n", test->name);

    if (!test->fn()) {
        printf("FAIL %s\n", test->name);
        cleanup_test_files();
        return 0;
    }

    printf("PASS %s\n", test->name);
    cleanup_test_files();
    return 1;
}

int main(void) {
    const test_case_t tests[] = {
        { "dna_roundtrip_all_bytes", test_dna_roundtrip_all_bytes },
        { "dna_rejects_invalid_base", test_dna_rejects_invalid_base },
        { "crc32_known_vector", test_crc32_known_vector },
        { "header_roundtrip_and_validation", test_header_roundtrip_and_validation },
        { "rs_corrects_up_to_limit", test_rs_corrects_up_to_limit },
        { "rs_rejects_over_limit", test_rs_rejects_over_limit },
        { "codec_roundtrip_empty_and_multiblock", test_codec_roundtrip_empty_and_multiblock },
        { "codec_reports_invalid_dna", test_codec_reports_invalid_dna },
        { "codec_reports_checksum_mismatch", test_codec_reports_checksum_mismatch },
        { "codec_failure_keeps_existing_output", test_codec_failure_keeps_existing_output },
        { "codec_corrects_single_base_substitution", test_codec_corrects_single_base_substitution },
        { "codec_emits_biologically_constrained_oligos", test_codec_emits_biologically_constrained_oligos },
        { "codec_decodes_shuffled_oligos", test_codec_decodes_shuffled_oligos },
        { "codec_recovers_missing_oligo_with_parity", test_codec_recovers_missing_oligo_with_parity },
        { "codec_recovers_duplicate_replacing_missing_oligo", test_codec_recovers_duplicate_replacing_missing_oligo },
        { "codec_recovers_two_missing_oligos_same_stripe", test_codec_recovers_two_missing_oligos_same_stripe },
        { "codec_decodes_without_parity_if_all_data_present", test_codec_decodes_without_parity_if_all_data_present },
        { "codec_recovers_one_missing_with_only_second_parity", test_codec_recovers_one_missing_with_only_second_parity },
        { "codec_rejects_two_missing_if_one_parity_is_missing", test_codec_rejects_two_missing_if_one_parity_is_missing }
    };
    int passed = 0;
    int total = (int)(sizeof(tests) / sizeof(tests[0]));

    for (int i = 0; i < total; i++) {
        if (run_test(&tests[i])) {
            passed++;
        }
    }

    printf("%d/%d tests passed\n", passed, total);
    return (passed == total) ? 0 : 1;
}
