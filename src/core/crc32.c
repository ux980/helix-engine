#include "helix/crc32.h"

static uint32_t crc32_table[256];
static int crc32_initialized = 0;

static void crc32_init_table(void) {
    if (crc32_initialized) {
        return;
    }

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;

        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1U) {
                crc = (crc >> 1) ^ UINT32_C(0xEDB88320);
            } else {
                crc >>= 1;
            }
        }

        crc32_table[i] = crc;
    }

    crc32_initialized = 1;
}

uint32_t helix_crc32_init(void) {
    crc32_init_table();
    return UINT32_C(0xFFFFFFFF);
}

uint32_t helix_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    crc32_init_table();

    if (!data && len > 0) {
        return crc;
    }

    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    }

    return crc;
}

uint32_t helix_crc32_finish(uint32_t crc) {
    return crc ^ UINT32_C(0xFFFFFFFF);
}

uint32_t helix_crc32(const uint8_t *data, size_t len) {
    return helix_crc32_finish(helix_crc32_update(helix_crc32_init(), data, len));
}
