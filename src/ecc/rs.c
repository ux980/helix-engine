#include <string.h>
#include "helix/rs.h"
#include "helix/gf256.h"

static uint8_t g_gen_poly[HELIX_RS_T + 1];
static int g_rs_initialized = 0;

static void rs_compute_generator(void) {
    memset(g_gen_poly, 0, sizeof(g_gen_poly));
    g_gen_poly[0] = 1;

    for (int i = 0; i < HELIX_RS_T; i++) {
        uint8_t next[HELIX_RS_T + 1];
        memset(next, 0, sizeof(next));

        for (int j = 0; j <= i; j++) {
            next[j] ^= g_gen_poly[j];
            next[j + 1] ^= gf256_mul(g_gen_poly[j], gf256_pow_exp(i));
        }

        memcpy(g_gen_poly, next, sizeof(g_gen_poly));
    }
}

void rs_init(void) {
    if (g_rs_initialized) {
        return;
    }

    gf256_init();
    rs_compute_generator();
    g_rs_initialized = 1;
}

void rs_encode_block(const uint8_t *data, uint8_t *parity) {
    uint8_t msg[HELIX_RS_N];

    memcpy(msg, data, HELIX_RS_K);
    memset(msg + HELIX_RS_K, 0, HELIX_RS_T);

    for (int i = 0; i < HELIX_RS_K; i++) {
        uint8_t feedback = msg[i];
        if (feedback != 0) {
            for (int j = 1; j <= HELIX_RS_T; j++) {
                msg[i + j] ^= gf256_mul(g_gen_poly[j], feedback);
            }
        }
    }

    memcpy(parity, msg + HELIX_RS_K, HELIX_RS_T);
}

int rs_calc_syndromes(const uint8_t *block, uint8_t *syn) {
    int has_errors = 0;

    for (int i = 0; i < HELIX_RS_T; i++) {
        uint8_t s = 0;

        for (int j = 0; j < HELIX_RS_N; j++) {
            s = gf256_mul(s, gf256_pow_exp(i)) ^ block[j];
        }

        syn[i] = s;
        if (s != 0) {
            has_errors = 1;
        }
    }

    return has_errors;
}

int rs_correct_errors(uint8_t *block, const uint8_t *syn) {
    uint8_t lambda[HELIX_RS_T + 1];
    uint8_t b[HELIX_RS_T + 1];
    uint8_t omega[HELIX_RS_T];
    int err_pos[HELIX_RS_MAX_ERR];

    memset(lambda, 0, sizeof(lambda));
    memset(b, 0, sizeof(b));
    memset(omega, 0, sizeof(omega));

    lambda[0] = 1;
    b[0] = 1;

    int L = 0;
    int m = 1;
    uint8_t b_inv = 1;

    for (int n = 0; n < HELIX_RS_T; n++) {
        uint8_t d = syn[n];

        for (int i = 1; i <= L; i++) {
            d ^= gf256_mul(lambda[i], syn[n - i]);
        }

        if (d == 0) {
            m++;
        } else {
            uint8_t old_lambda[HELIX_RS_T + 1];
            memcpy(old_lambda, lambda, sizeof(old_lambda));

            uint8_t scale = gf256_mul(d, b_inv);

            for (int i = 0; i <= HELIX_RS_T - m; i++) {
                if (b[i] != 0) {
                    lambda[i + m] ^= gf256_mul(scale, b[i]);
                }
            }

            if ((2 * L) <= n) {
                L = n + 1 - L;
                memcpy(b, old_lambda, sizeof(old_lambda));
                b_inv = gf256_div(1, d);
                m = 1;
            } else {
                m++;
            }
        }
    }

    int count = 0;
    for (int i = 0; i < HELIX_RS_N; i++) {
        uint8_t sum = 0;

        for (int j = 0; j <= L; j++) {
            sum ^= gf256_mul(lambda[j], gf256_pow_exp(((255 - i) * j) % 255));
        }

        if (sum == 0) {
            if (count >= HELIX_RS_MAX_ERR) {
                break;
            }
            err_pos[count++] = HELIX_RS_N - 1 - i;
        }
    }

    if (count == 0 || count > L) {
        return 0;
    }

    for (int i = 0; i < HELIX_RS_T; i++) {
        omega[i] = syn[i];
        for (int j = 1; j <= i && j <= L; j++) {
            omega[i] ^= gf256_mul(lambda[j], syn[i - j]);
        }
    }

    for (int i = 0; i < count; i++) {
        int pos = err_pos[i];
        int poly_pos = HELIX_RS_N - 1 - pos;
        uint8_t num = 0;
        uint8_t den = 0;

        for (int j = 0; j < L; j++) {
            num ^= gf256_mul(omega[j], gf256_pow_exp((-poly_pos * j) % 255));
        }

        for (int j = 1; j <= L; j += 2) {
            den ^= gf256_mul(lambda[j], gf256_pow_exp((-poly_pos * (j - 1)) % 255));
        }

        if (den == 0) {
            return 0;
        }

        block[pos] ^= gf256_div(gf256_mul(num, gf256_pow_exp(poly_pos)), den);
    }

    {
        uint8_t check_syn[HELIX_RS_T];
        if (rs_calc_syndromes(block, check_syn) != 0) {
            return 0;
        }
    }

    return 1;
}
