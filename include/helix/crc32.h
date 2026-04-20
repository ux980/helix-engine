#ifndef HELIX_CRC32_H
#define HELIX_CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t helix_crc32_init(void);
uint32_t helix_crc32_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t helix_crc32_finish(uint32_t crc);
uint32_t helix_crc32(const uint8_t *data, size_t len);

#endif
