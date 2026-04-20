#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "helix/config.h"
#include "helix/file_format.h"

static void format_u64_20(char out[20], uint64_t value) {
    for (int i = 19; i >= 0; i--) {
        out[i] = (char)('0' + (value % 10));
        value /= 10;
    }
}

static void format_u32_hex8(char out[8], uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";

    for (int i = 7; i >= 0; i--) {
        out[i] = digits[value & 0x0FU];
        value >>= 4;
    }
}

static void format_u32_dec(char *out, size_t width, uint32_t value) {
    for (size_t i = 0; i < width; i++) {
        out[width - 1 - i] = (char)('0' + (value % 10U));
        value /= 10U;
    }
}

static int parse_u64_20(const char *text, uint64_t *out_value) {
    uint64_t value = 0;

    if (!text || !out_value) {
        return 0;
    }

    for (size_t i = 0; i < 20; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!isdigit(c)) {
            return 0;
        }

        uint64_t digit = (uint64_t)(c - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return 0;
        }
        value = (value * 10) + digit;
    }

    *out_value = value;
    return 1;
}

static int parse_u32_dec(const char *text, size_t width, uint32_t *out_value) {
    uint32_t value = 0;

    if (!text || !out_value) {
        return 0;
    }

    for (size_t i = 0; i < width; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!isdigit(c)) {
            return 0;
        }

        value = (value * 10U) + (uint32_t)(c - '0');
    }

    *out_value = value;
    return 1;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int parse_u32_hex8(const char *text, uint32_t *out_value) {
    uint32_t value = 0;

    if (!text || !out_value) {
        return 0;
    }

    for (size_t i = 0; i < 8; i++) {
        int digit = hex_value(text[i]);
        if (digit < 0) {
            return 0;
        }

        value = (value << 4) | (uint32_t)digit;
    }

    *out_value = value;
    return 1;
}

static int padding_is_zero(const char *header, size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
        if (header[i] != '\0') {
            return 0;
        }
    }

    return 1;
}

helix_status_t helix_write_header(FILE *fp, uint64_t original_size) {
    if (!fp) {
        return HELIX_ERR_INVALID_ARG;
    }

    char header[HELIX_HEADER_V1_SIZE];
    memset(header, 0, sizeof(header));

    size_t magic_len = strlen(HELIX_MAGIC_V1);
    if (magic_len + 20 + 1 > sizeof(header)) {
        return HELIX_ERR_INTERNAL;
    }

    memcpy(header, HELIX_MAGIC_V1, magic_len);
    format_u64_20(header + magic_len, original_size);
    header[magic_len + 20] = '\n';

    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        return HELIX_ERR_FILE_WRITE;
    }

    return HELIX_OK;
}

helix_status_t helix_write_header_with_crc(FILE *fp, uint64_t original_size, uint32_t crc32) {
    if (!fp) {
        return HELIX_ERR_INVALID_ARG;
    }

    char header[HELIX_HEADER_V2_SIZE];
    memset(header, 0, sizeof(header));

    size_t magic_len = strlen(HELIX_MAGIC_V2);
    if (magic_len + 20 + 8 + 1 > sizeof(header)) {
        return HELIX_ERR_INTERNAL;
    }

    memcpy(header, HELIX_MAGIC_V2, magic_len);
    format_u64_20(header + magic_len, original_size);
    format_u32_hex8(header + magic_len + 20, crc32);
    header[magic_len + 20 + 8] = '\n';

    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        return HELIX_ERR_FILE_WRITE;
    }

    return HELIX_OK;
}

helix_status_t helix_write_header_v3(
    FILE *fp,
    uint64_t original_size,
    uint32_t crc32,
    uint32_t oligo_count,
    uint32_t oligo_payload_bytes
) {
    if (!fp) {
        return HELIX_ERR_INVALID_ARG;
    }

    char header[HELIX_HEADER_V3_SIZE];
    memset(header, 0, sizeof(header));

    size_t magic_len = strlen(HELIX_MAGIC_V3);
    if (magic_len + 20 + 8 + 10 + 4 + 1 > sizeof(header)) {
        return HELIX_ERR_INTERNAL;
    }

    memcpy(header, HELIX_MAGIC_V3, magic_len);
    format_u64_20(header + magic_len, original_size);
    format_u32_hex8(header + magic_len + 20, crc32);
    format_u32_dec(header + magic_len + 28, 10, oligo_count);
    format_u32_dec(header + magic_len + 38, 4, oligo_payload_bytes);
    header[magic_len + 42] = '\n';

    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        return HELIX_ERR_FILE_WRITE;
    }

    return HELIX_OK;
}

helix_status_t helix_write_header_v4(
    FILE *fp,
    uint64_t original_size,
    uint32_t crc32,
    uint32_t oligo_count,
    uint32_t oligo_payload_bytes,
    uint32_t parity_data_oligos
) {
    if (!fp) {
        return HELIX_ERR_INVALID_ARG;
    }

    char header[HELIX_HEADER_V4_SIZE];
    memset(header, 0, sizeof(header));

    size_t magic_len = strlen(HELIX_MAGIC_V4);
    if (magic_len + 20 + 8 + 10 + 4 + 4 + 1 > sizeof(header)) {
        return HELIX_ERR_INTERNAL;
    }

    memcpy(header, HELIX_MAGIC_V4, magic_len);
    format_u64_20(header + magic_len, original_size);
    format_u32_hex8(header + magic_len + 20, crc32);
    format_u32_dec(header + magic_len + 28, 10, oligo_count);
    format_u32_dec(header + magic_len + 38, 4, oligo_payload_bytes);
    format_u32_dec(header + magic_len + 42, 4, parity_data_oligos);
    header[magic_len + 46] = '\n';

    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        return HELIX_ERR_FILE_WRITE;
    }

    return HELIX_OK;
}

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
) {
    if (!fp) {
        return HELIX_ERR_INVALID_ARG;
    }

    char header[HELIX_HEADER_V5_SIZE];
    memset(header, 0, sizeof(header));

    size_t magic_len = strlen(HELIX_MAGIC_V5);
    if (magic_len + 20 + 8 + 10 + 4 + 4 + 2 + 2 + 2 + 2 + 1 > sizeof(header)) {
        return HELIX_ERR_INTERNAL;
    }

    memcpy(header, HELIX_MAGIC_V5, magic_len);
    format_u64_20(header + magic_len, original_size);
    format_u32_hex8(header + magic_len + 20, crc32);
    format_u32_dec(header + magic_len + 28, 10, oligo_count);
    format_u32_dec(header + magic_len + 38, 4, oligo_payload_bytes);
    format_u32_dec(header + magic_len + 42, 4, parity_data_oligos);
    format_u32_dec(header + magic_len + 46, 2, parity_oligos_per_stripe);
    format_u32_dec(header + magic_len + 48, 2, gc_min_percent);
    format_u32_dec(header + magic_len + 50, 2, gc_max_percent);
    format_u32_dec(header + magic_len + 52, 2, max_homopolymer);
    header[magic_len + 54] = '\n';

    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        return HELIX_ERR_FILE_WRITE;
    }

    return HELIX_OK;
}

helix_status_t helix_read_header(FILE *fp, uint64_t *original_size) {
    helix_header_t header;
    helix_status_t st;

    if (!original_size) {
        return HELIX_ERR_INVALID_ARG;
    }

    st = helix_read_header_info(fp, &header);
    if (st != HELIX_OK) {
        return st;
    }

    *original_size = header.original_size;
    return HELIX_OK;
}

helix_status_t helix_read_header_info(FILE *fp, helix_header_t *header) {
    char raw[HELIX_HEADER_V5_SIZE];
    size_t magic_len = strlen(HELIX_MAGIC_V1);
    size_t header_size = 0;
    uint32_t version = 0;

    if (!fp || !header) {
        return HELIX_ERR_INVALID_ARG;
    }

    memset(header, 0, sizeof(*header));
    memset(raw, 0, sizeof(raw));

    if (fread(raw, 1, magic_len, fp) != magic_len) {
        return HELIX_ERR_FILE_READ;
    }

    if (memcmp(raw, HELIX_MAGIC_V5, magic_len) == 0) {
        header_size = HELIX_HEADER_V5_SIZE;
        version = 5;
    } else if (memcmp(raw, HELIX_MAGIC_V4, magic_len) == 0) {
        header_size = HELIX_HEADER_V4_SIZE;
        version = 4;
    } else if (memcmp(raw, HELIX_MAGIC_V3, magic_len) == 0) {
        header_size = HELIX_HEADER_V3_SIZE;
        version = 3;
    } else if (memcmp(raw, HELIX_MAGIC_V2, magic_len) == 0) {
        header_size = HELIX_HEADER_V2_SIZE;
        version = 2;
    } else if (memcmp(raw, HELIX_MAGIC_V1, magic_len) == 0) {
        header_size = HELIX_HEADER_V1_SIZE;
        version = 1;
    } else {
        return HELIX_ERR_BAD_HEADER;
    }

    if (fread(raw + magic_len, 1, header_size - magic_len, fp) != header_size - magic_len) {
        return HELIX_ERR_FILE_READ;
    }

    if (!parse_u64_20(raw + magic_len, &header->original_size)) {
        return HELIX_ERR_BAD_HEADER;
    }

    header->version = version;
    header->header_size = (uint32_t)header_size;

    if (version == 5) {
        if (!parse_u32_hex8(raw + magic_len + 20, &header->crc32) ||
            !parse_u32_dec(raw + magic_len + 28, 10, &header->oligo_count) ||
            !parse_u32_dec(raw + magic_len + 38, 4, &header->oligo_payload_bytes) ||
            !parse_u32_dec(raw + magic_len + 42, 4, &header->parity_data_oligos) ||
            !parse_u32_dec(raw + magic_len + 46, 2, &header->parity_oligos_per_stripe) ||
            !parse_u32_dec(raw + magic_len + 48, 2, &header->gc_min_percent) ||
            !parse_u32_dec(raw + magic_len + 50, 2, &header->gc_max_percent) ||
            !parse_u32_dec(raw + magic_len + 52, 2, &header->max_homopolymer)) {
            return HELIX_ERR_BAD_HEADER;
        }

        if (raw[magic_len + 54] != '\n') {
            return HELIX_ERR_BAD_HEADER;
        }

        if (!padding_is_zero(raw, magic_len + 55, header_size)) {
            return HELIX_ERR_BAD_HEADER;
        }

        header->has_crc32 = 1;
        header->is_indexed = 1;
    } else if (version == 4) {
        if (!parse_u32_hex8(raw + magic_len + 20, &header->crc32) ||
            !parse_u32_dec(raw + magic_len + 28, 10, &header->oligo_count) ||
            !parse_u32_dec(raw + magic_len + 38, 4, &header->oligo_payload_bytes) ||
            !parse_u32_dec(raw + magic_len + 42, 4, &header->parity_data_oligos)) {
            return HELIX_ERR_BAD_HEADER;
        }

        if (raw[magic_len + 46] != '\n') {
            return HELIX_ERR_BAD_HEADER;
        }

        if (!padding_is_zero(raw, magic_len + 47, header_size)) {
            return HELIX_ERR_BAD_HEADER;
        }

        header->has_crc32 = 1;
        header->is_indexed = 1;
    } else if (version == 3) {
        if (!parse_u32_hex8(raw + magic_len + 20, &header->crc32) ||
            !parse_u32_dec(raw + magic_len + 28, 10, &header->oligo_count) ||
            !parse_u32_dec(raw + magic_len + 38, 4, &header->oligo_payload_bytes)) {
            return HELIX_ERR_BAD_HEADER;
        }

        if (raw[magic_len + 42] != '\n') {
            return HELIX_ERR_BAD_HEADER;
        }

        if (!padding_is_zero(raw, magic_len + 43, header_size)) {
            return HELIX_ERR_BAD_HEADER;
        }

        header->has_crc32 = 1;
        header->is_indexed = 1;
    } else if (version == 2) {
        if (!parse_u32_hex8(raw + magic_len + 20, &header->crc32)) {
            return HELIX_ERR_BAD_HEADER;
        }

        if (raw[magic_len + 28] != '\n') {
            return HELIX_ERR_BAD_HEADER;
        }

        if (!padding_is_zero(raw, magic_len + 29, header_size)) {
            return HELIX_ERR_BAD_HEADER;
        }

        header->has_crc32 = 1;
    } else {
        if (raw[magic_len + 20] != '\n') {
            return HELIX_ERR_BAD_HEADER;
        }

        if (!padding_is_zero(raw, magic_len + 21, header_size)) {
            return HELIX_ERR_BAD_HEADER;
        }
    }

    return HELIX_OK;
}
