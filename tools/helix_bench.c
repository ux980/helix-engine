#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "helix/config.h"
#include "helix/file_format.h"

#ifdef _WIN32
    #define HELIX_BIN "helix.exe"
    #define HELIX_MUTATE_BIN "helix_mutate.exe"
#else
    #define HELIX_BIN "./helix"
    #define HELIX_MUTATE_BIN "./helix_mutate"
#endif

typedef struct {
    uint64_t state;
} helix_rng_t;

static unsigned int generate_auto_seed(void) {
    int local = 0;
    uintptr_t stack_addr = (uintptr_t)&local;
    return (unsigned int)time(NULL) ^ (unsigned int)clock() ^ (unsigned int)(stack_addr >> 4);
}

static void rng_seed(helix_rng_t *rng, const char *seed_str, int *out_auto_seed) {
    unsigned int seed = 0U;

    if (!rng || !out_auto_seed) {
        return;
    }

    if (seed_str && strcmp(seed_str, "-noseed") == 0) {
        rng->state = UINT64_C(0xE7037ED1A0B428DB);
        *out_auto_seed = 0;
        return;
    }

    if (seed_str) {
        seed = (unsigned int)strtoul(seed_str, NULL, 10);
        *out_auto_seed = 0;
    } else {
        seed = generate_auto_seed();
        *out_auto_seed = 1;
    }

    rng->state = UINT64_C(0xA0761D6478BD642F) ^ (uint64_t)seed;
}

static uint64_t rng_next_u64(helix_rng_t *rng) {
    uint64_t z = (rng->state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static uint32_t rng_next_index(helix_rng_t *rng, uint32_t limit) {
    return (uint32_t)(rng_next_u64(rng) % (uint64_t)limit);
}

static long get_file_size(const char *path) {
    FILE *fp = fopen(path, "rb");
    long size;

    if (!fp) {
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    size = ftell(fp);
    fclose(fp);
    return size;
}

static double now_seconds(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

static int run_command(const char *cmd) {
    int rc = system(cmd);
    if (rc != 0) {
        printf("Falha ao executar: %s\n", cmd);
    }
    return rc;
}

static int compare_result_is_equal(const char *original, const char *decoded) {
    FILE *fa = fopen(original, "rb");
    FILE *fb = fopen(decoded, "rb");
    int equal = 1;

    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 0;
    }

    while (1) {
        int ca = fgetc(fa);
        int cb = fgetc(fb);

        if (ca != cb) {
            equal = 0;
            break;
        }

        if (ca == EOF) {
            break;
        }
    }

    fclose(fa);
    fclose(fb);
    return equal;
}

static int mutation_spec_is_numeric(const char *spec, double *out_value) {
    char *endptr = NULL;
    double value;

    if (!spec || !out_value) {
        return 0;
    }

    value = strtod(spec, &endptr);
    if (endptr == spec || *endptr != '\0' || value < 0.0 || value > 1.0) {
        return 0;
    }

    *out_value = value;
    return 1;
}

static int read_file_bytes(const char *path, uint8_t **out_data, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    long size;
    uint8_t *data = NULL;

    if (!fp || !out_data || !out_size) {
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
    *out_size = (size_t)size;
    return 1;
}

static int write_file_bytes(const char *path, const uint8_t *data, size_t size) {
    FILE *fp = fopen(path, "wb");

    if (!fp) {
        return 0;
    }

    if (size > 0 && fwrite(data, 1, size, fp) != size) {
        fclose(fp);
        return 0;
    }

    return fclose(fp) == 0;
}

static uint32_t stripe_count_from_header(const helix_header_t *header) {
    if (!header || header->parity_data_oligos == 0U) {
        return 0U;
    }

    return (header->oligo_count + header->parity_data_oligos - 1U) / header->parity_data_oligos;
}

static uint32_t physical_oligo_count_from_header(const helix_header_t *header) {
    if (!header || !header->is_indexed) {
        return 0U;
    }

    if (header->version >= 4U) {
        uint32_t stripe_count = stripe_count_from_header(header);
        uint32_t parity_per_stripe = (header->parity_oligos_per_stripe > 0U) ? header->parity_oligos_per_stripe : 1U;
        return header->oligo_count + (stripe_count * parity_per_stripe);
    }

    return header->oligo_count;
}

static uint32_t stripe_members_from_header(const helix_header_t *header, uint32_t stripe_index) {
    uint32_t stripe_start;
    uint32_t remaining;

    if (!header || header->parity_data_oligos == 0U) {
        return 0U;
    }

    stripe_start = stripe_index * header->parity_data_oligos;
    if (stripe_start >= header->oligo_count) {
        return 0U;
    }

    remaining = header->oligo_count - stripe_start;
    if (remaining > header->parity_data_oligos) {
        remaining = header->parity_data_oligos;
    }

    return remaining;
}

static int load_header_from_path(const char *path, helix_header_t *out_header) {
    FILE *fp = fopen(path, "rb");
    helix_status_t st;

    if (!fp || !out_header) {
        if (fp) fclose(fp);
        return 0;
    }

    st = helix_read_header_info(fp, out_header);
    fclose(fp);
    return st == HELIX_OK;
}

static int structural_layout_is_valid(const helix_header_t *header, size_t size) {
    size_t body_size;

    if (!header || header->header_size == 0U || size < header->header_size) {
        return 0;
    }

    body_size = size - header->header_size;
    return (body_size % HELIX_OLIGO_LINE_CHARS) == 0U;
}

static int swap_oligo_lines(uint8_t *body, uint32_t a, uint32_t b) {
    uint8_t tmp[HELIX_OLIGO_LINE_CHARS];

    if (!body) {
        return 0;
    }

    memcpy(tmp, body + ((size_t)a * HELIX_OLIGO_LINE_CHARS), HELIX_OLIGO_LINE_CHARS);
    memcpy(
        body + ((size_t)a * HELIX_OLIGO_LINE_CHARS),
        body + ((size_t)b * HELIX_OLIGO_LINE_CHARS),
        HELIX_OLIGO_LINE_CHARS
    );
    memcpy(body + ((size_t)b * HELIX_OLIGO_LINE_CHARS), tmp, HELIX_OLIGO_LINE_CHARS);
    return 1;
}

static int remove_oligo_line_from_buffer(
    uint8_t **io_data,
    size_t *io_size,
    uint32_t header_size,
    uint32_t remove_index
) {
    uint8_t *data;
    size_t size;
    size_t line_offset;

    if (!io_data || !*io_data || !io_size || *io_size < header_size) {
        return 0;
    }

    data = *io_data;
    size = *io_size;
    line_offset = (size_t)header_size + ((size_t)remove_index * HELIX_OLIGO_LINE_CHARS);

    memmove(
        data + line_offset,
        data + line_offset + HELIX_OLIGO_LINE_CHARS,
        size - line_offset - HELIX_OLIGO_LINE_CHARS
    );

    *io_size = size - HELIX_OLIGO_LINE_CHARS;
    return 1;
}

static int replace_oligo_line_in_buffer(
    uint8_t *data,
    size_t size,
    uint32_t header_size,
    uint32_t dst_index,
    uint32_t src_index
) {
    uint8_t line_copy[HELIX_OLIGO_LINE_CHARS];
    size_t body_size;
    uint32_t line_count;

    if (!data || size < header_size) {
        return 0;
    }

    body_size = size - header_size;
    if ((body_size % HELIX_OLIGO_LINE_CHARS) != 0U) {
        return 0;
    }

    line_count = (uint32_t)(body_size / HELIX_OLIGO_LINE_CHARS);
    if (dst_index >= line_count || src_index >= line_count) {
        return 0;
    }

    memcpy(line_copy, data + header_size + ((size_t)src_index * HELIX_OLIGO_LINE_CHARS), HELIX_OLIGO_LINE_CHARS);
    memcpy(data + header_size + ((size_t)dst_index * HELIX_OLIGO_LINE_CHARS), line_copy, HELIX_OLIGO_LINE_CHARS);
    return 1;
}

static int apply_shuffle_mutation(
    uint8_t *data,
    size_t size,
    const helix_header_t *header,
    helix_rng_t *rng,
    long *out_mutated_oligos
) {
    size_t body_size;
    uint32_t line_count;
    uint8_t *body;

    if (!data || !header || !rng || !out_mutated_oligos || !structural_layout_is_valid(header, size)) {
        return 0;
    }

    body_size = size - header->header_size;
    line_count = (uint32_t)(body_size / HELIX_OLIGO_LINE_CHARS);
    if (line_count < 2U) {
        return 0;
    }

    body = data + header->header_size;
    for (uint32_t i = line_count - 1U; i > 0U; i--) {
        uint32_t j = rng_next_index(rng, i + 1U);
        if (i != j) {
            (void)swap_oligo_lines(body, i, j);
        }
    }

    *out_mutated_oligos = (long)line_count;
    return 1;
}

static int apply_drop1_mutation(
    uint8_t **io_data,
    size_t *io_size,
    const helix_header_t *header,
    helix_rng_t *rng,
    long *out_mutated_oligos
) {
    uint32_t data_count;
    uint32_t remove_index;

    if (!io_data || !io_size || !header || !rng || !out_mutated_oligos || header->version < 3U) {
        return 0;
    }

    data_count = header->oligo_count;
    if (data_count == 0U) {
        return 0;
    }

    remove_index = rng_next_index(rng, data_count);
    if (!remove_oligo_line_from_buffer(io_data, io_size, header->header_size, remove_index)) {
        return 0;
    }

    *out_mutated_oligos = 1;
    return 1;
}

static int apply_drop_parity_mutation(
    uint8_t **io_data,
    size_t *io_size,
    const helix_header_t *header,
    helix_rng_t *rng,
    long *out_mutated_oligos
) {
    uint32_t stripe_count;
    uint32_t stripe_index;
    uint32_t physical_index = 0U;
    uint32_t parity_per_stripe;

    if (!io_data || !io_size || !header || !rng || !out_mutated_oligos || header->version < 4U) {
        return 0;
    }

    stripe_count = stripe_count_from_header(header);
    parity_per_stripe = (header->parity_oligos_per_stripe > 0U) ? header->parity_oligos_per_stripe : 1U;
    if (stripe_count == 0U) {
        return 0;
    }

    stripe_index = rng_next_index(rng, stripe_count);
    for (uint32_t s = 0; s < stripe_index; s++) {
        physical_index += stripe_members_from_header(header, s) + parity_per_stripe;
    }
    physical_index += stripe_members_from_header(header, stripe_index);
    physical_index += rng_next_index(rng, parity_per_stripe);

    if (!remove_oligo_line_from_buffer(io_data, io_size, header->header_size, physical_index)) {
        return 0;
    }

    *out_mutated_oligos = 1;
    return 1;
}

static int apply_duplicate_replace_mutation(
    uint8_t *data,
    size_t size,
    const helix_header_t *header,
    helix_rng_t *rng,
    long *out_mutated_oligos
) {
    uint32_t stripe_count;
    uint32_t candidates[1024];
    uint32_t candidate_count = 0U;
    uint32_t stripe_index;
    uint32_t stripe_start_physical = 0U;
    uint32_t members;
    uint32_t src_member;
    uint32_t dst_member;
    uint32_t parity_per_stripe;

    if (!data || !header || !rng || !out_mutated_oligos || header->version < 4U) {
        return 0;
    }

    stripe_count = stripe_count_from_header(header);
    parity_per_stripe = (header->parity_oligos_per_stripe > 0U) ? header->parity_oligos_per_stripe : 1U;
    if (stripe_count == 0U) {
        return 0;
    }

    for (uint32_t s = 0; s < stripe_count; s++) {
        if (stripe_members_from_header(header, s) >= 2U) {
            if (candidate_count >= (uint32_t)(sizeof(candidates) / sizeof(candidates[0]))) {
                break;
            }
            candidates[candidate_count++] = s;
        }
    }

    if (candidate_count == 0U || !structural_layout_is_valid(header, size)) {
        return 0;
    }

    stripe_index = candidates[rng_next_index(rng, candidate_count)];
    for (uint32_t s = 0; s < stripe_index; s++) {
        stripe_start_physical += stripe_members_from_header(header, s) + parity_per_stripe;
    }

    members = stripe_members_from_header(header, stripe_index);
    src_member = rng_next_index(rng, members);
    do {
        dst_member = rng_next_index(rng, members);
    } while (dst_member == src_member);

    if (!replace_oligo_line_in_buffer(
            data,
            size,
            header->header_size,
            stripe_start_physical + dst_member,
            stripe_start_physical + src_member
        )) {
        return 0;
    }

    *out_mutated_oligos = 1;
    return 1;
}

static int apply_drop2same_mutation(
    uint8_t **io_data,
    size_t *io_size,
    const helix_header_t *header,
    helix_rng_t *rng,
    long *out_mutated_oligos
) {
    uint32_t stripe_count;
    uint32_t candidates[1024];
    uint32_t candidate_count = 0U;
    uint32_t stripe_index;
    uint32_t stripe_start_physical = 0U;
    uint32_t members;
    uint32_t remove_a;
    uint32_t remove_b;
    uint32_t high;
    uint32_t low;
    uint32_t parity_per_stripe;

    if (!io_data || !io_size || !header || !rng || !out_mutated_oligos || header->version < 4U) {
        return 0;
    }

    stripe_count = stripe_count_from_header(header);
    parity_per_stripe = (header->parity_oligos_per_stripe > 0U) ? header->parity_oligos_per_stripe : 1U;
    if (stripe_count == 0U) {
        return 0;
    }

    for (uint32_t s = 0; s < stripe_count; s++) {
        if (stripe_members_from_header(header, s) >= 2U) {
            if (candidate_count >= (uint32_t)(sizeof(candidates) / sizeof(candidates[0]))) {
                break;
            }
            candidates[candidate_count++] = s;
        }
    }

    if (candidate_count == 0U) {
        return 0;
    }

    stripe_index = candidates[rng_next_index(rng, candidate_count)];
    for (uint32_t s = 0; s < stripe_index; s++) {
        stripe_start_physical += stripe_members_from_header(header, s) + parity_per_stripe;
    }

    members = stripe_members_from_header(header, stripe_index);
    remove_a = rng_next_index(rng, members);
    do {
        remove_b = rng_next_index(rng, members);
    } while (remove_b == remove_a);

    high = remove_a > remove_b ? remove_a : remove_b;
    low = remove_a > remove_b ? remove_b : remove_a;

    if (!remove_oligo_line_from_buffer(io_data, io_size, header->header_size, stripe_start_physical + high) ||
        !remove_oligo_line_from_buffer(io_data, io_size, header->header_size, stripe_start_physical + low)) {
        return 0;
    }

    *out_mutated_oligos = 2;
    return 1;
}

static int apply_structural_mutation(
    const char *encoded_path,
    const char *mutated_path,
    const char *mutation_spec,
    const char *seed_str,
    long *out_mutated_oligos
) {
    helix_header_t header;
    uint8_t *data = NULL;
    size_t size = 0U;
    helix_rng_t rng;
    int auto_seed = 0;
    int ok = 0;

    if (!encoded_path || !mutated_path || !mutation_spec || !out_mutated_oligos) {
        return 0;
    }

    if (!load_header_from_path(encoded_path, &header) || !header.is_indexed) {
        return 0;
    }

    if (!read_file_bytes(encoded_path, &data, &size)) {
        return 0;
    }

    rng_seed(&rng, seed_str, &auto_seed);

    if (strcmp(mutation_spec, "shuffle") == 0) {
        ok = apply_shuffle_mutation(data, size, &header, &rng, out_mutated_oligos);
    } else if (strcmp(mutation_spec, "drop1") == 0) {
        ok = apply_drop1_mutation(&data, &size, &header, &rng, out_mutated_oligos);
    } else if (strcmp(mutation_spec, "drop_parity") == 0) {
        ok = apply_drop_parity_mutation(&data, &size, &header, &rng, out_mutated_oligos);
    } else if (strcmp(mutation_spec, "duplicate_replace") == 0) {
        ok = apply_duplicate_replace_mutation(data, size, &header, &rng, out_mutated_oligos);
    } else if (strcmp(mutation_spec, "drop2same") == 0) {
        ok = apply_drop2same_mutation(&data, &size, &header, &rng, out_mutated_oligos);
    }

    if (!ok) {
        free(data);
        return 0;
    }

    ok = write_file_bytes(mutated_path, data, size);
    free(data);
    return ok;
}

static int calculate_mutation_stats(
    const char *encoded_path,
    const char *mutated_path,
    long *out_total_bases,
    long *out_mutated_bases,
    double *out_actual_rate
) {
    FILE *fe = fopen(encoded_path, "rb");
    FILE *fm = fopen(mutated_path, "rb");
    helix_header_t encoded_header;
    helix_header_t mutated_header;
    long total_bases = 0;
    long mutated_bases = 0;
    int ok = 0;

    if (!fe || !fm || !out_total_bases || !out_mutated_bases || !out_actual_rate) {
        if (fe) fclose(fe);
        if (fm) fclose(fm);
        return 0;
    }

    if (helix_read_header_info(fe, &encoded_header) != HELIX_OK ||
        helix_read_header_info(fm, &mutated_header) != HELIX_OK) {
        goto cleanup;
    }

    if (encoded_header.header_size != mutated_header.header_size) {
        goto cleanup;
    }

    while (1) {
        int ce = fgetc(fe);
        int cm = fgetc(fm);

        if (ce == EOF || cm == EOF) {
            ok = (ce == EOF && cm == EOF);
            break;
        }

        if ((ce == 'A' || ce == 'C' || ce == 'G' || ce == 'T') &&
            (cm == 'A' || cm == 'C' || cm == 'G' || cm == 'T')) {
            total_bases++;
            if (ce != cm) {
                mutated_bases++;
            }
        }
    }

cleanup:
    fclose(fe);
    fclose(fm);

    if (!ok) {
        return 0;
    }

    *out_total_bases = total_bases;
    *out_mutated_bases = mutated_bases;
    *out_actual_rate = (total_bases > 0) ? ((double)mutated_bases / (double)total_bases) : 0.0;
    return 1;
}

static void print_usage(const char *prog) {
    printf("Uso: %s <input> <encoded.dna> <mutated.dna> <decoded.bin> <mutation_spec> <results.csv> [seed]\n", prog);
    printf("Exemplos:\n");
    printf("%s data.bin out.dna out_mut.dna out.bin 0.01 bench.csv 12345\n", prog);
    printf("%s data.bin out.dna out_mut.dna out.bin shuffle bench.csv 12345\n", prog);
    printf("%s data.bin out.dna out_mut.dna out.bin drop1 bench.csv 12345\n", prog);
    printf("Modos estruturais: shuffle, drop1, drop_parity, duplicate_replace, drop2same\n");
    printf("Sem seed numerica, uma seed aleatoria e gerada. Use -noseed para fluxo nao deterministico.\n");
}

int main(int argc, char *argv[]) {
    const char *input_path;
    const char *encoded_path;
    const char *mutated_path;
    const char *decoded_path;
    const char *mutation_spec;
    const char *csv_path;
    char auto_seed_buf[32];
    const char *seed_str = NULL;
    long input_size;
    char cmd[2048];
    int encode_exit = -1;
    int mutate_exit = -1;
    int decode_exit = -1;
    int bit_perfect;
    int auto_seed = 0;
    int is_substitution = 0;
    double requested_rate = -1.0;
    long total_bases = -1;
    long mutated_bases = -1;
    double actual_rate = -1.0;
    long structural_oligos = -1;
    long structural_total_oligos = -1;
    long encoded_size;
    long mutated_size;
    long decoded_size;
    double t0;
    double t1;
    double t2;
    double t3;
    FILE *csv;
    long csv_size;

    if (argc != 7 && argc != 8) {
        print_usage(argv[0]);
        return 1;
    }

    input_path = argv[1];
    encoded_path = argv[2];
    mutated_path = argv[3];
    decoded_path = argv[4];
    mutation_spec = argv[5];
    csv_path = argv[6];

    if (argc == 8) {
        seed_str = argv[7];
    } else {
        snprintf(auto_seed_buf, sizeof(auto_seed_buf), "%u", generate_auto_seed());
        seed_str = auto_seed_buf;
        auto_seed = 1;
    }

    is_substitution = mutation_spec_is_numeric(mutation_spec, &requested_rate);

    input_size = get_file_size(input_path);
    if (input_size < 0) {
        printf("Erro: nao foi possivel ler o arquivo de entrada.\n");
        return 1;
    }

    t0 = now_seconds();
    snprintf(cmd, sizeof(cmd), HELIX_BIN " enc \"%s\" \"%s\"", input_path, encoded_path);
    encode_exit = run_command(cmd);
    t1 = now_seconds();

    t2 = t1;
    if (encode_exit == 0) {
        if (is_substitution) {
            if (strcmp(seed_str, "-noseed") != 0) {
                snprintf(
                    cmd,
                    sizeof(cmd),
                    HELIX_MUTATE_BIN " \"%s\" \"%s\" %s %s",
                    encoded_path,
                    mutated_path,
                    mutation_spec,
                    seed_str
                );
            } else {
                snprintf(
                    cmd,
                    sizeof(cmd),
                    HELIX_MUTATE_BIN " \"%s\" \"%s\" %s -noseed",
                    encoded_path,
                    mutated_path,
                    mutation_spec
                );
            }

            mutate_exit = run_command(cmd);
        } else {
            helix_header_t encoded_header;

            if (load_header_from_path(encoded_path, &encoded_header)) {
                structural_total_oligos = (long)physical_oligo_count_from_header(&encoded_header);
            }

            mutate_exit = apply_structural_mutation(
                encoded_path,
                mutated_path,
                mutation_spec,
                seed_str,
                &structural_oligos
            ) ? 0 : 1;

            if (mutate_exit != 0) {
                printf("Falha ao aplicar mutacao estrutural: %s\n", mutation_spec);
            } else if (structural_total_oligos > 0 && structural_oligos >= 0) {
                actual_rate = (double)structural_oligos / (double)structural_total_oligos;
            }
        }
    }
    t2 = now_seconds();

    if (encode_exit == 0 && mutate_exit == 0 && is_substitution) {
        (void)calculate_mutation_stats(
            encoded_path,
            mutated_path,
            &total_bases,
            &mutated_bases,
            &actual_rate
        );
    }

    t3 = t2;
    if (encode_exit == 0 && mutate_exit == 0) {
        snprintf(cmd, sizeof(cmd), HELIX_BIN " dec \"%s\" \"%s\"", mutated_path, decoded_path);
        decode_exit = run_command(cmd);
    }
    t3 = now_seconds();

    bit_perfect = (decode_exit == 0) ? compare_result_is_equal(input_path, decoded_path) : 0;

    encoded_size = get_file_size(encoded_path);
    mutated_size = get_file_size(mutated_path);
    decoded_size = get_file_size(decoded_path);

    csv = fopen(csv_path, "a+");
    if (!csv) {
        printf("Erro: nao foi possivel abrir CSV de resultados.\n");
        return 1;
    }

    fseek(csv, 0, SEEK_END);
    csv_size = ftell(csv);

    if (csv_size == 0) {
        fprintf(
            csv,
            "input_file,input_bytes,encoded_bytes,mutated_bytes,decoded_bytes,"
            "mutation_spec,total_units,affected_units,measured_rate,seed,"
            "encode_time_sec,mutate_time_sec,decode_time_sec,"
            "encode_exit,mutate_exit,decode_exit,bit_perfect\n"
        );
    }

    fprintf(
        csv,
        "\"%s\",%ld,%ld,%ld,%ld,\"%s\",",
        input_path,
        input_size,
        encoded_size,
        mutated_size,
        decoded_size,
        mutation_spec
    );

    if (is_substitution) {
        fprintf(csv, "%ld,%ld,%.6f,", total_bases, mutated_bases, actual_rate);
    } else {
        if (structural_total_oligos >= 0) {
            fprintf(csv, "%ld,%ld,%.6f,", structural_total_oligos, structural_oligos, actual_rate);
        } else {
            fprintf(csv, ",%ld,,", structural_oligos);
        }
    }

    fprintf(
        csv,
        "\"%s\",%.6f,%.6f,%.6f,%d,%d,%d,%d\n",
        seed_str,
        (t1 - t0),
        (t2 - t1),
        (t3 - t2),
        encode_exit,
        mutate_exit,
        decode_exit,
        bit_perfect
    );

    fclose(csv);

    printf("Benchmark concluido.\n");
    printf("Input bytes    : %ld\n", input_size);
    printf("Encoded bytes  : %ld\n", encoded_size);
    printf("Mutated bytes  : %ld\n", mutated_size);
    printf("Decoded bytes  : %ld\n", decoded_size);
    printf("Mutation spec  : %s\n", mutation_spec);
    if (is_substitution) {
        printf("Bases totais   : %ld\n", total_bases);
        printf("Bases mutadas  : %ld\n", mutated_bases);
        printf("Taxa real      : %.6f\n", actual_rate);
    } else {
        printf("Oligos afetados: %ld\n", structural_oligos);
    }
    printf("Encode time    : %.6f s\n", (t1 - t0));
    printf("Mutate time    : %.6f s\n", (t2 - t1));
    printf("Decode time    : %.6f s\n", (t3 - t2));
    printf("Encode exit    : %d\n", encode_exit);
    printf("Mutate exit    : %d\n", mutate_exit);
    printf("Decode exit    : %d\n", decode_exit);
    printf("Bit-perfect    : %s\n", bit_perfect ? "SIM" : "NAO");
    printf("Seed           : %s%s\n", seed_str, auto_seed ? " (auto)" : "");

    return (encode_exit == 0 && mutate_exit == 0 && decode_exit == 0 && bit_perfect) ? 0 : 1;
}
