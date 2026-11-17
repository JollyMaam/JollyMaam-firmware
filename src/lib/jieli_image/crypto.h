#ifndef JM_JIELI_CRYPTO_H
#define JM_JIELI_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

uint16_t jm_crc16_seed(const uint8_t *data, size_t size, uint16_t seed);
uint16_t jm_crc16(const uint8_t *data, size_t size);

void jm_jieli_enc(uint8_t *data, size_t size, uint16_t key);

void jm_jieli_sfc(uint8_t *image, uint32_t offset, uint32_t size,
                  uint32_t base, uint16_t key);

#endif
