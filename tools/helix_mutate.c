#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "helix/config.h"

static const char BASES[4] = { 'A', 'C', 'G', 'T' };

typedef struct {
    uint64_t state;
} helix_rng_t;

static unsigned int generate_auto_seed(void) {
    int local = 0;
    uintptr_t stack_addr = (uintptr_t)&local;
    return (unsigned int)time(NULL) ^ (unsigned int)clock() ^ (unsigned int)(stack_addr >> 4);
}

static uint64_t rng_next_u64(helix_rng_t *rng) {
    uint64_t z = (rng->state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static double rng_next_unit(helix_rng_t *rng) {
    return (double)(rng_next_u64(rng) >> 11) * (1.0 / 9007199254740992.0);
}

static int rng_next_index(helix_rng_t *rng, int limit) {
    return (int)(rng_next_u64(rng) % (uint64_t)limit);
}

static char random_different_base(helix_rng_t *rng, char current) {
    char candidate;
    do {
        candidate = BASES[rng_next_index(rng, 4)];
    } while (candidate == current);
    return candidate;
}

int main(int argc, char *argv[]) {
    if (argc != 4 && argc != 5) {
        printf("Uso: %s <input.dna> <output_mutated.dna> <substitution_rate> [seed]\n", argv[0]);
        printf("Exemplo: %s sample.dna sample_mut.dna 0.01 12345\n", argv[0]);
        printf("Sem seed numerica, uma seed aleatoria e gerada. Use -noseed para nao chamar srand().\n");
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = argv[2];
    double substitution_rate = atof(argv[3]);
    unsigned int seed = 0;
    int use_seed = 1;
    int auto_seed = 1;
    helix_rng_t rng;

    if (argc == 5) {
        if (strcmp(argv[4], "-noseed") == 0) {
            use_seed = 0;
            auto_seed = 0;
        } else {
            seed = (unsigned int)strtoul(argv[4], NULL, 10);
            auto_seed = 0;
        }
    } else {
        seed = generate_auto_seed();
    }

    if (substitution_rate < 0.0 || substitution_rate > 1.0) {
        printf("Erro: substitution_rate deve estar entre 0.0 e 1.0\n");
        return 1;
    }

    FILE *fi = fopen(input_path, "rb");
    FILE *fo = fopen(output_path, "wb");

    if (!fi || !fo) {
        printf("Erro: nao foi possivel abrir os arquivos.\n");
        if (fi) fclose(fi);
        if (fo) fclose(fo);
        return 1;
    }

    if (use_seed) {
        rng.state = UINT64_C(0xA0761D6478BD642F) ^ (uint64_t)seed;
    } else {
        rng.state = UINT64_C(0xE7037ED1A0B428DB);
    }

    char header[HELIX_HEADER_V5_SIZE];
    size_t magic_len = strlen(HELIX_MAGIC_V1);
    size_t header_size = 0;

    if (fread(header, 1, magic_len, fi) != magic_len) {
        printf("Erro: arquivo invalido ou header incompleto.\n");
        fclose(fi);
        fclose(fo);
        return 1;
    }

    if (memcmp(header, HELIX_MAGIC_V5, magic_len) == 0) {
        header_size = HELIX_HEADER_V5_SIZE;
    } else if (memcmp(header, HELIX_MAGIC_V4, magic_len) == 0) {
        header_size = HELIX_HEADER_V4_SIZE;
    } else if (memcmp(header, HELIX_MAGIC_V3, magic_len) == 0) {
        header_size = HELIX_HEADER_V3_SIZE;
    } else if (memcmp(header, HELIX_MAGIC_V2, magic_len) == 0) {
        header_size = HELIX_HEADER_V2_SIZE;
    } else if (memcmp(header, HELIX_MAGIC_V1, magic_len) == 0) {
        header_size = HELIX_HEADER_V1_SIZE;
    } else {
        printf("Erro: cabecalho HELIX invalido.\n");
        fclose(fi);
        fclose(fo);
        return 1;
    }

    if (fread(header + magic_len, 1, header_size - magic_len, fi) != header_size - magic_len) {
        printf("Erro: arquivo invalido ou header incompleto.\n");
        fclose(fi);
        fclose(fo);
        return 1;
    }

    if (fwrite(header, 1, header_size, fo) != header_size) {
        printf("Erro ao gravar header.\n");
        fclose(fi);
        fclose(fo);
        return 1;
    }

    long total_bases = 0;
    long mutated_bases = 0;

    int c;
    while ((c = fgetc(fi)) != EOF) {
        char base = (char)c;

        if (base == 'A' || base == 'C' || base == 'G' || base == 'T') {
            total_bases++;

            double r = rng_next_unit(&rng);
            if (r < substitution_rate) {
                base = random_different_base(&rng, base);
                mutated_bases++;
            }
        }

        if (fputc(base, fo) == EOF) {
            printf("Erro ao escrever arquivo mutado.\n");
            fclose(fi);
            fclose(fo);
            return 1;
        }
    }

    fclose(fi);
    fclose(fo);

    printf("Mutacao concluida.\n");
    printf("Bases totais   : %ld\n", total_bases);
    printf("Bases mutadas  : %ld\n", mutated_bases);
    printf("Taxa solicitada: %.6f\n", substitution_rate);
    if (use_seed) {
        printf("Seed           : %u%s\n", seed, auto_seed ? " (auto)" : "");
    } else {
        printf("Seed           : -noseed\n");
    }

    if (total_bases > 0) {
        printf("Taxa real      : %.6f\n", (double)mutated_bases / (double)total_bases);
    }

    return 0;
}
