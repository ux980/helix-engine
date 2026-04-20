#include "helix/gf256.h"

static uint8_t g_exp[512];
static uint8_t g_log[256];
static int g_initialized = 0;

void gf256_init(void) {
    if (g_initialized) {
        return;
    }

    int b = 1;
    g_log[0] = 0;

    for (int i = 0; i < 255; i++) {
        g_exp[i] = (uint8_t)b;
        g_log[b] = (uint8_t)i;
        b <<= 1;
        if (b & 0x100) {
            b ^= 0x11D;
        }
    }

    for (int i = 255; i < 512; i++) {
        g_exp[i] = g_exp[i - 255];
    }

    g_initialized = 1;
}

uint8_t gf256_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    return g_exp[g_log[a] + g_log[b]];
}

uint8_t gf256_div(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }

    int idx = (int)g_log[a] - (int)g_log[b];
    if (idx < 0) {
        idx += 255;
    }
    return g_exp[idx];
}

uint8_t gf256_pow_exp(int index) {
    while (index < 0) {
        index += 255;
    }
    while (index >= 255) {
        index -= 255;
    }
    return g_exp[index];
}