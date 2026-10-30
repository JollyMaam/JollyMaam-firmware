#ifndef JM_JIELI_CRYPTO_H
#define JM_JIELI_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/* CRC-CCITT, non-reflected. A zero seed is the image CRC convention. */
uint16_t jm_crc16_seed(const uint8_t *data, size_t size, uint16_t seed);
uint16_t jm_crc16(const uint8_t *data, size_t size);

/* In-place symmetric ENC transform. Each call starts a new stream. */
void jm_jieli_enc(uint8_t *data, size_t size, uint16_t key);

/* In-place flash-controller transform. The caller supplies a buffer covering
 * offset..offset+size. offset-base must be a multiple of 32 and offset >= base.
 * A final short block is allowed. No allocation or device access occurs. */
void jm_jieli_sfc(uint8_t *image, uint32_t offset, uint32_t size,
                  uint32_t base, uint16_t key);

#endif
